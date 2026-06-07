/*
 * LilBrimstone Square Generator
 * 
 * Features:
 * - 4x Upsampling (PolyBLEP Anti-Aliasing)
 * - TPT Resonant Lowpass (Fixed Tone)
 * - DC Blocker / Sag (Fixed Drift)
 * - Analog Unipolar Generation (0v - 2v) with Float Center
 */

#ifndef LILBRIMSTONE_SQUARE_H
#define LILBRIMSTONE_SQUARE_H

#include <math.h>

#define SQ_OVERSAMPLE 4

class SquareVoice {
public:
    // Core parameters
    float sampleRate;
    float phase;
    float phaseInc;
    float velocity; 

    // Filter States
    float s1, s2;           // TPT Lowpass
    float prevInputHP;      // DC Blocker Input
    float prevOutputHP;     // DC Blocker Output

    // Optimized Constants
    float g;                // Filter Cutoff
    float R;                // Filter Damping
    float sagCoef;          // DC Blocker Decay
    float invOS;            // 1 / OVERSAMPLE

    SquareVoice() {
        phase = 0.0f;
        s1 = 0.0f; s2 = 0.0f;
        prevInputHP = 0.0f; prevOutputHP = 0.0f;
        velocity = 1.0f;
        phaseInc = 0.01f;
    }

    void init(float sr) {
        sampleRate = (sr > 1.0f) ? sr : 48000.0f;
        invOS = 1.0f / (float)SQ_OVERSAMPLE;

        // --- 1. HARDCODED TONE (Knob 0.43 -> ~10kHz) ---
        float rawTone = 0.43f;
        float mappedTone = 0.8f + (0.2f * rawTone); 
        float cutoff = 50.0f * powf(20000.0f / 50.0f, mappedTone);
        
        g = tanf(3.14159265f * cutoff / (sampleRate * (float)SQ_OVERSAMPLE));

        // --- 2. HARDCODED SAG (Knob 0.1) ---
        float s_val = 0.1f;
        s_val = s_val * s_val * s_val * s_val; 
        sagCoef = 0.99995f - (s_val * 0.00095f);

        // --- 3. HARDCODED OVERSHOOT (Knob 0.0 -> Q=0.707) ---
        float ovr = 0.0f;
        float Q = 0.707f + (ovr * 3.3f); 
        R = 1.0f / (2.0f * Q);
    }
    
    // Set frequency and update phase increment (LFO Mod support)
    void setFrequency(float freq) {
        float inc = freq / (sampleRate * (float)SQ_OVERSAMPLE);
        if (inc >= 0.5f) inc = 0.49f;
        phaseInc = inc;
    }

    void setPhase(float p) {
        phase = p;
    }

    // --- Main Audio Method ---
    // pulseWidthControl: 0.0 (Square) to 1.0 (Thin)
    inline float process(float pulseWidthControl) {
        // Map Control
        float scaledControl = pulseWidthControl * 0.93f;
        float targetWidth = 0.5f - (scaledControl * 0.5f);

        // Dynamic Clamping
        float minW = phaseInc * 1.5f;
        if (targetWidth < minW) targetWidth = minW;
        if (targetWidth > 0.5f) targetWidth = 0.5f;

        float frameOutput = 0.0f;

        // --- 4x OVERSAMPLING LOOP ---
        for (int k = 0; k < SQ_OVERSAMPLE; ++k) {
            
            float rawValue = 0.0f;

            // 1. Unipolar Generation (0v / 2v)
            if (phase < targetWidth) rawValue = 2.0f; 
            else rawValue = 0.0f;

            // 2. PolyBLEP Anti-Aliasing (Scaled 2x for Unipolar step)
            rawValue += 2.0f * _poly_blep(phase, phaseInc);
            
            float tAtFall = phase - targetWidth;
            if (tAtFall < 0.0f) tAtFall += 1.0f;
            rawValue -= 2.0f * _poly_blep(tAtFall, phaseInc);

            // Velocity applied here before filter
            float sig = rawValue * velocity; 

            // 3. TPT SVF Lowpass (Tone)
            float hp = (sig - (2.0f * R + g) * s1 - s2) / (1.0f + 2.0f * R * g + g * g);
            float bp = g * hp + s1;
            float lp = g * bp + s2;
            s1 = g * hp + bp;
            s2 = g * bp + lp;
            sig = lp;

            // 4. Sag / DC Blocker
            float output = sig - prevInputHP + (sagCoef * prevOutputHP);
            
            if (output > -1e-18f && output < 1e-18f) output = 0.0f;

            prevInputHP = sig;
            prevOutputHP = output;

            // Increment Phase
            phase += phaseInc;
            if (phase >= 1.0f) phase -= 1.0f;

            frameOutput += output;
        }

        // --- DECIMATION ---
        return frameOutput * invOS;
    }

private:
    inline float _poly_blep(float t, float dt) {
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
};

#endif // LILBRIMSTONE_SQUARE_H