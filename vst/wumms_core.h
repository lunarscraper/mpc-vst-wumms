// wumms_core.h (DSP-Kern von Wumms, MIT-Lizenz, siehe ../LICENSE)
// End-of-Chain-Geraet nach Art des OTO Boum, Signalweg wie beim Original:
//   In Gain -> Gate -> Kompressor -> Lo Cut (6 dB/Okt) -> Verzerrung (4 Typen)
//   -> Hi Cut (12 dB/Okt) -> Mix (Dry/Wet) -> Level
// Neubau nach Bedienkonzept und Signalweg, keine Schaltungsemulation.
//
// Framework-neutral, keine Abhaengigkeiten ausser der Standardbibliothek.
// Verzerrung und Hi Cut laufen mit 2x-Oversampling (Halbband-FIR, linearphasig).
// Das kostet LATENCY Samples; das Dry-Signal wird um genau so viel verzoegert,
// damit MIX (Parallelkompression) ohne Kammfilter mischt.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wumms {

enum { DISTO_BOOST = 0, DISTO_TUBE, DISTO_FUZZ, DISTO_SQUARE, NDISTO };
enum { LATENCY = 15 };   // Samples (Basisrate): Hoch- und Runtertasten zusammen

struct Params
{
    float gainDb    = 0.0f;     // -12 .. +24  Eingangspegel
    float comp      = 0.0f;     // 0 .. 1  ein Regler: Threshold, Ratio und Makeup zusammen
                                //   0 = aus (1:1), 0.5 = Limiter (unendlich:1),
                                //   1 = Umkehr (1:-1, laut wird leiser als leise)
    float attackMs  = 5.0f;     // 0.05 .. 33
    float releaseMs = 200.0f;   // 50 .. 1000
    float drive     = 0.0f;     // 0 .. 1  (0 = Verzerrung ganz aus dem Weg)
    int   disto     = DISTO_BOOST;
    float mix       = 1.0f;     // 0 = nur Dry, 1 = nur Wet
    float levelDb   = 0.0f;     // -24 .. +12
    float locutHz   = 0.0f;     // 0 = aus, sonst 75 / 150 / 300
    float hicut     = 1.0f;     // 0 .. 1 Reglerweg: 20 Hz .. 20 kHz, ganz offen = umgangen
    float gateDb    = -100.0f;  // <= -90 = aus, sonst -50 .. 0
};

// Reglerweg -> Hz (auch fuer die Anzeige)
static inline float hicut_hz(float x) { return 20.0f * std::pow(1000.0f, std::min(1.0f, std::max(0.0f, x))); }

// Kompressor-Kennlinie aus dem einen Regler (auch fuer die Anzeige)
static inline float comp_slope(float c)     { return 2.0f * c; }                          // 1 - 1/Ratio
static inline float comp_thresh_db(float c) { return -30.0f * std::min(1.0f, 2.0f * c); }
static inline float comp_makeup_db(float c) { return 0.6f * -comp_thresh_db(c) * std::min(1.0f, comp_slope(c)); }

class Engine
{
public:
    void init(float sampleRate)
    {
        fs = sampleRate;
        fsOS = 2.0f * fs;
        smooth   = coef(0.005f);            // Reglerglaettung 5 ms
        gDetAtk  = coef(0.0002f);           // Gate-Pegelmesser
        gDetRel  = coef(0.010f);
        gOpen    = coef(0.0005f);           // Gate oeffnet in 0.5 ms ...
        gClose   = coef(0.060f);            // ... und schliesst in 60 ms
        gHold    = (int)(0.020f * fs);      // nach 20 ms unter der Schwelle
        dcCoef   = 1.0f - std::exp(-2.0f * kPi * 5.0f / fsOS);   // 5 Hz, nur im Zerr-Anteil
        fzCoef   = 1.0f - std::exp(-1.0f / (0.010f * fsOS));     // Fuzz: Pegelmesser 10 ms
        const float b = kTubeBias;          // Roehre: Arbeitspunkt verschoben
        tubeOff = softSat(b);
        const float d = (softSat(b + 0.001f) - softSat(b - 0.001f)) / 0.002f;
        tubeNorm = 1.0f / (1.0f + tubeOff);
        tubePre = (1.0f + tubeOff) / d;     // Kleinsignal-Verstaerkung 1
        reset();
    }

    void reset()
    {
        std::memset(ch, 0, sizeof(ch));
        idx = 0; osIdx = 0;
        gateEnv = 0.0f; gateGain = 1.0f; gateOpen = true; holdCnt = 0;
        grDb = 0.0f;
        first = true;
        ctrl = 0;
    }

    // Aktuelle Pegelreduktion des Kompressors in dB (<= 0), fuer Tests/Anzeige
    float gain_reduction_db() const { return grDb; }

    void process(const Params& p, const float* inL, const float* inR,
                 float* outL, float* outR, uint32_t frames)
    {
        // ---------- einmal pro Block ----------
        const float c       = clamp(p.comp, 0.0f, 1.0f);
        const float slope   = comp_slope(c);
        const float thrDb   = comp_thresh_db(c);
        const float kneeLo  = dbToGain(thrDb - 0.5f * kKnee);
        const float atk     = coef(std::max(0.05f, p.attackMs) * 0.001f);
        const float rel     = coef(std::max(10.0f, p.releaseMs) * 0.001f);

        const int   type    = p.disto < 0 ? 0 : p.disto >= NDISTO ? NDISTO - 1 : p.disto;
        const float drv     = clamp(p.drive, 0.0f, 1.0f);
        static const float kMaxDb[NDISTO] = { 30.0f, 36.0f, 48.0f, 60.0f };
        const float driveDb = kMaxDb[type] * drv;
        const float tDrive  = dbToGain(driveDb);
        // Lautheit ausgleichen: ein Signal mit Spitze kRef (-12 dBFS) behaelt seine Spitze,
        // leiseres kommt hoch, lauteres wird gestaucht
        // (Mittel aus beiden Halbwellen, TUBE und FUZZ kappen oben und unten verschieden)
        const float tPost   = 2.0f * kRef / (std::fabs(shape(kRef * tDrive, type)) + std::fabs(shape(-kRef * tDrive, type)));
        const float tAmt    = std::min(1.0f, drv * 10.0f);        // die ersten 10 % blenden ein

        const float tIn     = dbToGain(clamp(p.gainDb, -24.0f, 36.0f));
        const float tMk     = dbToGain(comp_makeup_db(c));
        const float tMix    = clamp(p.mix, 0.0f, 1.0f);
        const float tLevel  = dbToGain(clamp(p.levelDb, -60.0f, 12.0f));

        const bool  gateOn  = p.gateDb > -90.0f;
        const float gThr    = dbToGain(p.gateDb);

        const bool  loOn    = p.locutHz > 1.0f;
        const float loCoef  = 1.0f - std::exp(-2.0f * kPi * (loOn ? p.locutHz : 75.0f) / fs);

        const float hc      = clamp(p.hicut, 0.0f, 1.0f);
        const float tHiHz   = hicut_hz(hc);
        const float tHiByp  = clamp((hc - 0.95f) / 0.05f, 0.0f, 1.0f);   // ganz offen = umgangen

        if (first)   // nach init/reset ohne Einschwingen starten
        {
            inSm = tIn; mkSm = tMk; mixSm = tMix; levelSm = tLevel;
            driveSm = tDrive; postSm = tPost; amtSm = tAmt; hiHzSm = tHiHz; hiBypSm = tHiByp;
            setHiCut(hiHzSm);
            first = false;
        }
        if (type != lastType) { lastType = type; ch[0].dc = ch[1].dc = 0.0f; ch[0].fz = ch[1].fz = 0.0f; }

        for (uint32_t i = 0; i < frames; ++i)
        {
            inSm    += smooth * (tIn - inSm);
            mkSm    += smooth * (tMk - mkSm);
            mixSm   += smooth * (tMix - mixSm);
            levelSm += smooth * (tLevel - levelSm);
            driveSm += smooth * (tDrive - driveSm);
            postSm  += smooth * (tPost - postSm);
            amtSm   += smooth * (tAmt - amtSm);
            if (ctrl == 0)
            {
                hiHzSm  += 0.25f * (tHiHz - hiHzSm);
                hiBypSm += 0.25f * (tHiByp - hiBypSm);
                setHiCut(hiHzSm);
            }
            ctrl = (ctrl + 1) & 15;

            const float dry[2] = { inL[i], inR[i] };
            float x[2] = { dry[0] * inSm, dry[1] * inSm };

            // ---------- Gate (beide Kanaele gemeinsam, feste Zeiten wie beim Original) ----------
            float rect = std::max(std::fabs(x[0]), std::fabs(x[1]));
            gateEnv += (rect > gateEnv ? gDetAtk : gDetRel) * (rect - gateEnv);
            if (gateOn)
            {
                if (gateEnv > gThr)                  { gateOpen = true; holdCnt = gHold; }
                else if (gateEnv < gThr * 0.7f)      { if (holdCnt > 0) --holdCnt; else gateOpen = false; }
            }
            else gateOpen = true;
            gateGain += (gateOpen ? gOpen : gClose) * ((gateOpen ? 1.0f : 0.0f) - gateGain);
            rect *= gateGain;

            // ---------- Kompressor (Feed-Forward, Stereo gekoppelt, weiches Knie) ----------
            float gr = 0.0f;
            if (slope > 0.0f && rect > kneeLo)
            {
                const float over = 20.0f * std::log10(rect) - thrDb;
                if (over >= 0.5f * kKnee) gr = -slope * over;
                else { const float t = over + 0.5f * kKnee; gr = -slope * t * t / (2.0f * kKnee); }
                if (gr < -48.0f) gr = -48.0f;
            }
            grDb += (gr < grDb ? atk : rel) * (gr - grDb);
            float g = gateGain * mkSm;
            if (grDb < -0.001f) g *= std::exp(grDb * 0.11512925f);   // dB -> Faktor
            else grDb += kDenorm;

            for (int cI = 0; cI < 2; ++cI)
            {
                Channel& s = ch[cI];
                float v = x[cI] * g;

                // Lo Cut 6 dB/Okt vor der Verzerrung (nimmt ihr den Bass weg)
                s.loLp += loCoef * (v - s.loLp) + kDenorm;
                if (loOn) v -= s.loLp;

                // ---------- 2x hoch (Halbband, Polyphase) ----------
                s.up[idx] = v;
                float a = 0.0f;
                for (int k = 0; k < 8; ++k)
                    a += kHb[k] * (s.up[(idx - 7 + k) & 15] + s.up[(idx - 8 - k) & 15]);
                const float u[2] = { 2.0f * a, s.up[(idx - 7) & 15] };

                float w = 0.0f;
                for (int n = 0; n < 2; ++n)
                {
                    // Verzerrung: sauberer Anteil + eingeblendeter Zerr-Anteil
                    float vd = u[n] * driveSm;
                    if (type == DISTO_FUZZ)   // Arbeitspunkt wandert mit dem Pegel: schiefes Tastverhaeltnis
                    {
                        s.fz += fzCoef * (std::min(std::fabs(vd), 4.0f) - s.fz);
                        vd -= 0.35f * s.fz;
                    }
                    float sh = shape(vd, type) * postSm;
                    s.dc += dcCoef * (sh - s.dc);
                    sh -= s.dc;
                    float y = u[n] + amtSm * (sh - u[n]);

                    // Hi Cut 12 dB/Okt
                    const float lp = svfLp(s, y + kDenorm);
                    y = lp + hiBypSm * (y - lp);

                    // ---------- 2x runter: beim ersten der beiden Samples abgreifen ----------
                    const int o = (osIdx + n) & 31;
                    s.down[o] = y;
                    if (n == 0)
                    {
                        w = 0.5f * s.down[(o - 15) & 31];
                        for (int k = 0; k < 8; ++k)
                            w += kHb[k] * (s.down[(o - 14 + 2 * k) & 31] + s.down[(o - 16 - 2 * k) & 31]);
                    }
                }

                // ---------- Mix gegen das verzoegerte Dry-Signal, Level ----------
                s.dry[idx] = dry[cI];
                const float d = s.dry[(idx - LATENCY) & 15];
                x[cI] = (d + mixSm * (w - d)) * levelSm;
            }

            idx = (idx + 1) & 15;
            osIdx = (osIdx + 2) & 31;
            outL[i] = x[0];
            outR[i] = x[1];
        }
    }

private:
    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kDenorm = 1.0e-20f;   // gegen Denormals auf ARM
    static constexpr float kKnee = 6.0f;         // dB
    static constexpr float kTubeBias = 0.4f;
    static constexpr float kRef = 0.25f;         // Bezugspegel fuer den Lautheitsausgleich
    // Halbband-FIR, 31 Taps (Kaiser): die 8 Koeffizienten bei +-1, +-3, .. +-15 um die Mitte
    // (Mitte 0.5, alle anderen 0). Bis 16 kHz flach, ab 28 kHz mehr als 65 dB gedaempft (44.1 kHz).
    static constexpr float kHb[8] = { 0.314424587f, -0.094995013f, 0.046589055f, -0.024251087f,
                                      0.011989062f, -0.005208734f, 0.001767719f, -0.000315589f };

    struct Channel
    {
        float up[16];      // Eingang des Hochtasters (Basisrate)
        float down[32];    // Eingang des Runtertasters (2x)
        float dry[16];     // Dry-Verzoegerung
        float loLp, dc, fz, ic1, ic2;
    };

    float coef(float seconds) const { return 1.0f - std::exp(-1.0f / (seconds * fs)); }

    void setHiCut(float fc)
    {
        fc = clamp(fc, 20.0f, 0.45f * fsOS);
        const float g = std::tan(kPi * fc / fsOS);
        const float k = 1.41421356f;             // Butterworth, keine Resonanz
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    inline float svfLp(Channel& s, float x) const   // TPT-State-Variable-Filter, 2-Pol
    {
        const float v3 = x - s.ic2;
        const float v1 = a1 * s.ic1 + a2 * v3;
        const float v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
        s.ic1 = 2.0f * v1 - s.ic1;
        s.ic2 = 2.0f * v2 - s.ic2;
        return v2;
    }

    static inline float softSat(float x)            // schnelle tanh-Naeherung, |y| <= 1
    {
        x = clamp(x, -3.0f, 3.0f);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    inline float shape(float v, int type) const
    {
        switch (type)
        {
        case DISTO_TUBE:      // unsymmetrisch weich: gerade Obertoene
            return (softSat(tubePre * v + kTubeBias) - tubeOff) * tubeNorm;
        case DISTO_FUZZ:      // oben hart gekappt, unten weich und frueher (plus Arbeitspunkt, s. process)
            return v >= 0.0f ? std::min(v, 1.0f) : 0.7f * softSat(v * (1.0f / 0.7f));
        case DISTO_SQUARE:    // hart und steil: wird zum Rechteck
            return clamp(4.0f * v, -1.0f, 1.0f);
        default:              // BOOST: symmetrisch weich
            return softSat(v);
        }
    }

    static inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
    static inline float clamp(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

    float fs = 44100.0f, fsOS = 88200.0f;
    float smooth = 0.0f, gDetAtk = 0.0f, gDetRel = 0.0f, gOpen = 0.0f, gClose = 0.0f, dcCoef = 0.0f, fzCoef = 0.0f;
    float tubeOff = 0.0f, tubeNorm = 1.0f, tubePre = 1.0f;
    int   gHold = 0;

    Channel ch[2];
    int   idx = 0, osIdx = 0, ctrl = 0, lastType = -1;
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

    float gateEnv = 0.0f, gateGain = 1.0f;
    bool  gateOpen = true;
    int   holdCnt = 0;
    float grDb = 0.0f;

    bool  first = true;
    float inSm = 1.0f, mkSm = 1.0f, mixSm = 1.0f, levelSm = 1.0f;
    float driveSm = 1.0f, postSm = 1.0f, amtSm = 0.0f, hiHzSm = 20000.0f, hiBypSm = 1.0f;
};

} // namespace wumms
