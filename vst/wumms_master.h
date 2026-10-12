// wumms_master.h (MASTER-Seite von Wumms, MIT-Lizenz, siehe ../LICENSE)
// Die Endstufe fuer ein fertiges Master, hinter dem bisherigen Wumms-Signalweg:
//   Low Shelf -> High Shelf -> Mono Bass (Weiche auf Mitte/Seite) -> Width -> Push -> Limiter
// Der Limiter schaut LOOK Samples voraus, misst die Spitzen 4x ueberabgetastet (True Peak,
// Naeherung) und haelt den Ausgang unter CEILING. Dazu misst die Seite die Lautheit des
// Ausgangs (Short-Term-LUFS nach ITU-R BS.1770, 3-s-Fenster) und die Pegelreduktion.
//
// Abgeschaltet (MASTER OFF) laeuft nur eine Verzoegerung um LOOK Samples, damit die Latenz,
// die das Plugin dem Host meldet, immer gleich bleibt. Ein- und Ausschalten blendet in 20 ms.
// Framework-neutral, keine Abhaengigkeiten ausser der Standardbibliothek.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wumms {

struct MasterParams
{
    bool  on        = false;
    float lowDb     = 0.0f;     // -6 .. +6   Low Shelf (120 Hz)
    float highDb    = 0.0f;     // -6 .. +6   High Shelf (8 kHz)
    float monoHz    = 0.0f;     // 0 = aus, sonst 40 .. 250: darunter wird der Bass mono
                                //   (Linkwitz-Riley 24 dB/Okt: Seite Hochpass, Mitte gleiche Phase)
    float width     = 1.0f;     // 0 = mono, 1 = unveraendert, 2 = doppelt so breit
    float pushDb    = 0.0f;     // 0 .. +12   Pegel in den Limiter
    float ceilDb    = -1.0f;    // -6 .. -0.1 hoechster Ausgangspegel
    float releaseMs = 0.0f;     // 0 = AUTO, sonst 10 .. 1000
};

class Master
{
public:
    enum { LOOK = 64 };         // Samples Vorausschau (1,45 ms bei 44,1 kHz), immer im Signalweg

    void init(float sampleRate)
    {
        fs = sampleRate;
        smooth = coef(0.020f);             // Reglerglaettung pro Block-Sample
        fadeStep = 1.0f / (0.020f * fs);   // Ein/Aus in 20 ms
        actCoef = coef(0.5f);              // AUTO-Release: wie lange schon begrenzt wird
        rcFast = coef(0.020f);             // AUTO: 20 ms nach kurzen Spitzen ...
        rcSlow = coef(0.500f);             // ... 500 ms, wenn dauernd begrenzt wird
        initKWeighting();
        initInterp();
        blockLen = std::max(1, (int)std::lround(0.1 * fs));   // 100-ms-Bloecke fuer die Lautheit
        reset();
        std::memset(dlyX, 0, sizeof(dlyX));
        std::memset(kz, 0, sizeof(kz));
        std::memset(blocks, 0, sizeof(blocks));
        blockAcc = 0.0; blockCnt = 0; blockIdx = 0; blocksFilled = 0;
        lufs = -100.0f; fade = 0.0f; wIdx = 0;
        first = true;
    }

    // Filter und Limiter auf null (beim Einschalten: das Einblenden verdeckt den Neustart)
    void reset()
    {
        std::memset(eq, 0, sizeof(eq));
        std::memset(sideZ, 0, sizeof(sideZ));
        std::memset(midZ, 0, sizeof(midZ));
        std::memset(dlyY, 0, sizeof(dlyY));
        std::memset(hist, 0, sizeof(hist));
        dqHead = dqTail = 0;
        boxSum = 0.0; boxCnt = 0;
        std::memset(boxBuf, 0, sizeof(boxBuf));
        relG = 1.0f; act = 0.0f; tIdx[0] = tIdx[1] = 0;
        for (int i = 0; i < WIN; ++i) boxBuf[i] = 1.0f;
        boxSum = WIN;
    }

    // fuer die Anzeige: Short-Term-Lautheit (LUFS, -100 = Stille) und Pegelreduktion (dB, <= 0)
    float loudness_lufs() const { return lufs; }
    float gain_reduction_db() const { return grBlock; }
    bool  active() const { return fade > 0.0f; }

    void process(const MasterParams& p, float* L, float* R, uint32_t frames)
    {
        // ---------- einmal pro Block ----------
        const float tLow  = clamp(p.lowDb, -12.0f, 12.0f);
        const float tHigh = clamp(p.highDb, -12.0f, 12.0f);
        const float tW    = clamp(p.width, 0.0f, 2.0f);
        const float tPush = dbToGain(clamp(p.pushDb, -12.0f, 24.0f));
        const float ceil  = dbToGain(clamp(p.ceilDb, -24.0f, -0.1f));
        const bool  monoOn = p.monoHz > 1.0f;
        const float relFix = p.releaseMs > 0.5f ? coef(clamp(p.releaseMs, 5.0f, 2000.0f) * 0.001f) : -1.0f;

        if (first) { lowSm = tLow; highSm = tHigh; wSm = tW; pushSm = tPush; fade = p.on ? 1.0f : 0.0f; first = false; }
        if (p.on && fade <= 0.0f) reset();   // frisch einschalten: aus dem Stand

        // Shelf-Gains pro Block nachziehen (Koeffizienten pro Block, Gain-Spruenge klein)
        const float kb = 1.0f - std::pow(1.0f - smooth, (float)frames);
        lowSm  += kb * (tLow - lowSm);
        highSm += kb * (tHigh - highSm);
        if (std::fabs(lowSm - tLow) < 0.002f) lowSm = tLow;
        if (std::fabs(highSm - tHigh) < 0.002f) highSm = tHigh;
        const bool lowOn = std::fabs(lowSm) > 0.001f, highOn = std::fabs(highSm) > 0.001f;
        if (lowOn)  shelf(lowSm, 120.0f, false, lowC);
        if (highOn) shelf(highSm, 8000.0f, true, highC);
        if (monoOn && p.monoHz != sideHz) { sideHz = p.monoHz; highpass(sideHz, sideC); allpass(sideHz, midC); }

        const bool run = p.on || fade > 0.0f;
        float minG = 1.0f;

        for (uint32_t i = 0; i < frames; ++i)
        {
            const float x[2] = { L[i], R[i] };
            dlyX[0][wIdx] = x[0];
            dlyX[1][wIdx] = x[1];
            const int rIdx = (wIdx - LOOK) & (DLY - 1);
            float o[2] = { dlyX[0][rIdx], dlyX[1][rIdx] };   // der Bypass: nur verzoegert

            if (run)
            {
                fade = clamp(fade + (p.on ? fadeStep : -fadeStep), 0.0f, 1.0f);
                wSm    += smooth * (tW - wSm);
                pushSm += smooth * (tPush - pushSm);

                // ---------- EQ ----------
                float y[2] = { x[0], x[1] };
                for (int c = 0; c < 2; ++c)
                {
                    if (lowOn)  y[c] = biquad(lowC, eq[c][0], y[c]);
                    if (highOn) y[c] = biquad(highC, eq[c][1], y[c]);
                }
                // ---------- Mono Bass, Width (Mitte/Seite) ----------
                float m = 0.5f * (y[0] + y[1]);
                float s = 0.5f * (y[0] - y[1]);
                if (monoOn)   // Seite: zwei Butterworth-Hochpaesse (LR4); Mitte: der Allpass derselben Weiche
                {
                    s = biquad(sideC, sideZ[1], biquad(sideC, sideZ[0], s));
                    m = biquad(midC, midZ, m);
                }
                s *= wSm;
                y[0] = (m + s) * pushSm;
                y[1] = (m - s) * pushSm;

                // ---------- Limiter: Spitzen messen (4x), Vorausschau, Gain ----------
                const float pk = std::max(peak4(0, y[0]), peak4(1, y[1]));
                const float req = pk > ceil ? ceil / pk : 1.0f;
                const float h = slidingMin(req);
                // Release: nach oben langsam, nach unten sofort (bleibt so immer <= h)
                float rc;
                if (relFix >= 0.0f) rc = relFix;
                else
                {
                    act += actCoef * ((h < 0.989f ? 1.0f : 0.0f) - act);
                    rc = rcFast + act * (rcSlow - rcFast);
                }
                relG = h < relG ? h : relG + rc * (h - relG);
                const float g = boxAverage(relG) * 0.99999f;
                minG = std::min(minG, g);

                dlyY[0][wIdx] = y[0];
                dlyY[1][wIdx] = y[1];
                const int yIdx = (wIdx - DELAY_Y) & (DLY - 1);
                const float l0 = dlyY[0][yIdx] * g, l1 = dlyY[1][yIdx] * g;
                o[0] += fade * (l0 - o[0]);
                o[1] += fade * (l1 - o[1]);
            }
            wIdx = (wIdx + 1) & (DLY - 1);

            meter(o[0], o[1]);
            L[i] = o[0];
            R[i] = o[1];
        }
        const float gr = minG < 1.0f ? 20.0f * std::log10(std::max(minG, 1e-6f)) : 0.0f;
        grBlock = run ? gr : 0.0f;
    }

private:
    // Ring, Fenster, Detektor-Verzoegerung (der Messwert zur Zeit n gilt fuer x[n-4..n-3]), Taps je Phase.
    // Nach Minimum und Mittelwert ueber WIN liegt der Gain zur Zeit t unter dem Bedarf von t-WIN+1,
    // also unter dem von x[t-WIN+1-DF]: um genau so viel wird das Signal verzoegert.
    enum { DLY = 128, WIN = 61, DF = 4, IP = 8 };
    enum { DELAY_Y = WIN - 1 + DF };
    static_assert((int)DELAY_Y == (int)LOOK, "Vorausschau und Fenster passen nicht zusammen");
    static constexpr float kPi = 3.14159265358979f;
    static constexpr float kDenorm = 1.0e-20f;

    struct Coefs { float b0, b1, b2, a1, a2; };

    float coef(float seconds) const { return 1.0f - std::exp(-1.0f / (seconds * fs)); }
    static inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
    static inline float clamp(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

    static inline float biquad(const Coefs& c, float* z, float x)   // Direktform II transponiert
    {
        const float y = c.b0 * x + z[0];
        z[0] = c.b1 * x - c.a1 * y + z[1];
        z[1] = c.b2 * x - c.a2 * y + kDenorm;
        return y;
    }

    void shelf(float db, float f0, bool high, Coefs& c) const   // RBJ-Kochbuch, Steilheit S = 1
    {
        const float A = std::pow(10.0f, db / 40.0f);
        const float w = 2.0f * kPi * std::min(f0, 0.45f * fs) / fs;
        const float cw = std::cos(w), sw = std::sin(w);
        const float al = sw / 2.0f * std::sqrt(2.0f);
        const float sA = 2.0f * std::sqrt(A) * al;
        float b0, b1, b2, a0, a1, a2;
        if (!high)
        {
            b0 = A * ((A + 1) - (A - 1) * cw + sA);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sA);
            a0 = (A + 1) + (A - 1) * cw + sA;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sA;
        }
        else
        {
            b0 = A * ((A + 1) + (A - 1) * cw + sA);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sA);
            a0 = (A + 1) - (A - 1) * cw + sA;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sA;
        }
        c = { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }

    void allpass(float f0, Coefs& c) const    // 2-Pol-Allpass, Q 0.707: die Phase der LR4-Weiche
    {
        const float w = 2.0f * kPi * clamp(f0, 10.0f, 0.45f * fs) / fs;
        const float cw = std::cos(w), al = std::sin(w) / (2.0f * 0.70710678f);
        const float a0 = 1 + al;
        c = { (1 - al) / a0, -2 * cw / a0, (1 + al) / a0, -2 * cw / a0, (1 - al) / a0 };
    }

    void highpass(float f0, Coefs& c) const   // 2-Pol-Butterworth fuer die Seite
    {
        const float w = 2.0f * kPi * clamp(f0, 10.0f, 0.45f * fs) / fs;
        const float cw = std::cos(w), al = std::sin(w) / (2.0f * 0.70710678f);
        const float a0 = 1 + al;
        c = { (1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0 };
    }

    // 4x-Interpolation nur fuer die Messung: die drei Zwischenwerte zwischen x[n-4] und x[n-3]
    // (gefensterter Sinc, 8 Taps je Phase). Liefert die groesste Spitze um x[n-4..n-3].
    void initInterp()
    {
        for (int ph = 0; ph < 3; ++ph)
        {
            const double f = (ph + 1) * 0.25;
            double sum = 0;
            for (int k = 0; k < IP; ++k)
            {
                const double t = (double)(k - 3) - f;    // Abstand des Taps x[n-7+k] vom Zwischenpunkt
                const double sx = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
                const double wv = std::cos(M_PI * 0.5 * t / 4.5);   // Kosinusfenster ueber +-4.5
                ipc[ph][k] = (float)(sx * wv * wv);
                sum += ipc[ph][k];
            }
            for (int k = 0; k < IP; ++k) ipc[ph][k] = (float)(ipc[ph][k] / sum);
        }
    }
    inline float peak4(int c, float x)
    {
        float* h = hist[c];
        h[tIdx[c] & 7] = x;
        const uint32_t n = tIdx[c]++;
        auto at = [&](int k) { return h[(n - 7 + k) & 7]; };   // k = 0 .. 7 -> x[n-7] .. x[n]
        float p = std::max(std::fabs(at(3)), std::fabs(at(4)));
        for (int ph = 0; ph < 3; ++ph)
        {
            float a = 0.0f;
            for (int k = 0; k < IP; ++k) a += ipc[ph][k] * at(k);
            p = std::max(p, std::fabs(a));
        }
        return p;
    }

    // gleitendes Minimum ueber WIN Samples (monotone Warteschlange)
    inline float slidingMin(float v)
    {
        const uint32_t now = cnt++;
        while (dqTail != dqHead && dqVal[(dqTail - 1) & (DLY - 1)] >= v) --dqTail;
        dqVal[dqTail & (DLY - 1)] = v;
        dqPos[dqTail & (DLY - 1)] = now;
        ++dqTail;
        while (now - dqPos[dqHead & (DLY - 1)] >= (uint32_t)WIN) ++dqHead;
        return dqVal[dqHead & (DLY - 1)];
    }
    // gleitender Mittelwert ueber WIN Samples: der Gain erreicht das Minimum genau, wenn die Spitze kommt
    inline float boxAverage(float v)
    {
        const int k = boxCnt;
        boxCnt = (boxCnt + 1) % WIN;
        boxSum += (double)v - boxBuf[k];
        boxBuf[k] = v;
        if (boxCnt == 0)   // Rundungsfehler nicht wachsen lassen
        {
            double s = 0;
            for (int j = 0; j < WIN; ++j) s += boxBuf[j];
            boxSum = s;
        }
        return (float)(boxSum / WIN);
    }

    // ---------- Lautheit (BS.1770: K-Filter, 100-ms-Bloecke, 3 s Short-Term) ----------
    void initKWeighting()
    {
        // Koeffizienten fuer jede Abtastrate (wie libebur128)
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(M_PI * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1.0 + K / Q + K * K;
        k1 = { (float)((Vh + Vb * K / Q + K * K) / a0), (float)(2.0 * (K * K - Vh) / a0), (float)((Vh - Vb * K / Q + K * K) / a0),
               (float)(2.0 * (K * K - 1.0) / a0), (float)((1.0 - K / Q + K * K) / a0) };
        f0 = 38.13547087602444; Q = 0.5003270373238773;
        K = std::tan(M_PI * f0 / fs);
        a0 = 1.0 + K / Q + K * K;
        k2 = { 1.0f, -2.0f, 1.0f, (float)(2.0 * (K * K - 1.0) / a0), (float)((1.0 - K / Q + K * K) / a0) };
    }
    inline void meter(float l, float r)
    {
        const float a = biquad(k2, kz[0][1], biquad(k1, kz[0][0], l));
        const float b = biquad(k2, kz[1][1], biquad(k1, kz[1][0], r));
        blockAcc += (double)a * a + (double)b * b;
        if (++blockCnt >= blockLen)
        {
            blocks[blockIdx] = blockAcc / blockLen;
            blockIdx = (blockIdx + 1) % NBLK;
            if (blocksFilled < NBLK) ++blocksFilled;
            blockAcc = 0.0; blockCnt = 0;
            double s = 0.0;
            for (int j = 0; j < NBLK; ++j) s += blocks[j];
            s /= NBLK;   // die ersten 3 s zaehlen fehlende Bloecke als Stille
            lufs = s > 1e-10 ? (float)(-0.691 + 10.0 * std::log10(s)) : -100.0f;
        }
    }

    enum { NBLK = 30 };

    float fs = 44100.0f, smooth = 0.0f, fadeStep = 0.0f, actCoef = 0.0f, rcFast = 0.0f, rcSlow = 0.0f;
    bool  first = true;
    float fade = 0.0f;
    float lowSm = 0.0f, highSm = 0.0f, wSm = 1.0f, pushSm = 1.0f, sideHz = -1.0f;
    Coefs lowC{1, 0, 0, 0, 0}, highC{1, 0, 0, 0, 0}, sideC{1, 0, 0, 0, 0}, midC{1, 0, 0, 0, 0}, k1{1, 0, 0, 0, 0}, k2{1, 0, 0, 0, 0};
    float eq[2][2][2] = {};
    float sideZ[2][2] = {}, midZ[2] = {};

    float dlyX[2][DLY] = {}, dlyY[2][DLY] = {};
    int   wIdx = 0;

    float ipc[3][IP] = {};
    float hist[2][8] = {};
    uint32_t tIdx[2] = {0, 0};

    float dqVal[DLY] = {};
    uint32_t dqPos[DLY] = {};
    uint32_t dqHead = 0, dqTail = 0, cnt = 0;
    float boxBuf[WIN] = {};
    double boxSum = 0.0;
    int   boxCnt = 0;
    float relG = 1.0f, act = 0.0f;
    float grBlock = 0.0f;

    float kz[2][2][2] = {};
    double blocks[NBLK] = {}, blockAcc = 0.0;
    int   blockLen = 4410, blockCnt = 0, blockIdx = 0, blocksFilled = 0;
    float lufs = -100.0f;
};

} // namespace wumms
