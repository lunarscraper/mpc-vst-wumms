/* =============================================================================
 * wumms_vst.cpp - Wumms: an end-of-chain warming unit in the manner of the OTO Boum as a VST2
 * insert effect for the MPC OS plugin host (Force, MPC Live/One/X/Key), armhf. The DSP is
 * wumms_core.h: in gain, gate, one-knob compressor, low cut, four distortion types, high cut,
 * dry/wet mix, level. This file is the plug-in around it: the shell of mpc-vst-vfilter
 * (hand-written, no wrapper, no SDK) with the 32 preset slots and LOAD/SAVE of mpc-vst-rattler.
 * MIT license (see ../LICENSE). "OTO" and "Boum" belong to their owners; no affiliation.
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "wumms_core.h"

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[2]; } VstEvents;

enum {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4,
    effGetProgramName = 5, effGetProgramNameIndexed = 29, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10, kVstTimeSigValid = 1 << 13 };
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

/* the parameters, by key (module.json); IDX[] is their position in the generated PARAMS[] */
enum { GAIN, COMP, ATTACK, RELEASE, DRIVE, DISTO, MIX, LEVEL, LOCUT, HICUT, GATE, SLOT, LOAD, SAVE, NKEYS };
static const char *const KEYS[NKEYS] = {
    "gain", "comp", "attack", "release", "drive", "disto", "mix", "level", "locut", "hicut", "gate",
    "slot", "load", "save",
};
static int IDX[NKEYS];
static int KEY_OF[NPARAMS];   /* PARAMS[] position -> key enum, -1 = not ours */
/* slot, load and save belong to the preset bank: never part of a chunk or a preset */
static bool is_bank_key(int i) { const int k = KEY_OF[i]; return k == SLOT || k == LOAD || k == SAVE; }

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    std::atomic<float> cache[NPARAMS];
    std::atomic<int> notify[NPARAMS];
    float open[NPARAMS] = {0};
    volatile int release[NPARAMS] = {0};
    bool down[NPARAMS] = {false};   /* LOAD/SAVE: the host currently reports them pressed */
    std::atomic<bool> dirty{true};
    std::atomic<int> cur{0};        /* selected preset slot, 0-based */
    wumms::Engine engine;
    wumms::Params p;
    float sr = 44100;
    std::vector<uint8_t> chunk;
};

static int param_index(const char *key) {
    for (int i = 0; i < NPARAMS; i++) if (!std::strcmp(PARAMS[i].key, key)) return i;
    return -1;
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void copy_str(void *dst, const char *s, size_t max) {
    std::strncpy((char *)dst, s, max - 1);
    ((char *)dst)[max - 1] = 0;
}
static int norm_to_ui(const param_t *p, float n) {
    if (p->nopts) return (int)std::lround(clamp01(n) * (p->nopts - 1));
    return (int)std::lround(p->min + (p->max - p->min) * clamp01(n));
}
static float ui_to_norm(const param_t *p, double v) {
    if (p->nopts) return p->nopts > 1 ? clamp01((float)(v / (p->nopts - 1))) : 0.0f;
    return p->max > p->min ? clamp01((float)((v - p->min) / (p->max - p->min))) : 0.0f;
}
/* a knob is used at full resolution (the integer range only names its ends), a switch as its index */
static float val_at(Plugin *w, int i) {
    const param_t *p = &PARAMS[i];
    const float n = w->cache[i].load();
    if (p->nopts) return (float)norm_to_ui(p, n);
    return p->min + (p->max - p->min) * clamp01(n);
}
static float val(Plugin *w, int k) { return IDX[k] < 0 ? 0.0f : val_at(w, IDX[k]); }
static float pct(Plugin *w, int k) { return val(w, k) * 0.01f; }
static void set_val(Plugin *w, int i, double v) {
    if (i < 0) return;
    w->cache[i].store(ui_to_norm(&PARAMS[i], v));
    w->notify[i].store(1);
}
static void start(Plugin *w, int k, double v) { set_val(w, IDX[k], v); }

struct NoDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t old = 0;
    NoDenormals() { asm volatile("vmrs %0, fpscr" : "=r"(old)); asm volatile("vmsr fpscr, %0" : : "r"(old | (1u << 24))); }
    ~NoDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(old)); }
#elif defined(__x86_64__) || defined(__i386__)
    unsigned old = __builtin_ia32_stmxcsr();
    NoDenormals() { __builtin_ia32_ldmxcsr(old | 0x8040); }
    ~NoDenormals() { __builtin_ia32_ldmxcsr(old); }
#endif
};

/* knob laws, shared by configure() and the value display. ATTACK, RELEASE and HI CUT are
 * exponential and read the knob's full resolution (the 0..100 is only the scale). */
static float law_attack_ms(float x) { return 0.05f * std::pow(660.0f, x); }    /* 50 us .. 33 ms */
static float law_release_ms(float x) { return 50.0f * std::pow(20.0f, x); }    /* 50 ms .. 1 s */
static const float LOCUT_HZ[4] = {0.0f, 75.0f, 150.0f, 300.0f};

static void configure(Plugin *w) {
    wumms::Params &p = w->p;
    p.gainDb = val(w, GAIN);
    p.comp = pct(w, COMP);
    p.attackMs = law_attack_ms(pct(w, ATTACK));
    p.releaseMs = law_release_ms(pct(w, RELEASE));
    p.drive = pct(w, DRIVE);
    p.disto = clampi((int)val(w, DISTO), 0, wumms::NDISTO - 1);
    p.mix = pct(w, MIX);
    p.levelDb = val(w, LEVEL);
    p.locutHz = LOCUT_HZ[clampi((int)val(w, LOCUT), 0, 3)];
    p.hicut = pct(w, HICUT);
    const int g = clampi((int)val(w, GATE), 0, 6);
    p.gateDb = g ? -60.0f + 10.0f * g : -100.0f;      /* OFF, -50 .. 0 dB in 10 dB steps */
}

/* the start setting: everything neutral, the signal passes unchanged (slot 1, CLEAN) */
static void start_values(Plugin *w) {
    start(w, GAIN, 0); start(w, COMP, 0); start(w, ATTACK, 60); start(w, RELEASE, 45);
    start(w, DRIVE, 0); start(w, DISTO, 0); start(w, MIX, 100); start(w, LEVEL, 0);
    start(w, LOCUT, 0); start(w, HICUT, 100); start(w, GATE, 0);
}

/* ---- state as text: "key=value;" for every parameter of the sound. Used for the project chunk
 * and for the preset slots alike; restored by key, so parameters added later keep old
 * projects and presets loading (missing keys stay at their start values). ------------------ */
static std::string build_state(Plugin *w) {
    std::string t;
    char buf[96];
    for (int i = 0; i < NPARAMS; i++) {
        if (is_bank_key(i)) continue;
        std::snprintf(buf, sizeof buf, "%s=%.3f;", PARAMS[i].key, (double)val_at(w, i));
        t += buf;
    }
    return t;
}
static void apply_state(Plugin *w, const std::string &t, bool with_slot) {
    for (size_t pos = 0; pos < t.size();) {
        size_t semi = t.find(';', pos);
        if (semi == std::string::npos) break;
        std::string kv = t.substr(pos, semi - pos);
        pos = semi + 1;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = kv.substr(0, eq);
        if (key == "prog") {   /* which slot a project had selected; never loads the slot */
            const int v = std::atoi(kv.c_str() + eq + 1);
            if (with_slot && v >= 0 && v < 32) w->cur.store(v);
            continue;
        }
        const int i = param_index(key.c_str());
        if (i >= 0 && !is_bank_key(i)) set_val(w, i, std::atof(kv.c_str() + eq + 1));
    }
    w->dirty.store(true);
}

/* ---------------------------------------------------------------------------
 * Preset bank (pattern of mpc-vst-acid): NSLOTS slots shared by every Wumms instance and
 * every project, kept in one text file on the SD card (one line per saved slot:
 * "index<TAB>name<TAB>state"). The slots are also the plugin's VST programs, so the host's
 * PRESET list shows and selects them; the PRESET knob + LOAD/SAVE buttons on the PRESET tab
 * do the same from the skin. The first slots come with factory settings until SAVE overwrites
 * them (a factory setting is never written to the file).
 * ------------------------------------------------------------------------- */
#define NSLOTS 32
static std::mutex g_bank_lock;
static bool g_bank_loaded;
static std::string g_slot_chunk[NSLOTS], g_slot_name[NSLOTS], g_bank_path;

static const struct { const char *name, *state; } FACTORY[] = {
    /* keys a state leaves out stay at their start values (start_values(): everything neutral) */
    {"CLEAN", ""},
    {"MASTER GLUE", "comp=16;attack=62;release=45;drive=8;disto=0;"},
    {"GOA GLUE", "gain=2;comp=22;attack=70;release=30;drive=14;disto=1;hicut=96;level=-3;"},
    {"TECHNO PUNCH", "gain=3;comp=30;attack=78;release=22;drive=18;disto=0;level=-3;"},
    {"TUBE WARMTH", "comp=10;attack=60;release=50;drive=30;disto=1;hicut=92;level=-2;"},
    {"TAPE DARK", "comp=18;attack=55;release=55;drive=24;disto=0;hicut=84;level=-2;"},
    {"PARALLEL SMASH", "gain=6;comp=48;attack=20;release=35;drive=20;disto=0;mix=35;"},
    {"PARALLEL FUZZ", "gain=3;comp=30;attack=40;release=40;drive=45;disto=2;locut=2;hicut=82;mix=25;"},
    {"LIMIT", "comp=50;attack=10;release=40;"},
    {"PUMP", "gain=6;comp=42;attack=85;release=62;drive=10;disto=0;level=-3;"},
    {"REVERSE DUCK", "gain=6;comp=80;attack=30;release=45;level=-3;"},
    {"FUZZ BUS", "comp=25;attack=50;release=40;drive=55;disto=2;locut=1;hicut=80;level=-3;"},
    {"SQUARE CRUSH", "comp=20;drive=60;disto=3;locut=2;hicut=72;level=-3;"},
    {"LOFI RADIO", "comp=35;attack=40;release=40;drive=40;disto=1;locut=3;hicut=68;level=2;"},
    {"GATED DRIVE", "gain=3;comp=28;attack=60;release=25;drive=35;disto=0;gate=3;level=-3;"},
    {"DRUM CRUNCH", "gain=4;comp=36;attack=72;release=20;drive=38;disto=2;hicut=88;mix=60;"},
};
enum { NFACTORY = (int)(sizeof FACTORY / sizeof FACTORY[0]) };

/* Next to the plugin's own folder rather than inside it, so reinstalling the plugin folder
 * doesn't take the presets with it; inside it if the parent can't be written. No dladdr:
 * /proc/self/maps has the path. WUMMS_PRESETS overrides the place (offline test). */
static std::string so_dir() {
    std::string dir;
    if (FILE *f = std::fopen("/proc/self/maps", "r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            char *p = std::strstr(line, "/wumms.so");
            char *first = std::strchr(line, '/');
            if (!p || !first || first > p) continue;
            dir.assign(first, (size_t)(p - first));
            break;
        }
        std::fclose(f);
    }
    return dir;
}
static void bank_load_locked() {
    if (g_bank_loaded) return;
    g_bank_loaded = true;
    g_bank_path.clear();
    if (const char *env = std::getenv("WUMMS_PRESETS")) g_bank_path = env;
    else {
        std::string dir = so_dir(), cand[2];
        if (dir.empty()) dir = "/tmp";
        size_t cut = dir.rfind('/');
        cand[0] = (cut != std::string::npos && cut > 0 ? dir.substr(0, cut) : dir) + "/wumms_presets.txt";
        cand[1] = dir + "/wumms_presets.txt";
        for (const std::string &c : cand)             /* an existing file wins ... */
            if (FILE *f = std::fopen(c.c_str(), "r")) { std::fclose(f); g_bank_path = c; break; }
        for (int i = 0; i < 2 && g_bank_path.empty(); i++)   /* ... else the first place we may write */
            if (FILE *f = std::fopen(cand[i].c_str(), "a")) { std::fclose(f); g_bank_path = cand[i]; }
    }
    if (g_bank_path.empty()) return;
    if (FILE *f = std::fopen(g_bank_path.c_str(), "r")) {
        static char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            line[std::strcspn(line, "\r\n")] = 0;
            char *t1 = std::strchr(line, '\t');
            char *t2 = t1 ? std::strchr(t1 + 1, '\t') : nullptr;
            int idx = std::atoi(line);
            if (!t2 || idx < 1 || idx > NSLOTS) continue;
            *t1 = *t2 = 0;
            g_slot_name[idx - 1] = t1 + 1;
            g_slot_chunk[idx - 1] = t2 + 1;
        }
        std::fclose(f);
    }
}
static bool bank_write_locked() {   /* whole file, via a temp file so a power cut can't leave half a bank */
    if (g_bank_path.empty()) return false;
    std::string tmp = g_bank_path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    for (int i = 0; i < NSLOTS; i++)
        if (!g_slot_chunk[i].empty())
            std::fprintf(f, "%d\t%s\t%s\n", i + 1, g_slot_name[i].c_str(), g_slot_chunk[i].c_str());
    return std::fclose(f) == 0 && std::rename(tmp.c_str(), g_bank_path.c_str()) == 0;
}
static std::string slot_title(int i) {
    std::lock_guard<std::mutex> lk(g_bank_lock);
    char buf[32];
    if (g_slot_chunk[i].empty()) {
        if (i < NFACTORY) return FACTORY[i].name;
        std::snprintf(buf, sizeof buf, "%02d (empty)", i + 1);
        return buf;
    }
    if (!g_slot_name[i].empty()) return g_slot_name[i];
    std::snprintf(buf, sizeof buf, "Wumms %02d", i + 1);
    return buf;
}
static void slot_load(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    std::string c;
    { std::lock_guard<std::mutex> lk(g_bank_lock); c = g_slot_chunk[n]; }
    const bool factory = c.empty() && n < NFACTORY;
    if (factory) c = FACTORY[n].state;
    if (c.empty() && !factory) return;  /* an empty slot leaves the current setting alone */
    start_values(w);
    apply_state(w, c, false);
}
static bool slot_save(Plugin *w) {
    const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
    const std::string c = build_state(w);
    std::lock_guard<std::mutex> lk(g_bank_lock);
    g_slot_chunk[n] = c;
    return bank_write_locked();
}

static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    Plugin *w = (Plugin *)e->object;
    NoDenormals nd;
    if (w->dirty.exchange(false)) configure(w);
    if (n > 0) w->engine.process(w->p, in[0], in[1], out[0], out[1], (uint32_t)n);
    for (int c = 0; c < 2; c++) {
        float *y = out[c];
        for (int i = 0; i < n; i++) {
            const float a = std::fabs(y[i]);              /* output safety above -1.4 dBFS, ceiling 0.99 */
            if (a > 0.85f) {
                float t = std::min((a - 0.85f) / 0.14f, 3.0f), t2 = t * t;
                y[i] = std::copysign(0.85f + 0.14f * t * (27 + t2) / (27 + 9 * t2), y[i]);
            }
        }
    }
    bool any = false;
    for (int i = 0; i < NPARAMS; i++) {
        if (w->release[i]) { w->release[i] = 0; any = true; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
        if (!w->notify[i].exchange(0)) continue;
        any = true;
        w->master(&w->fx, audioMasterAutomate, i, 0, 0, w->cache[i].load());
    }
    if (any) w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (popup_set(w->open, i, n)) return;
    const int key = KEY_OF[i];
    if (key == SLOT) {   /* browsing only: LOAD loads, so turning the knob can't wipe what is playing */
        const int v = (int)std::lround(clamp01(n) * (NSLOTS - 1));
        if (v != w->cur.load()) { w->cur.store(v); w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f); }
        return;
    }
    if (key == LOAD || key == SAVE) {
        const bool down = n > 0.5f;
        const bool rising = down && !w->down[i];   /* an echo of our own automate must not fire again */
        w->down[i] = down;
        if (rising) {
            if (key == LOAD) slot_load(w); else slot_save(w);
            w->release[i] = 1;
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return;
    }
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            n = (float)clampi((int)std::lround(cur) + (pos > cur ? 1 : -1), 0, p->nopts - 1) / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(clamp01(n));
    w->dirty.store(true);
    if (!nudge) popup_picked(w->open, w->release, i);
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    const int key = KEY_OF[i];
    if (key == SLOT) return (float)w->cur.load() / (NSLOTS - 1);
    if (key == LOAD || key == SAVE) return 0.0f;   /* triggers always read released */
    if (popup_is(i)) return w->open[i];
    return w->cache[i].load();
}

/* project chunk: "WUMM1;" + the state + "prog=<slot>;" */
static intptr_t get_chunk(Plugin *w, void **ptr) {
    std::string t = "WUMM1;" + build_state(w);
    char buf[24];
    std::snprintf(buf, sizeof buf, "prog=%d;", w->cur.load());
    t += buf;
    w->chunk.assign(t.begin(), t.end());
    *ptr = w->chunk.data();
    return (intptr_t)w->chunk.size();
}
static intptr_t set_chunk(Plugin *w, const void *data, intptr_t len) {
    if (!data || len < 6) return 0;
    std::string t((const char *)data, (size_t)len);
    t.resize(std::strlen(t.c_str()));   /* stop at a NUL the host may have included */
    if (t.compare(0, 6, "WUMM1;")) return 0;
    apply_state(w, t.substr(6), true);
    return 1;
}

static void display(Plugin *w, int idx, char *buf, size_t n) {
    const param_t *pp = &PARAMS[idx];
    const int key = KEY_OF[idx];
    if (key == SLOT) { std::snprintf(buf, n, "%s", slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str()); return; }
    if (key == LOAD || key == SAVE) { buf[0] = 0; return; }
    if (popup_is(idx)) {
        const int u = norm_to_ui(pp, w->open[idx]);
        if (pp->nopts) std::snprintf(buf, n, "%s", pp->opts[u]); else std::snprintf(buf, n, "%d", u);
        return;
    }
    const int u = norm_to_ui(pp, w->cache[idx].load());
    if (pp->nopts) { std::snprintf(buf, n, "%s", pp->opts[u]); return; }
    const float v = val_at(w, idx);
    const float x = (v - pp->min) / (pp->max > pp->min ? pp->max - pp->min : 1);
    switch (key) {
    case GAIN: case LEVEL: std::snprintf(buf, n, "%+.1f dB", v); break;
    case COMP: {   /* the one knob as the ratio it stands for */
        const float s = wumms::comp_slope(x);
        if (s < 0.005f) std::snprintf(buf, n, "OFF");
        else if (s < 0.985f) std::snprintf(buf, n, "%.1f:1", 1.0f / (1.0f - s));
        else if (s < 1.015f) std::snprintf(buf, n, "LIMIT");
        else std::snprintf(buf, n, "REV %d %%", (int)std::lround((s - 1.0f) * 100));
        break;
    }
    case ATTACK: {
        const float ms = law_attack_ms(x);
        if (ms < 1) std::snprintf(buf, n, "%.2f ms", ms); else std::snprintf(buf, n, "%.1f ms", ms);
        break;
    }
    case RELEASE: std::snprintf(buf, n, "%d ms", (int)std::lround(law_release_ms(x))); break;
    case HICUT: {
        const float hz = wumms::hicut_hz(x);
        if (x >= 0.999f) std::snprintf(buf, n, "OFF");
        else if (hz < 1000) std::snprintf(buf, n, "%d Hz", (int)std::lround(hz));
        else std::snprintf(buf, n, "%.1f kHz", hz / 1000);
        break;
    }
    default: std::snprintf(buf, n, "%d %%", u); break;
    }
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: delete w; return 1;
    case effSetProgram:
        if (v >= 0 && v < NSLOTS) {
            w->cur.store((int)v);
            slot_load(w);
            w->master(&w->fx, audioMasterUpdateDisplay, 0, 0, 0, 0.0f);
        }
        return 0;
    case effGetProgram: return w->cur.load();
    case effGetProgramName: if (p) copy_str(p, slot_title(clampi(w->cur.load(), 0, NSLOTS - 1)).c_str(), 24); return 0;
    case effGetProgramNameIndexed:
        if (idx < 0 || idx >= NSLOTS || !p) return 0;
        copy_str(p, slot_title(idx).c_str(), 24);
        return 1;
    case effSetProgramName: {   /* only a saved slot has a line in the file to carry the name */
        const int n = clampi(w->cur.load(), 0, NSLOTS - 1);
        std::lock_guard<std::mutex> lk(g_bank_lock);
        if (!p || g_slot_chunk[n].empty()) return 0;
        std::string nm((const char *)p);
        for (char &c : nm) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        g_slot_name[n] = nm.substr(0, 23);
        bank_write_locked();
        return 0;
    }
    case effGetPlugCategory: return 1;   /* kPlugCategEffect */
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_bank_key(idx);
    case effGetParamName: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32); return 1;
    case effGetParamLabel: if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8); return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        char buf[32];
        display(w, idx, buf, sizeof buf);
        copy_str(p, buf, 24);
        return 1;
    }
    case effSetSampleRate:
        if (o > 0) { w->sr = o; w->engine.init(o); w->dirty.store(true); }
        return 1;
    case effSetBlockSize: case effMainsChanged: return 1;
    case effCanDo: return -1;
    case effGetChunk: return get_chunk(w, (void **)p);
    case effSetChunk: return set_chunk(w, p, v);
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < NPARAMS; i++) KEY_OF[i] = -1;
        for (int k = 0; k < NKEYS; k++) { IDX[k] = param_index(KEYS[k]); if (IDX[k] >= 0) KEY_OF[IDX[k]] = k; }
    });
    { std::lock_guard<std::mutex> lk(g_bank_lock); bank_load_locked(); }
    Plugin *w = new Plugin();
    w->master = master;
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->notify[i].store(0); }
    start_values(w);
    w->engine.init(w->sr);
    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450;
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numPrograms = NSLOTS;
    e->numInputs = 2;
    e->numOutputs = 2;
    e->initialDelay = wumms::LATENCY;   /* the 2x oversampling's filters */
    e->flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    return e;
}
