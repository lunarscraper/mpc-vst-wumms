/* Offline x86 test of wumms_vst.cpp (test.sh builds it with ASan/UBSan): the start setting passes
 * the signal unchanged (apart from the reported latency), IN GAIN and LEVEL, the one-knob
 * compressor (2:1-ish, limiter, reversal; attack and release), the gate, LO CUT and HI CUT, DRIVE
 * for each distortion type, MIX (0 = dry, parallel mixing without comb filtering), channel
 * independence, the 32 preset slots (program list, factory settings, SAVE/LOAD, shared bank file),
 * loud input below 0 dBFS, chunk restore, a random stress run at three sample rates, NaN/denormal-
 * free output. With "bench" as the second argument it only measures CPU load. PASSED/FAILED. */
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

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
static intptr_t master(AEffect *, int32_t, int32_t, intptr_t, void *, float) { return 0; }
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)
static float SR = 44100;
static const int BS = 256;
static bool bad = false;
typedef std::vector<float> Buf;

static int param(AEffect *e, const char *name) {
    char buf[64];
    for (int i = 0; i < e->numParams; i++) { buf[0] = 0; e->dispatcher(e, 8, i, 0, buf, 0); if (!std::strcmp(buf, name)) return i; }
    std::printf("FAIL: no parameter %s\n", name); fails++; return 0;
}
static std::string display(AEffect *e, const char *name) { char buf[64] = {0}; e->dispatcher(e, 7, param(e, name), 0, buf, 0); return buf; }
/* set a parameter in the units of module.json (0..100 unless listed; switches by index) */
static void set(AEffect *e, const char *name, double v) {
    double lo = 0, hi = 100;
    const std::string s = name;
    if (s == "In Gain") { lo = -12; hi = 24; }
    else if (s == "Level") { lo = -24; hi = 12; }
    else if (s == "Disto" || s == "Lo Cut") { hi = 3; }
    else if (s == "Gate") { hi = 6; }
    else if (s == "Load" || s == "Save") { hi = 1; }
    else if (s == "Preset") { lo = 1; hi = 32; }
    e->setParameter(e, param(e, name), (float)((v - lo) / (hi - lo)));
}
static void press(AEffect *e, const char *name) { set(e, name, 1); set(e, name, 0); }
static std::string progname(AEffect *e, int i) { char b[64] = {0}; e->dispatcher(e, 29, i, 0, b, 0); return b; }

/* the test signal: a sine at f, amplitude amp on the right, ampL on the left (default: the same) */
static double g_ph = 0;
static Buf g_in;   /* what the last run() fed into the right channel */
static Buf run(AEffect *e, double f, double amp, double sec, Buf *left = nullptr, double ampL = -1) {
    Buf outR, il(BS), ir(BS), ol(BS), orr(BS);
    float *in[2] = {il.data(), ir.data()}, *out[2] = {ol.data(), orr.data()};
    if (ampL < 0) ampL = amp;
    g_in.clear();
    for (int b = 0; b < (int)(sec * SR / BS); b++) {
        for (int i = 0; i < BS; i++) {
            const double s = std::sin(g_ph); g_ph += 2 * M_PI * f / SR;
            il[i] = (float)(ampL * s); ir[i] = (float)(amp * s); g_in.push_back(ir[i]);
        }
        e->processReplacing(e, in, out, BS);
        for (int i = 0; i < BS; i++) {
            for (float s : {ol[i], orr[i]}) if (!std::isfinite(s) || std::fpclassify(s) == FP_SUBNORMAL) bad = true;
            outR.push_back(orr[i]);
            if (left) left->push_back(ol[i]);
        }
    }
    return outR;
}
static void silence(AEffect *e, double sec) { run(e, 100, 0.0, sec); }
/* amplitude of the component at f (Hann window, so neighbouring harmonics do not leak in) */
static double goertzel(const Buf &a, double f, double t0) {
    size_t i0 = (size_t)(t0 * SR), i1 = a.size();
    double w = 2 * M_PI * f / SR, c = std::cos(w), s1 = 0, s2 = 0, sum = 0;
    for (size_t i = i0; i < i1; i++) {
        const double h = 0.5 - 0.5 * std::cos(2 * M_PI * (i - i0) / (double)(i1 - i0));
        double s0 = h * a[i] + 2 * c * s1 - s2; s2 = s1; s1 = s0; sum += h;
    }
    return 2 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - 2 * c * s1 * s2)) / sum;
}
static double rms(const Buf &a, double t0 = 0.2, double t1 = -1) {
    double s = 0; size_t i0 = (size_t)(t0 * SR), i1 = t1 < 0 ? a.size() : (size_t)(t1 * SR);
    for (size_t i = i0; i < i1; i++) s += (double)a[i] * a[i];
    return std::sqrt(s / (i1 - i0));
}
static double peak(const Buf &a, double t0 = 0, double t1 = -1) {
    double p = 0; size_t i0 = (size_t)(t0 * SR), i1 = t1 < 0 ? a.size() : (size_t)(t1 * SR);
    for (size_t i = i0; i < i1; i++) p = std::max(p, (double)std::fabs(a[i]));
    return p;
}
/* harmonic k of f0 relative to the fundamental */
static double harm(const Buf &a, double f0, int k) { return goertzel(a, f0 * k, 0.2) / std::max(goertzel(a, f0, 0.2), 1e-12); }
static double thd(const Buf &a, double f0) {
    double hs = 0;
    for (int k = 2; k <= 10; k++) { double h = harm(a, f0, k); hs += h * h; }
    return std::sqrt(hs);
}
static double db(double x) { return 20 * std::log10(x + 1e-12); }
/* largest difference between the output and the input delayed by `delay` samples, from t0 on */
static double diff_delayed(const Buf &out, const Buf &in, int delay, double t0 = 0.2) {
    double d = 0;
    for (size_t i = (size_t)(t0 * SR); i < out.size(); i++) d = std::max(d, (double)std::fabs(out[i] - in[i - delay]));
    return d;
}

/* the start setting: everything out of the way */
static void neutral(AEffect *e) {
    set(e, "In Gain", 0); set(e, "Comp", 0); set(e, "Attack", 60); set(e, "Release", 45); set(e, "Drive", 0);
    set(e, "Disto", 0); set(e, "Mix", 100); set(e, "Level", 0); set(e, "Lo Cut", 0); set(e, "Hi Cut", 100); set(e, "Gate", 0);
}

int main(int argc, char **argv) {
    if (argc < 2) { std::printf("usage: host_test plugin.so [bench]\n"); return 2; }
    const char *bank = "/tmp/wumms_test_presets.txt";
    std::remove(bank);
    setenv("WUMMS_PRESETS", bank, 1);
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { std::printf("FAILED: dlopen %s\n", dlerror()); return 1; }
    typedef AEffect *(*MainF)(audioMasterCallback);
    MainF mainf = (MainF)dlsym(h, "VSTPluginMain");
    if (!mainf) { std::printf("FAILED: no VSTPluginMain\n"); return 1; }
    AEffect *e = mainf(master);
    CHECK(e->numInputs == 2 && e->numOutputs == 2, "not a stereo effect");
    e->dispatcher(e, 0, 0, 0, nullptr, 0);
    e->dispatcher(e, 10, 0, 0, nullptr, SR);
    const int LAT = e->initialDelay;

    if (argc > 2 && !std::strcmp(argv[2], "bench")) {
        set(e, "In Gain", 6); set(e, "Comp", 40); set(e, "Drive", 60); set(e, "Disto", 1); set(e, "Lo Cut", 1);
        set(e, "Hi Cut", 80); set(e, "Gate", 2); set(e, "Mix", 70);
        auto t0 = std::chrono::steady_clock::now();
        run(e, 110, 0.3, 10.0);
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("bench (this CPU, one core): stereo, everything on: %.2f %%\n", 10.0 * s);
        return 0;
    }
    std::printf("defaults: gain %s, comp %s, attack %s, release %s, disto %s, hi cut %s, gate %s, preset %s, latency %d\n",
                display(e, "In Gain").c_str(), display(e, "Comp").c_str(), display(e, "Attack").c_str(), display(e, "Release").c_str(),
                display(e, "Disto").c_str(), display(e, "Hi Cut").c_str(), display(e, "Gate").c_str(), display(e, "Preset").c_str(), LAT);

    /* 1. the start setting: the input comes back unchanged, LAT samples late */
    {
        Buf a = run(e, 220, 0.5, 1.0); Buf in = g_in;
        double d1 = diff_delayed(a, in, LAT), t = thd(a, 220);
        Buf b = run(e, 8000, 0.5, 0.5); double d2 = diff_delayed(b, g_in, LAT);
        Buf c = run(e, 15000, 0.5, 0.5); double g3 = db(rms(c) / rms(g_in));
        std::printf("  start setting: difference to the delayed input %.1f dB (220 Hz), %.1f dB (8 kHz); 15 kHz at %+.2f dB; THD %.4f %%\n",
                    db(d1 / 0.5), db(d2 / 0.5), g3, 100 * t);
        CHECK(d1 < 0.5e-3 && d2 < 0.5e-2, "the start setting changes the signal");
        CHECK(std::fabs(g3) < 0.3, "treble loss in the start setting (%+.2f dB)", g3);
        CHECK(t < 1e-4, "the start setting distorts");
    }

    /* 2. IN GAIN and LEVEL */
    {
        set(e, "In Gain", 6); double g1 = db(rms(run(e, 220, 0.1, 0.5)) / rms(g_in));
        set(e, "In Gain", 0); set(e, "Level", -12); double g2 = db(rms(run(e, 220, 0.1, 0.5)) / rms(g_in));
        std::printf("  in gain +6: %+.2f dB, level -12: %+.2f dB (%s)\n", g1, g2, display(e, "Level").c_str());
        CHECK(std::fabs(g1 - 6) < 0.2 && std::fabs(g2 + 12) < 0.2, "IN GAIN / LEVEL off");
        neutral(e);
    }

    /* 3. the compressor: a quiet (-30 dBFS) and a loud (-6 dBFS) tone; their distance shrinks */
    {
        set(e, "Attack", 20); set(e, "Release", 30);
        const double AQ = 0.0316, AL = 0.5;
        double dist[4]; int k = 0;
        for (int c : {0, 25, 50, 100}) {
            set(e, "Comp", c); silence(e, 0.3);
            double q = rms(run(e, 220, AQ, 1.0), 0.6); silence(e, 0.3);
            double l = rms(run(e, 220, AL, 1.0), 0.6);
            dist[k++] = db(l / q);
            std::printf("  comp %3d (%-8s): quiet %6.1f dBFS, loud %6.1f dBFS, distance %5.1f dB\n", c, display(e, "Comp").c_str(), db(q), db(l), db(l / q));
        }
        CHECK(std::fabs(dist[0] - 24) < 0.2, "COMP 0 is not off");
        CHECK(dist[1] > 10 && dist[1] < 20, "COMP 25 does not compress about 2:1 (%.1f dB)", dist[1]);
        CHECK(std::fabs(dist[2]) < 2.5, "COMP 50 does not limit (%.1f dB)", dist[2]);
        CHECK(dist[3] < -10, "COMP 100 does not reverse the dynamics (%.1f dB)", dist[3]);
        CHECK(display(e, "Comp").compare(0, 3, "REV") == 0, "comp readout %s", display(e, "Comp").c_str());
        /* attack: a slow attack lets the first milliseconds through */
        double pk[2];
        set(e, "Comp", 50);
        for (int j = 0; j < 2; j++) {
            set(e, "Attack", j ? 100 : 0); silence(e, 1.5);
            Buf a = run(e, 1000, 0.5, 0.5);
            pk[j] = rms(a, 0, 0.010) / rms(a, 0.3);
        }
        std::printf("  attack: first 10 ms against the settled level %+.1f dB (%s) vs %+.1f dB (attack 100)\n", db(pk[0]), "attack 0", db(pk[1]));
        CHECK(pk[1] > pk[0] * 2, "ATTACK does not let the transient through");
        /* release: after a loud tone a quiet one comes back sooner with a short release */
        double rc[2];
        set(e, "Attack", 20);
        for (int j = 0; j < 2; j++) {
            set(e, "Release", j ? 100 : 0); silence(e, 1.5);
            run(e, 1000, 0.5, 0.5);
            Buf a = run(e, 1000, 0.02, 1.0);
            rc[j] = rms(a, 0.05, 0.15) / rms(a, 0.9);
        }
        std::printf("  release: quiet tone 50..150 ms after the loud one at %+.1f dB (release 0) vs %+.1f dB (release 100) of its settled level\n", db(rc[0]), db(rc[1]));
        CHECK(rc[0] > rc[1] * 2, "RELEASE does not change the recovery");
        neutral(e);
    }

    /* 4. the gate at -30 dB: -40 dBFS closes it, -20 dBFS passes */
    {
        set(e, "Gate", 3);
        silence(e, 0.5); double q = rms(run(e, 220, 0.01, 1.0), 0.5) / rms(g_in, 0.5);
        silence(e, 0.5); double l = rms(run(e, 220, 0.1, 1.0), 0.5) / rms(g_in, 0.5);
        set(e, "Gate", 0);
        silence(e, 0.5); double o = rms(run(e, 220, 0.01, 1.0), 0.5) / rms(g_in, 0.5);
        std::printf("  gate %s: -40 dBFS at %.1f dB, -20 dBFS at %+.2f dB; off: -40 dBFS at %+.2f dB\n", "-30 DB", db(q), db(l), db(o));
        CHECK(q < 0.01 && std::fabs(db(l)) < 0.2 && std::fabs(db(o)) < 0.2, "the gate does not gate");
        set(e, "Gate", 3); CHECK(display(e, "Gate") == "-30 DB", "gate readout %s", display(e, "Gate").c_str());
        neutral(e);
    }

    /* 5. LO CUT (6 dB/oct) and HI CUT (12 dB/oct) */
    {
        double lo[4];
        for (int k = 0; k < 4; k++) { set(e, "Lo Cut", k); lo[k] = db(rms(run(e, 50, 0.3, 0.6), 0.3) / rms(g_in, 0.3)); }
        std::printf("  lo cut: 50 Hz at %+.1f / %.1f / %.1f / %.1f dB (off, 75, 150, 300 Hz)\n", lo[0], lo[1], lo[2], lo[3]);
        CHECK(std::fabs(lo[0]) < 0.1 && lo[1] < -3 && lo[2] < lo[1] - 3 && lo[3] < lo[2] - 3 && lo[3] > -20, "LO CUT is not 75/150/300 Hz at 6 dB/oct");
        set(e, "Lo Cut", 0);
        set(e, "Hi Cut", 60);
        double fc = 20.0 * std::pow(1000.0, 0.6);
        double h1 = db(rms(run(e, fc, 0.3, 0.6), 0.3) / rms(g_in, 0.3)), h2 = db(rms(run(e, fc * 4, 0.3, 0.6), 0.3) / rms(g_in, 0.3));
        std::printf("  hi cut %s: %.1f dB at the corner, %.1f dB two octaves above\n", display(e, "Hi Cut").c_str(), h1, h2);
        CHECK(std::fabs(h1 + 3) < 0.7 && std::fabs(h2 + 24) < 2, "HI CUT is not 12 dB/oct");
        neutral(e);
        CHECK(display(e, "Hi Cut") == "OFF", "hi cut readout %s", display(e, "Hi Cut").c_str());
    }

    /* 6. DRIVE for each type: clean at 0, harmonics at 100; TUBE and FUZZ have even ones, SQUARE ends as a square */
    {
        const char *dn[4] = {"BOOST", "TUBE", "FUZZ", "SQUARE"};
        for (int s = 0; s < 4; s++) {
            set(e, "Disto", s);
            double t[3], h2 = 0, h3 = 0, lv[3];
            int k = 0;
            for (int d : {0, 30, 100}) {
                set(e, "Drive", d);
                Buf a = run(e, 220, 0.125, 0.6);
                t[k] = thd(a, 220); lv[k] = db(rms(a) / rms(g_in)); h2 = harm(a, 220, 2); h3 = harm(a, 220, 3); k++;
            }
            std::printf("  %-6s: THD %6.3f %% / %5.1f %% / %5.1f %% (drive 0, 30, 100), level %+.1f / %+.1f / %+.1f dB, at 100: 2nd %.3f, 3rd %.3f\n",
                        dn[s], 100 * t[0], 100 * t[1], 100 * t[2], lv[0], lv[1], lv[2], h2, h3);
            CHECK(display(e, "Disto") == dn[s], "disto readout %s", display(e, "Disto").c_str());
            CHECK(t[0] < 1e-4, "%s: DRIVE 0 is not clean", dn[s]);
            CHECK(t[2] > 0.1 && t[2] > t[1] && t[1] > 0.002, "%s: DRIVE adds no harmonics", dn[s]);
            CHECK(lv[2] > -6 && lv[2] < 14, "%s: level at full drive %+.1f dB", dn[s], lv[2]);
            if (s == 0 || s == 3) CHECK(h2 < 0.01, "%s has even harmonics", dn[s]);
            if (s == 1) { set(e, "Drive", 30); double m = harm(run(e, 220, 0.125, 0.6), 220, 2); CHECK(m > 0.02, "TUBE has no even harmonics (%.4f)", m); }
            if (s == 2) CHECK(h2 > 0.03, "FUZZ has no even harmonics");
            if (s == 3) CHECK(std::fabs(h3 - 1.0 / 3) < 0.03, "SQUARE is not a square (3rd %.3f)", h3);
            double off = 0; { Buf a = run(e, 220, 0.125, 0.6); for (size_t i = (size_t)(0.3 * SR); i < a.size(); i++) off += a[i]; off /= (a.size() - 0.3 * SR); }
            CHECK(std::fabs(off) < 2e-3, "%s: DC at the output (%.4f)", dn[s], off);
        }
        neutral(e);
    }

    /* 7. MIX: 0 = the dry input (delayed); 50 with a neutral wet path mixes without comb filtering */
    {
        set(e, "Mix", 50);
        Buf b = run(e, 9000, 0.4, 0.6);
        double g = db(rms(b) / rms(g_in));
        set(e, "Drive", 100); set(e, "Disto", 3); set(e, "Comp", 60); set(e, "Mix", 0);
        Buf a = run(e, 5000, 0.4, 0.6);
        double d = diff_delayed(a, g_in, LAT);
        std::printf("  mix 0: difference to the delayed input %.1f dB; mix 50 (neutral): 9 kHz at %+.2f dB\n", db(d / 0.4), g);
        CHECK(d < 1e-4, "MIX 0 is not dry");
        CHECK(std::fabs(g) < 0.1, "dry and wet do not add up in phase");
        neutral(e); silence(e, 3.0);   /* let the compressor recover */
    }

    /* 8. channels: with drive, a silent left stays silent */
    {
        set(e, "Drive", 60); set(e, "Disto", 1);
        silence(e, 0.5);
        Buf L; Buf R = run(e, 220, 0.2, 0.6, &L, 0.0);
        std::printf("  stereo: left (silent in) rms %.6f, right rms %.4f\n", rms(L, 0.3), rms(R, 0.3));
        CHECK(rms(L, 0.3) < 1e-4 && rms(R, 0.3) > 0.01, "channels not independent");
        neutral(e);
    }

    /* 9. the 32 slots: program list, factory settings, SAVE/LOAD, bank file shared by instances */
    {
        CHECK(e->numPrograms == 32, "numPrograms %d", e->numPrograms);
        CHECK(progname(e, 0) == "CLEAN" && progname(e, 1) == "MASTER GLUE" && progname(e, 31) == "32 (empty)", "program names: %s / %s / %s",
              progname(e, 0).c_str(), progname(e, 1).c_str(), progname(e, 31).c_str());
        for (int i = 0; i < 16; i++) {
            e->dispatcher(e, 2, 0, i, nullptr, 0);
            silence(e, 0.3);
            Buf a = run(e, 110, 0.25, 1.5);
            std::printf("  preset %-15s -15 dBFS in -> %6.1f dBFS out, peak %.3f\n", progname(e, i).c_str(), db(rms(a, 0.8)), peak(a));
            CHECK(rms(a, 0.8) > 0.02 && rms(a, 0.8) < 0.6 && peak(a) <= 0.991, "preset %s silent or too hot", progname(e, i).c_str());
            CHECK(e->dispatcher(e, 3, 0, 0, nullptr, 0) == i, "effGetProgram");
        }
        /* slot 1 is the start setting */
        e->dispatcher(e, 2, 0, 5, nullptr, 0);
        e->dispatcher(e, 2, 0, 0, nullptr, 0);
        CHECK(display(e, "Comp") == "OFF" && display(e, "Drive") == "0 %" && display(e, "Mix") == "100 %", "CLEAN does not reset");
        /* turning the PRESET knob only browses */
        e->dispatcher(e, 2, 0, 3, nullptr, 0);
        std::string disto = display(e, "Disto");
        set(e, "Preset", 28);
        CHECK(display(e, "Preset") == "28 (empty)" && display(e, "Disto") == disto, "browsing changed the setting");
        /* SAVE to 28, change, LOAD */
        set(e, "Comp", 37.5); set(e, "Disto", 2); set(e, "Lo Cut", 3);
        press(e, "Save");
        CHECK(display(e, "Preset") == "Wumms 28" && progname(e, 27) == "Wumms 28", "saved slot name: %s", display(e, "Preset").c_str());
        set(e, "Comp", 90); set(e, "Disto", 0); set(e, "Lo Cut", 0);
        press(e, "Load");
        CHECK(display(e, "Disto") == "FUZZ" && display(e, "Lo Cut") == "300 HZ" && std::fabs(e->getParameter(e, param(e, "Comp")) - 0.375f) < 1e-4, "LOAD does not restore the saved setting");
        CHECK(e->getParameter(e, param(e, "Load")) == 0 && e->getParameter(e, param(e, "Save")) == 0, "buttons do not read released");
        /* LOAD on an empty slot leaves the setting alone */
        set(e, "Preset", 30); press(e, "Load");
        CHECK(display(e, "Disto") == "FUZZ", "empty slot wiped the setting");
        /* a factory slot can be overwritten, a second instance sees the bank, the file has both */
        set(e, "Preset", 2); press(e, "Save");
        e->dispatcher(e, 4, 0, 0, (void *)"MY GLUE", 0);
        AEffect *e2 = mainf(master);
        CHECK(progname(e2, 1) == "MY GLUE" && progname(e2, 27) == "Wumms 28" && progname(e2, 2) == "GOA GLUE", "second instance: %s", progname(e2, 1).c_str());
        e2->dispatcher(e2, 2, 0, 27, nullptr, 0);
        CHECK(display(e2, "Disto") == "FUZZ" && display(e2, "Preset") == "Wumms 28", "second instance cannot load slot 28");
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
        int lines = 0; char line[4096];
        if (FILE *f = std::fopen(bank, "r")) { while (std::fgets(line, sizeof line, f)) lines++; std::fclose(f); }
        CHECK(lines == 2, "bank file has %d lines", lines);
        neutral(e);
    }

    /* 10. everything up, full-scale input: stays below 0 dBFS */
    for (int s = 0; s < 4; s++) {
        neutral(e);
        set(e, "In Gain", 24); set(e, "Drive", 100); set(e, "Disto", s); set(e, "Level", 12); set(e, "Comp", 30);
        double pk = peak(run(e, 110, 1.0, 0.5));
        std::printf("  hot (disto %d): peak %.3f\n", s, pk);
        CHECK(pk < 1.0, "output above 0 dBFS");
    }

    /* 11. chunk */
    neutral(e);
    set(e, "Disto", 2); set(e, "Lo Cut", 1); set(e, "Gate", 4); set(e, "Level", -4); set(e, "Hi Cut", 43.21); set(e, "Preset", 5);
    {
        void *chunk = nullptr;
        intptr_t len = e->dispatcher(e, 23, 0, 0, &chunk, 0);
        std::vector<uint8_t> copy((uint8_t *)chunk, (uint8_t *)chunk + len);
        AEffect *e2 = mainf(master);
        e2->dispatcher(e2, 10, 0, 0, nullptr, SR);
        e2->dispatcher(e2, 24, 0, (intptr_t)copy.size(), copy.data(), 0);
        for (const char *k : {"Disto", "Lo Cut", "Gate", "Level", "Hi Cut", "Release", "Preset"})
            CHECK(display(e2, k) == display(e, k), "chunk: %s %s vs %s", k, display(e2, k).c_str(), display(e, k).c_str());
        CHECK(std::fabs(e2->getParameter(e2, param(e2, "Hi Cut")) - 0.4321f) < 1e-4, "chunk loses knob resolution");
        std::printf("  chunk %ld bytes: %s, %s, %s, %s, %s, %s\n", (long)len, display(e2, "Disto").c_str(), display(e2, "Lo Cut").c_str(),
                    display(e2, "Gate").c_str(), display(e2, "Level").c_str(), display(e2, "Hi Cut").c_str(), display(e2, "Preset").c_str());
        e2->dispatcher(e2, 1, 0, 0, nullptr, 0);
    }

    /* 12. stress: random parameters (LOAD/SAVE included) on a loud tone; 44.1, 48 and 96 kHz */
    {
        std::srand(808);
        double pk = 0;
        for (float sr : {44100.0f, 48000.0f, 96000.0f}) {
            SR = sr;
            e->dispatcher(e, 10, 0, 0, nullptr, sr);
            for (int it = 0; it < 400; it++) {
                for (int j = 0; j < 4; j++) e->setParameter(e, std::rand() % e->numParams, (float)std::rand() / RAND_MAX);
                pk = std::max(pk, peak(run(e, 40 + std::rand() % 4000, it % 7 ? 0.9 : 0.0, 0.03)));
            }
        }
        SR = 44100;
        std::printf("  stress: peak %.3f\n", pk);
        CHECK(pk < 1.0, "stress run above 0 dBFS");
    }

    CHECK(!bad, "NaN, Inf or denormals in the output");
    e->dispatcher(e, 1, 0, 0, nullptr, 0);
    std::remove(bank);
    std::printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
