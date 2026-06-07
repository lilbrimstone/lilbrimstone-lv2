/*
 * LilBrimstone Reverb — High-quality stereo reverb with 5 algorithms
 * 
 * Rebuilt Architecture: 8x8 Global Householder FDN with Hermite Interpolation.
 */

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <new>

#define PLUGIN_URI "https://github.com/lilbrimstone/reverb"

static const float PI_F = 3.14159265358979323846f;

// ─── Utility ────────────────────────────────────────────────────────────────

static inline float fast_tanh(float x) {
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float db2lin(float db) {
    return exp2f(db * (1.0f / 6.0206f));
}

// ─── Delay Lines & Interpolation ────────────────────────────────────────────

struct DelayLine {
    float* buf;
    int    len;
    int    pos;

    void init(int maxSamples) {
        len = maxSamples;
        buf = (float*)calloc(len, sizeof(float));
        pos = 0;
    }

    void free_mem() {
        if (buf) { free(buf); buf = nullptr; }
    }

    void clear() {
        if (buf) memset(buf, 0, len * sizeof(float));
        pos = 0;
    }

    void write(float v) {
        buf[pos] = v;
        pos++;
        if (pos >= len) pos = 0;
    }

    float read(int delay) const {
        int idx = pos - delay;
        if (idx < 0) idx += len;
        return buf[idx];
    }

    // High-quality Hermite interpolation for lush modulation
    float read_hermite(float delay) const {
        float fidx = (float)pos - delay;
        while (fidx < 0.0f) fidx += (float)len;
        while (fidx >= (float)len) fidx -= (float)len;

        int i1 = (int)fidx;
        float frac = fidx - (float)i1;

        int i0 = i1 - 1; if (i0 < 0) i0 += len;
        int i2 = i1 + 1; if (i2 >= len) i2 -= len;
        int i3 = i1 + 2; if (i3 >= len) i3 -= len;

        float xm1 = buf[i0];
        float x0  = buf[i1];
        float x1  = buf[i2];
        float x2  = buf[i3];

        float c1 = 0.5f * (x1 - xm1);
        float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);

        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }
};

// ─── DSP Building Blocks ────────────────────────────────────────────────────

struct OnePole {
    float z1;
    OnePole() : z1(0.0f) {}
    void clear() { z1 = 0.0f; }

    float lp(float in, float coeff) {
        z1 += coeff * (in - z1);
        return z1;
    }

    float hp(float in, float coeff) {
        z1 += coeff * (in - z1);
        return in - z1;
    }
};

struct DCBlocker {
    float x1, y1;
    DCBlocker() : x1(0), y1(0) {}
    void clear() { x1 = y1 = 0; }
    float process(float in) {
        y1 = in - x1 + 0.9995f * y1;
        x1 = in;
        return y1;
    }
};

struct FixedAllpass {
    DelayLine dl;
    int delay;

    void init(int maxSamples, int fixedDelay) {
        dl.init(maxSamples);
        delay = fixedDelay;
    }
    void free_mem() { dl.free_mem(); }
    void clear() { dl.clear(); }

    float process(float in, float coeff) {
        float delayed = dl.read(delay);
        float v = in - coeff * delayed;
        dl.write(v);
        return delayed + coeff * v;
    }
};

// ─── 8-Channel Algorithm Constants ──────────────────────────────────────────

static const int MAX_PREDELAY = 96000; // ~500ms max safety buffer

// Mutually prime delay lengths (scaled relative to 48k)
static const int TANK_PRIMES[8] = { 1153, 1499, 1861, 2309, 2713, 3163, 3727, 4391 };

// Diffusers to smear transients before the FDN tank (prevent "machine gun" echo)
static const int DIFFUSER_L[4]  = { 131, 211, 331, 461 };
static const int DIFFUSER_R[4]  = { 137, 223, 337, 467 };

// Prime LFO ratios to decorrelate pitch modulation
static const float LFO_RATIOS[8] = { 1.0f, 1.13f, 1.27f, 1.41f, 1.57f, 1.73f, 1.89f, 2.03f };

// ─── Reverb Engine ──────────────────────────────────────────────────────────

struct ReverbEngine {
    float sampleRate;
    float srRatio;

    DelayLine predelayL, predelayR;
    
    FixedAllpass inputApL[4];
    FixedAllpass inputApR[4];

    DelayLine fdn[8];
    OnePole   damp[8];

    OnePole inputLPF_L, inputLPF_R;
    OnePole inputHPF_L, inputHPF_R;
    DCBlocker dcL, dcR;

    float lfoPhases[8];
    
    // Shimmer
    DelayLine shimBufL, shimBufR;
    float shimPhaseA, shimPhaseB;

    void init(float sr) {
        sampleRate = sr;
        srRatio = sr / 48000.0f;

        predelayL.init(MAX_PREDELAY);
        predelayR.init(MAX_PREDELAY);

        int maxTank = (int)(6000.0f * srRatio) + 128;
        
        for (int i = 0; i < 4; i++) {
            inputApL[i].init((int)(1000 * srRatio), (int)(DIFFUSER_L[i] * srRatio));
            inputApR[i].init((int)(1000 * srRatio), (int)(DIFFUSER_R[i] * srRatio));
        }

        for (int i = 0; i < 8; i++) {
            fdn[i].init(maxTank);
            lfoPhases[i] = (float)i * 0.125f; // Distributed phases
        }

        shimBufL.init((int)(sr * 0.15f) + 64);
        shimBufR.init((int)(sr * 0.15f) + 64);
        shimPhaseA = 0.0f;
        shimPhaseB = 0.5f;
    }

    void free_mem() {
        predelayL.free_mem(); predelayR.free_mem();
        for (int i=0; i<4; ++i) { inputApL[i].free_mem(); inputApR[i].free_mem(); }
        for (int i=0; i<8; ++i) fdn[i].free_mem();
        shimBufL.free_mem(); shimBufR.free_mem();
    }

    void clear() {
        predelayL.clear(); predelayR.clear();
        for (int i=0; i<4; ++i) { inputApL[i].clear(); inputApR[i].clear(); }
        for (int i=0; i<8; ++i) { fdn[i].clear(); damp[i].clear(); }
        inputLPF_L.clear(); inputLPF_R.clear();
        inputHPF_L.clear(); inputHPF_R.clear();
        dcL.clear(); dcR.clear();
        shimBufL.clear(); shimBufR.clear();
    }

    float grainShift(DelayLine& buf, float in, float& pA, float& pB, float ratio) {
        float grainSize = sampleRate * 0.06f; 
        buf.write(in);

        float inc = (1.0f - ratio) / grainSize;
        pA += inc; pB += inc;
        if (pA >= 1.0f) pA -= 1.0f;
        if (pA < 0.0f)  pA += 1.0f;
        if (pB >= 1.0f) pB -= 1.0f;
        if (pB < 0.0f)  pB += 1.0f;

        float sA = buf.read_hermite(pA * grainSize + 1.0f);
        float sB = buf.read_hermite(pB * grainSize + 1.0f);

        float envA = 0.5f - 0.5f * cosf(pA * 2.0f * PI_F);
        float envB = 0.5f - 0.5f * cosf(pB * 2.0f * PI_F);

        return sA * envA + sB * envB;
    }

    void process(float inL, float inR, float& outL, float& outR,
                 int algo, float predelayMs, float decaySec,
                 float damping, float sizeParam, float diffusion,
                 float modRate, float modDepth,
                 float lowCutHz, float highCutHz, float width) {

        // ── Algorithm Contexts ──
        float sizeMult = 1.0f;
        float cloudAmt = 0.0f;
        float shimmerAmt = 0.0f;
        float modMult = 1.0f;
        float dampModifier = 1.0f;
        float diffMult = 1.0f;

        switch (algo) {
            case 0: // Room: Slightly larger base lines to prevent metallic ring, heavily damped, extremely low mod, high inherent diffusion
                sizeMult = 0.35f; modMult = 0.1f; dampModifier = 1.5f; diffMult = 1.25f; break;
            case 1: // Hall: Standard large space
                sizeMult = 1.0f; modMult = 0.9f; break;
            case 2: // Plate: Dense, highly modulated, bright (low damping factor)
                sizeMult = 0.5f; modMult = 1.5f; dampModifier = 0.5f; diffMult = 1.3f; break;
            case 3: // Shimmer
                sizeMult = 1.2f; shimmerAmt = 0.55f; break;
            case 4: // Cloud: Infinite, massive scaling
                sizeMult = 1.4f; cloudAmt = clampf(decaySec / 30.0f, 0.0f, 1.0f); break;
        }

        // ── Input filtering ──
        float lpCoeff = 1.0f - expf(-2.0f * PI_F * highCutHz / sampleRate);
        float hpCoeff = 1.0f - expf(-2.0f * PI_F * lowCutHz / sampleRate);
        float fL = inputHPF_L.hp(inputLPF_L.lp(inL, lpCoeff), hpCoeff);
        float fR = inputHPF_R.hp(inputLPF_R.lp(inR, lpCoeff), hpCoeff);

        // Pre-delay
        int pdSamples = std::max(0, std::min((int)(predelayMs * 0.001f * sampleRate), MAX_PREDELAY - 1));
        predelayL.write(fL); predelayR.write(fR);
        float dL = predelayL.read(pdSamples);
        float dR = predelayR.read(pdSamples);

        // ── Diffusers ──
        float diffGain = clampf(diffusion * 0.65f * diffMult, 0.0f, 0.85f);
        for (int i = 0; i < 4; i++) {
            dL = inputApL[i].process(dL, diffGain);
            dR = inputApR[i].process(dR, diffGain);
        }

        // ── 8x8 FDN Core ──
        float fdnOut[8];
        float dampCoeff = clampf(damping * 0.7f * dampModifier, 0.0f, 0.98f);
        
        float realSize = 0.1f + 0.9f * sizeParam;

        for (int i = 0; i < 8; i++) {
            float rate = (modRate / sampleRate) * LFO_RATIOS[i];
            lfoPhases[i] += rate;
            if (lfoPhases[i] >= 1.0f) lfoPhases[i] -= 1.0f;

            float lfoVal = sinf(lfoPhases[i] * 2.0f * PI_F);
            
            float baseLines = (float)TANK_PRIMES[i] * srRatio * sizeMult * realSize;
            float modSamples = modDepth * modMult * 16.0f * srRatio; 
            float readDel = baseLines + lfoVal * modSamples;
            
            readDel = clampf(readDel, 4.0f, (float)(fdn[i].len - 4));

            float fb = expf(-6.9078f * baseLines / (decaySec * sampleRate));
            
            if (cloudAmt > 0.0f) { fb = fb + cloudAmt * (0.995f - fb); }
            fb = clampf(fb, 0.001f, 0.995f); // Limit hard edge of feedback explosion

            float r = fdn[i].read_hermite(readDel);
            r = damp[i].lp(r, dampCoeff);
            
            fdnOut[i] = r * fb;
        }

        // ── Shimmer Injection (+1 Octave) ──
        if (shimmerAmt > 0.0f) {
            float shL = grainShift(shimBufL, fdnOut[0], shimPhaseA, shimPhaseB, 2.0f);
            float shR = grainShift(shimBufR, fdnOut[7], shimPhaseA, shimPhaseB, 2.0f);
            
            fdnOut[0] = (fdnOut[0] + shL * shimmerAmt) * 0.707f;
            fdnOut[7] = (fdnOut[7] + shR * shimmerAmt) * 0.707f;
        }

        // ── 8x8 Householder Matrix ──
        float sum = 0.0f;
        for (int i = 0; i < 8; i++) sum += fdnOut[i];
        
        sum *= 0.25f; // 2/N where N=8
        
        for (int i = 0; i < 8; i++) {
            float modified = fast_tanh(sum - fdnOut[i]); 
            float inputInj = (i < 4) ? (dL * 0.25f) : (dR * 0.25f);
            // 1e-15f completely eliminates Denormal CPU spikes
            fdn[i].write(modified + inputInj + 1e-15f); 
        }

        // ── Final Output Taps ──
        float mixL = fdnOut[0] - fdnOut[2] + fdnOut[4] - fdnOut[6];
        float mixR = fdnOut[1] - fdnOut[3] + fdnOut[5] - fdnOut[7];
        
        mixL *= 0.5f; 
        mixR *= 0.5f;

        // Stereo Width Check
        float mid  = (mixL + mixR) * 0.5f;
        float side = (mixL - mixR) * 0.5f * width;
        outL = dcL.process(mid + side);
        outR = dcR.process(mid - side);

        // Ultimate safety net
        if (!std::isfinite(outL) || !std::isfinite(outR)) {
            outL = 0.0f; outR = 0.0f;
            clear();
        }
    }
};

// ─── Plugin Shell ───────────────────────────────────────────────────────────

enum PortIndex {
    PORT_MIDI_IN = 0, PORT_MIDI_OUT = 1, PORT_IN_L = 2, PORT_IN_R = 3, PORT_OUT_L = 4, PORT_OUT_R = 5,
    PORT_ALGO = 6, PORT_PREDELAY = 7, PORT_DECAY = 8, PORT_DAMPING = 9,
    PORT_SIZE = 10, PORT_DIFFUSION = 11, PORT_MOD_RATE = 12, PORT_MOD_DEPTH = 13,
    PORT_LOW_CUT = 14, PORT_HIGH_CUT = 15, PORT_WIDTH = 16, PORT_MIX = 17, PORT_OUT_GAIN = 18
};

struct Reverb {
    LV2_URID_Map* map;
    const LV2_Atom_Sequence* midiIn;
    LV2_Atom_Sequence* midiOut;

    const float* inL; const float* inR; float* outL; float* outR;
    const float* pAlgo; const float* pPredelay; const float* pDecay;
    const float* pDamping; const float* pSize; const float* pDiffusion;
    const float* pModRate; const float* pModDepth; const float* pLowCut;
    const float* pHighCut; const float* pWidth; const float* pMix; const float* pOutGain;

    float sampleRate;
    int   currentAlgo;
    ReverbEngine engine;

    // Smoothed variables
    float sPredelay, sDecay, sDamping, sSize, sDiffusion;
    float sModRate, sModDepth, sLowCut, sHighCut, sWidth;
};

extern "C" {

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, 
                              const char* bundle_path, const LV2_Feature* const* features) {
    LV2_URID_Map* map = nullptr;
    if (features) {
        for (int i = 0; features[i]; i++) {
            if (!strcmp(features[i]->URI, LV2_URID__map))
                map = (LV2_URID_Map*)features[i]->data;
        }
    }
    if (!map) return nullptr;

    void* ptr = calloc(1, sizeof(Reverb));
    if (!ptr) return nullptr;
    Reverb* p = new(ptr) Reverb();

    p->map = map;
    p->sampleRate = (float)rate;
    p->engine.init((float)rate);
    p->currentAlgo = 0;

    // Default smoothed targets
    p->sPredelay = 20.0f; p->sDecay = 2.0f; p->sDamping = 0.5f;
    p->sSize = 0.5f; p->sDiffusion = 0.7f; p->sModRate = 0.3f; p->sModDepth = 0.2f;
    p->sLowCut = 20.0f; p->sHighCut = 16000.0f; p->sWidth = 1.0f;

    return (LV2_Handle)p;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    Reverb* p = (Reverb*)instance;
    switch (port) {
        case PORT_MIDI_IN: p->midiIn = (const LV2_Atom_Sequence*)data; break;
        case PORT_MIDI_OUT: p->midiOut = (LV2_Atom_Sequence*)data; break;
        case PORT_IN_L:  p->inL = (const float*)data; break;
        case PORT_IN_R:  p->inR = (const float*)data; break;
        case PORT_OUT_L: p->outL = (float*)data; break;
        case PORT_OUT_R: p->outR = (float*)data; break;
        case PORT_ALGO:  p->pAlgo = (const float*)data; break;
        case PORT_PREDELAY: p->pPredelay = (const float*)data; break;
        case PORT_DECAY: p->pDecay = (const float*)data; break;
        case PORT_DAMPING: p->pDamping = (const float*)data; break;
        case PORT_SIZE: p->pSize = (const float*)data; break;
        case PORT_DIFFUSION: p->pDiffusion = (const float*)data; break;
        case PORT_MOD_RATE: p->pModRate = (const float*)data; break;
        case PORT_MOD_DEPTH: p->pModDepth = (const float*)data; break;
        case PORT_LOW_CUT: p->pLowCut = (const float*)data; break;
        case PORT_HIGH_CUT: p->pHighCut = (const float*)data; break;
        case PORT_WIDTH: p->pWidth = (const float*)data; break;
        case PORT_MIX: p->pMix = (const float*)data; break;
        case PORT_OUT_GAIN: p->pOutGain = (const float*)data; break;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    Reverb* p = (Reverb*)instance;

    // --- MIDI PASSTHROUGH ---
    if (p->midiOut) {
        const uint32_t capacity = p->midiOut->atom.size;
        lv2_atom_sequence_clear(p->midiOut);
        if (p->midiIn) {
            p->midiOut->atom.type = p->midiIn->atom.type;
            LV2_ATOM_SEQUENCE_FOREACH(p->midiIn, ev) {
                lv2_atom_sequence_append_event(p->midiOut, capacity, ev);
            }
        }
    }

    int   algo      = (int)clampf(*p->pAlgo, 0.0f, 4.0f);
    float predelay  = clampf(*p->pPredelay, 0.0f, 250.0f);
    float decay     = clampf(*p->pDecay, 0.1f, 30.0f);
    float damping   = clampf(*p->pDamping, 0.0f, 1.0f);
    float size      = clampf(*p->pSize, 0.0f, 1.0f);
    float diffusion = clampf(*p->pDiffusion, 0.0f, 1.0f);
    float modHz     = clampf(*p->pModRate, 0.01f, 5.0f);
    float modDepth  = clampf(*p->pModDepth, 0.0f, 1.0f);
    float loCut     = clampf(*p->pLowCut, 20.0f, 1000.0f);
    float hiCut     = clampf(*p->pHighCut, 1000.0f, 20000.0f);
    float width     = clampf(*p->pWidth, 0.0f, 1.0f);
    float mix       = clampf(*p->pMix, 0.0f, 1.0f);
    float outGainLm = db2lin(clampf(*p->pOutGain, -24.0f, 12.0f));

    if (algo != p->currentAlgo) {
        p->engine.clear();
        p->currentAlgo = algo;
    }

    float smooth = 1.0f - expf(-2.0f * PI_F * 200.0f / p->sampleRate);
    float dryDb = cosf(mix * PI_F * 0.5f);
    float wetDb = sinf(mix * PI_F * 0.5f);

    for (uint32_t i = 0; i < n_samples; i++) {
        p->sPredelay += smooth * (predelay - p->sPredelay);
        p->sDecay    += smooth * (decay - p->sDecay);
        p->sDamping  += smooth * (damping - p->sDamping);
        p->sSize     += smooth * (size - p->sSize);
        p->sDiffusion+= smooth * (diffusion - p->sDiffusion);
        p->sModRate  += smooth * (modHz - p->sModRate);
        p->sModDepth += smooth * (modDepth - p->sModDepth);
        p->sLowCut   += smooth * (loCut - p->sLowCut);
        p->sHighCut  += smooth * (hiCut - p->sHighCut);
        p->sWidth    += smooth * (width - p->sWidth);

        float dryL = p->inL[i];
        float dryR = p->inR[i];
        float wetL, wetR;

        p->engine.process(dryL, dryR, wetL, wetR, algo,
                          p->sPredelay, p->sDecay, p->sDamping, p->sSize, p->sDiffusion,
                          p->sModRate, p->sModDepth, p->sLowCut, p->sHighCut, p->sWidth);

        p->outL[i] = (dryL * dryDb + wetL * wetDb) * outGainLm;
        p->outR[i] = (dryR * dryDb + wetR * wetDb) * outGainLm;
    }
}

static void cleanup(LV2_Handle instance) {
    Reverb* p = (Reverb*)instance;
    p->engine.free_mem();
    p->~Reverb();
    free(p);
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI, instantiate, connect_port, nullptr, run, nullptr, cleanup, nullptr
};

// David's specific Windows/Aarch64 visibility flags
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : nullptr;
}
} // extern "C"