#ifndef LILBRIMSTONE_FILTER_H
#define LILBRIMSTONE_FILTER_H

#include <math.h>
#include <stdint.h>

class SH101Filter {
public:
    SH101Filter() {}

    void init(float sampleRate) {
        fs  = sampleRate;
        fs4 = fs * 4.0f;

        cutoffHz = 1000.0f;
        res01    = 0.0f;

        setDcBlockHz(0.5f);

        rng = 0x12345678u;

        prevRes01 = 0.0f;
        kick = 0.0f;

        reset();
        updateCoeffs();
    }

    void reset() {
        for (int i = 0; i < 4; ++i) s[i] = 0.0f;

        xPrev = 0.0f;
        for (int i = 0; i < DECIM_TAPS; ++i) decimHist[i] = 0.0f;

        dc_x1 = 0.0f;
        dc_y1 = 0.0f;

        kick = 0.0f;
    }

    void setCutoff(float hz) {
        cutoffHz = clampf(hz, 10.0f, 0.49f * fs4);
        updateCoeffs();
    }

    void setResonance(float r) {
        res01 = clampf(r, 0.0f, 1.0f);

        // If user crosses into "self-osc likely" region, add a short kick.
        // This makes it start immediately instead of waiting for noise buildup.
        if (prevRes01 < 0.85f && res01 >= 0.85f) {
            kick = 0.02f; // tiny impulse (will be damped quickly if not needed)
        }
        prevRes01 = res01;

        updateCoeffs();
    }

    inline float process(float x) {
        // 4x oversample (linear interpolation)
        const float dx = 0.25f * (x - xPrev);
        const float x0 = xPrev + dx;
        const float x1 = xPrev + 2.0f * dx;
        const float x2 = xPrev + 3.0f * dx;
        const float x3 = x;
        xPrev = x;

        float y = 0.0f;
        y = decimatePush(process4x(x0));
        y = decimatePush(process4x(x1));
        y = decimatePush(process4x(x2));
        y = decimatePush(process4x(x3));

        if (res01 > 0.05f) {
            y = dcBlock(y);
        }

        y *= outGain;

        // gentle safety clip
        if (y > 6.0f)  y = 6.0f;
        if (y < -6.0f) y = -6.0f;

        return y;
    }

private:
    static const int   DECIM_TAPS = 12;
    static const float PI_F;

    float fs  = 48000.0f;
    float fs4 = 192000.0f;

    float cutoffHz = 1000.0f;
    float res01    = 0.0f;

    float a = 0.0f;
    float k = 0.0f;
    float satAmt = 0.0f;
    float fbSat = 0.0f;
    float outGain = 1.0f;

    float s[4] = {0,0,0,0};
    float xPrev = 0.0f;

    float decimHist[DECIM_TAPS] = {0};

    float dcR = 0.99999f;
    float dc_x1 = 0.0f;
    float dc_y1 = 0.0f;

    uint32_t rng = 0x12345678u;

    float prevRes01 = 0.0f;
    float kick = 0.0f;

private:
    static inline float clampf(float x, float lo, float hi) {
        return (x < lo) ? lo : (x > hi) ? hi : x;
    }

    inline void setDcBlockHz(float hz) {
        const float fc = (hz < 0.01f) ? 0.01f : hz;
        dcR = expf(-2.0f * PI_F * fc / fs);
    }

    inline float frandSigned() {
        rng = rng * 1664525u + 1013904223u;
        const uint32_t v = (rng >> 8) & 0x00FFFFFFu;
        return ((float)v / 8388607.5f) - 1.0f;
    }

    inline void updateCoeffs() {
        fs4 = fs * 4.0f;

        const float fc = clampf(cutoffHz, 10.0f, 0.49f * fs4);
        const float g  = tanf(PI_F * (fc / fs4));
        a = g / (1.0f + g);

        const float r  = res01;
        const float r2 = r * r;
        const float r4 = r2 * r2;

        // self-osc capable
        k = r2 * (5.2f + 3.2f * r); // 0..~8.4

        // reduce loop loss a bit (helps fast start)
        satAmt = 0.6f * r2;         // was 0.75*r2
        fbSat  = 0.25f + 0.75f * r; // slightly less aggressive at low res

        // loudness compensation (same)
        float cNorm = cutoffHz * (1.0f / 40000.0f);
        if (cNorm < 0.0f) cNorm = 0.0f;
        if (cNorm > 1.0f) cNorm = 1.0f;

        outGain = 1.0f + (2.0f * r2) + (1.2f * r4 * cNorm);
        if (outGain > 4.0f) outGain = 4.0f;
    }

    inline float satStage(float x) const {
        if (satAmt <= 1e-6f) return x;
        const float t = tanhf(x);
        return x + satAmt * (t - x);
    }

    inline float satFeedback(float x) const {
        const float drive = 1.0f + 2.0f * fbSat; // 1..3
        return tanhf(x * drive) / drive;
    }

    inline float onePoleTPT(float x, float& st) const {
        const float v = a * (x - st);
        const float y = v + st;
        st = y + v;
        return y;
    }

    inline float process4x(float x) {
        // Continuous tiny seed when resonance is high (fast start, no audible hiss)
        // Scale with res so it disappears at low resonance.
        if (res01 > 0.75f) {
            const float seed = (res01 - 0.75f) * (1.0f / 0.25f); // 0..1
            x += frandSigned() * (2e-4f * seed);                 // was 1e-5
        }

        // One-shot kick that decays quickly (a few ms)
        if (kick > 0.0f) {
            x += kick;
            kick *= 0.85f; // fast decay at 4x rate (~few ms)
            if (kick < 1e-6f) kick = 0.0f;
        }

        const float fb = satFeedback(s[3]);
        float u = x - k * fb;

        // keep headroom sane but not too lossy
        u *= 0.65f;

        float y0 = onePoleTPT(satStage(u),  s[0]);
        float y1 = onePoleTPT(satStage(y0), s[1]);
        float y2 = onePoleTPT(satStage(y1), s[2]);
        float y3 = onePoleTPT(satStage(y2), s[3]);

        return y3 * (1.0f / 0.65f);
    }

    inline float decimatePush(float x4) {
        for (int i = DECIM_TAPS - 1; i > 0; --i) decimHist[i] = decimHist[i - 1];
        decimHist[0] = x4;

        const float c0 = 0.008928571f;
        const float c1 = 0.028571429f;
        const float c2 = 0.064285714f;
        const float c3 = 0.114285714f;
        const float c4 = 0.141071429f;
        const float c5 = 0.142857143f;

        return
            c0 * (decimHist[0]  + decimHist[11]) +
            c1 * (decimHist[1]  + decimHist[10]) +
            c2 * (decimHist[2]  + decimHist[9])  +
            c3 * (decimHist[3]  + decimHist[8])  +
            c4 * (decimHist[4]  + decimHist[7])  +
            c5 * (decimHist[5]  + decimHist[6]);
    }

    inline float dcBlock(float x) {
        const float y = x - dc_x1 + dcR * dc_y1;
        dc_x1 = x;
        dc_y1 = y;
        return y;
    }
};

const float SH101Filter::PI_F = 3.14159265358979323846f;

#endif // LILBRIMSTONE_FILTER_H