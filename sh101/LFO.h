#pragma once
#include <cmath>
#include <cstdlib>
#include <cstdint>

struct SH101LFO {
    float phase = 0.0f;
    float sampleRate = 48000.0f;
    float sAndH = 0.0f;     // Sample & Hold value (updates once per cycle)
    float noiseSample = 0.0f; // Audio-rate noise
    
    // LCG Random State
    uint32_t rngState = 0xDEADBEEF;

    void init(float sr) {
        sampleRate = sr;
        phase = 0.0f;
        sAndH = 0.0f;
    }

    // Advances phase. Returns 'true' if cycle wrapped (for Triggering Envelopes)
    bool step(float freqHz) {
        float inc = freqHz / sampleRate;
        phase += inc;
        
        bool wrapped = false;
        if (phase >= 1.0f) {
            phase -= 1.0f;
            wrapped = true;
            // Update S&H only on cycle reset
            sAndH = nextRandom();
        }
        
        // Update Noise constantly
        noiseSample = nextRandom();
        
        return wrapped;
    }
    
    // Fast float Random (-1.0 to 1.0)
    inline float nextRandom() {
        rngState = rngState * 1664525 + 1013904223;
        union { float f; uint32_t i; } pun;
        pun.i = (rngState & 0x007FFFFF) | 0x3F800000; 
        return (pun.f - 1.5f) * 2.0f; 
    }

    inline float getTriangle() {
        // Map 0..1 phase to -1..1 Triangle
        if (phase < 0.5f) {
            return -1.0f + (4.0f * phase);
        } else {
            return 1.0f - (4.0f * (phase - 0.5f));
        }
    }
    
    inline float getSample(int wave) {
        switch (wave) {
            case 0: return getTriangle();
            case 1: return (phase < 0.5f) ? 1.0f : -1.0f; // Square
            case 2: return sAndH;       // Random (Stepped)
            case 3: return noiseSample; // Noise (Continuous)
        }
        return 0.0f;
    }
};