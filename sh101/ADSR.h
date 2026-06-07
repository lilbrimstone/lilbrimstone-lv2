#ifndef LILBRIMSTONE_ADSR_H
#define LILBRIMSTONE_ADSR_H

#include <math.h>

class SH101ADSR {
public:
    enum State { IDLE = 0, ATTACK, DECAY, SUSTAIN, RELEASE };

    SH101ADSR() {}

    inline void init(float sampleRate) {
        fs = (sampleRate > 1.0f) ? sampleRate : 48000.0f;
        // Anti-click VCA smoothing (~0.5ms — fast enough for arp, slow enough for no click)
        vcaCoef = coefFromSeconds_RC(0.0005f);
        // Gate release (~3ms — snappy)
        gateRelCoef = coefFromSeconds_60dB(0.003f);
        reset();
    }

    inline void reset() {
        state = IDLE;
        gate = false;
        env = 0.0f;
        gateEnv = 0.0f;
        vca = 0.0f;
        sustain01 = 0.0f;
        aCoef = 1.0f;
        dCoef = 1.0f;
        rCoef = 1.0f;
    }

    // --- Events ---
    inline void noteOn() { 
        gate = true;  
        state = ATTACK;
        // Hard retrigger: reset envelope to near-zero so every note
        // gets a clean, identical attack ramp. Without this, rapid
        // note-off/note-on pairs (from an arpeggiator) produce uneven
        // amplitudes because the envelope is at different levels when
        // retriggered depending on how far release progressed.
        env = 0.0f;
    }
    
    inline void noteOff() { 
        gate = false; 
        if (state != IDLE) state = RELEASE; 
    }

    inline bool isActive() const { return state != IDLE || vca > 0.001f; }
    
    // --- Setters (Knobs 0..10) ---
    
    // Attack: 0 -> 6.0s
    inline void setAttackKnob(float k0_10) {
        float sec = knobToSecondsPower(k0_10, 6.0f, 2.0f);
        if (sec < 0.0001f) aCoef = 1.0f; 
        else aCoef = coefFromSeconds_60dB(sec);
    }

    // Decay: 0 -> 20.0s
    inline void setDecayKnob(float k0_10) {
        float sec = knobToSecondsPower(k0_10, 20.0f, 2.0f);
        if (sec < 0.0001f) dCoef = 1.0f;
        else dCoef = coefFromSeconds_60dB(sec);
    }

    inline void setSustainKnob(float k0_10) {
        float s = k0_10 * 0.1f;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        sustain01 = s;
    }

    // Release: 0 -> 20.0s
    inline void setReleaseKnob(float k0_10) {
        float sec = knobToSecondsPower(k0_10, 20.0f, 2.0f);
        if (sec < 0.0001f) rCoef = 1.0f;
        else rCoef = coefFromSeconds_60dB(sec);
    }

    // --- Processing ---
    
    // 1. Process Main ADSR (For Filter)
    inline float processEnv() {
        switch (state) {
            case IDLE:
                env = 0.0f;
                break;

            case ATTACK:
                env += (1.01f - env) * aCoef;
                if (env >= 1.0f) { 
                    env = 1.0f; 
                    state = DECAY; 
                }
                break;

            case DECAY:
                env += (sustain01 - env) * dCoef;
                if (fabsf(env - sustain01) <= 1e-4f) { 
                    env = sustain01; 
                    state = SUSTAIN; 
                }
                break;

            case SUSTAIN:
                env = sustain01;
                if (!gate) state = RELEASE;
                break;

            case RELEASE:
                env += (-0.01f - env) * rCoef;
                if (env <= 1e-5f) { 
                    env = 0.0f; 
                    state = IDLE; 
                }
                break;
        }

        if (env < 0.0f) env = 0.0f;
        if (env > 1.0f) env = 1.0f;

        return env;
    }

    // 2a. Amp Mode: ENV (Uses the same envelope we just calculated)
    inline float processAmpFromEnv(float envInput) {
        vca += (envInput - vca) * vcaCoef;
        if (vca < 1e-9f) vca = 0.0f;
        return vca;
    }

    // 2b. Amp Mode: GATE (Uses independent simple envelope)
    inline float processGateAmp() {
        if (gate) {
            gateEnv = 1.0f; 
        } else {
            gateEnv += (0.0f - gateEnv) * gateRelCoef;
            if (gateEnv < 1e-5f) gateEnv = 0.0f;
        }
        vca += (gateEnv - vca) * vcaCoef;
        return vca;
    }

private:
    float fs = 48000.0f;
    State state = IDLE;
    bool gate = false;
    
    float env = 0.0f;
    float gateEnv = 0.0f;
    
    float vca = 0.0f;
    
    float sustain01 = 0.0f;
    float aCoef = 1.0f; 
    float dCoef = 1.0f;
    float rCoef = 1.0f;
    
    float vcaCoef = 1.0f;
    float gateRelCoef = 1.0f;

    inline float knobToSecondsPower(float k, float maxSec, float curve) {
        float x = k * 0.1f; 
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return maxSec;
        return maxSec * powf(x, curve);
    }

    inline float coefFromSeconds_60dB(float sec) const {
        if (sec <= 0.0f) return 1.0f;
        float samps = sec * fs;
        if (samps < 1.0f) return 1.0f;
        return 1.0f - expf(-6.907755f / samps);
    }

    inline float coefFromSeconds_RC(float sec) const {
        if (sec <= 0.0f) return 1.0f;
        float samps = sec * fs;
        if (samps < 1.0f) return 1.0f;
        return 1.0f - expf(-1.0f / samps);
    }
};

#endif // LILBRIMSTONE_ADSR_H