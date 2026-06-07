#ifndef LILBRIMSTONE_SAW_H
#define LILBRIMSTONE_SAW_H

#include <math.h>

class PolyBLEPSaw {
private:
    float phase;
    float sampleRate;
    float phaseInc; // Base increment per sample (0.0 to 1.0)

    // The PolyBLEP Residual Function
    // Smoothing the discontinuity at phase wrap (0.0 / 1.0)
    inline float get_blep_residual(float t, float dt) {
        if (t < dt) { 
            t /= dt; 
            return t + t - t * t - 1.0f; 
        } 
        else if (t > 1.0f - dt) { 
            t = (t - 1.0f) / dt; 
            return t * t + t + t + 1.0f; 
        }
        return 0.0f;
    }

public:
    PolyBLEPSaw() : phase(0.0f), sampleRate(44100.0f), phaseInc(0.001f) {}

    void init(float sr) {
        sampleRate = sr;
        phase = 0.0f;
        phaseInc = 0.0f;
    }

    void reset() {
        phase = 0.0f;
    }

    void setPhase(float p) {
        phase = p;
    }

    // Set Base Frequency
    void setFrequency(float freq) {
        phaseInc = freq / sampleRate;
    }
    
    // Get Current Phase Increment (useful for checking logic)
    float getPhaseInc() const {
        return phaseInc;
    }

    // Process one sample
    // freqMult: Multiplier for FM/Jitter. 1.0 = No Mod. 
    inline float next(float freqMult = 1.0f) {
        float dt = phaseInc * freqMult;
        
        // 1. Generate Naive Sawtooth (-1.0 to 1.0)
        float output = (2.0f * phase) - 1.0f;

        // 2. Subtract PolyBLEP Residual to fix aliasing
        output -= get_blep_residual(phase, dt);

        // 3. Increment Phase
        phase += dt;
        if (phase >= 1.0f) phase -= 1.0f;

        return output;
    }
};

#endif