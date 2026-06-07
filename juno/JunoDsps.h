#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <cstring>

static inline float getNoise(unsigned int& seed) {
    seed = seed * 1664525 + 1013904223;
    return ((float)(seed & 0x7FFFFFFF) * 4.65661287e-10f) * 2.0f - 1.0f;
}

static inline float SafetyClamp(float x) {
    if (x > 2.0f) return 2.0f;
    if (x < -2.0f) return -2.0f;
    return x;
}

static inline float mapCurve(float x, float min, float max, float tension) {
    return min + (max - min) * powf(x, tension);
}

static inline float fast_sin(float x) {
    return sinf(x);
}

class JunoHighPass {
private:
    float x1, y1;
    float alpha;
public:
    JunoHighPass() : x1(0), y1(0), alpha(1.0f) {}
    
    void init(float sampleRate) { x1 = y1 = 0.0f; set(0.0f, sampleRate); }
    
    void set(float param, float fs) { 
        if (param < 0.01f) { alpha = 0.0f; } 
        else {
            float fc = 40.0f + param * 560.0f; 
            float rc = 1.0f / (6.2831853f * fc);
            float dt = 1.0f / fs; 
            alpha = rc / (rc + dt);
        }
    }
    
    void setAlpha(float a) { alpha = a; }
    
    float getAlpha(float param, float fs) {
        if (param < 0.01f) return 0.0f;
        float fc = 40.0f + param * 560.0f; 
        float rc = 1.0f / (6.2831853f * fc);
        float dt = 1.0f / fs; 
        return rc / (rc + dt);
    }
    
    inline float process(float in) {
        if (alpha == 0.0f) return in;
        float out = alpha * (y1 + in - x1);
        if (fabs(out) < 1e-8f) out = 0.0f;
        x1 = in; y1 = out; return out;
    }
};

class JunoEnvelope {
public:
    enum State { IDLE, ATTACK, DECAY, SUSTAIN, RELEASE };
    State state = IDLE;
    float currentLevel = 0.0f;
    float s = 1.0f;
    float aCoeff = 0.0f, dCoeff = 0.0f, rCoeff = 0.0f;
    float fs = 48000.0f;
    
    void init(float sampleRate) { fs = sampleRate; state = IDLE; currentLevel = 0.0f; }
    
    inline void setCoeffs(float _a, float _d, float _s, float _r) {
        aCoeff = _a; dCoeff = _d; s = _s; rCoeff = _r;
    }

    void trigger(bool gate) {
        if (gate) state = ATTACK;
        else if (state != IDLE) state = RELEASE;
    }
    
    inline float process() {
        if (state == IDLE) return 0.0f;
        if (state == ATTACK) {
            currentLevel += (1.01f - currentLevel) * aCoeff;
            if (currentLevel >= 1.0f) { currentLevel = 1.0f; state = DECAY; }
        } else if (state == DECAY) {
            currentLevel += (s - currentLevel) * dCoeff;
            if (currentLevel <= s + 0.001f) { 
                 if (state == DECAY && currentLevel < s) currentLevel = s; 
                 state = SUSTAIN;
            }
        } else if (state == SUSTAIN) {
            currentLevel = s;
        } else if (state == RELEASE) {
            currentLevel += (0.0f - currentLevel) * rCoeff;
            if (currentLevel < 0.001f) { currentLevel = 0.0f; state = IDLE; }
        }
        return currentLevel;
    }
};

class JunoChorus {
public:
    int mode = 0;
    
private:
    static const int bufferSize = 4096;
    float bufferL[bufferSize]; float bufferR[bufferSize];
    int writePos = 0;
    float lfoPhase = 0.0f;
    float fs = 48000.0f;
    
    // Exact specs from datasheet
    float currentRateHz = 0.0f;
    float currentCenterS = 0.0f;
    float currentDepthS = 0.0f;
    bool isMono = false;

public:
    void init(float sampleRate) {
        fs = sampleRate;
        for(int i = 0; i < bufferSize; i++) { bufferL[i] = 0.0f; bufferR[i] = 0.0f; }
        writePos = 0;
        lfoPhase = 0.0f;
    }
    
    void setMode(int m) {
        mode = m;
        if (m == 1) { 
            // Chorus 1: 0.513 Hz, 1.66ms - 5.35ms
            currentRateHz = 0.513f;
            currentCenterS = 0.003505f; // (Min+Max)/2
            currentDepthS = 0.001845f;  // (Max-Min)/2
            isMono = false;
        } else if (m == 2) { 
            // Chorus 2: 0.863 Hz, 1.66ms - 5.35ms
            currentRateHz = 0.863f;
            currentCenterS = 0.003505f;
            currentDepthS = 0.001845f;
            isMono = false;
        } else if (m == 3) { 
            // Chorus 1+2: 9.75 Hz, 3.3ms - 3.7ms
            currentRateHz = 9.75f;
            currentCenterS = 0.0035f;
            currentDepthS = 0.0002f; // Very shallow depth
            isMono = true;
        }
    }
    
    inline void process(float inL, float inR, float& outL, float& outR) {
        if (mode == 0) { outL = inL; outR = inR; return; }
        
        // 1. Advance LFO
        lfoPhase += (currentRateHz / fs);
        if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        
        // 2. Triangle Wave (-1.0 to 1.0)
        float lfo = 2.0f * fabsf(2.0f * lfoPhase - 1.0f) - 1.0f;
        
        // 3. Convert Time to Samples
        float centerSamples = currentCenterS * fs;
        float depthSamples = currentDepthS * fs;
        
        // 4. Calculate Delays (Right channel is Inverted Phase)
        float delayL = centerSamples + (lfo * depthSamples);
        float delayR = centerSamples - (lfo * depthSamples); // 180 deg
        if (isMono) delayR = delayL; // No phase diff in mono mode usually, or could be same
        
        // 5. Interpolate Left
        int idxL = (int)delayL; float fracL = delayL - idxL;
        int pLa = (writePos - idxL + bufferSize) & 0xFFF;
        int pLb = (pLa - 1 + bufferSize) & 0xFFF;
        float wetL = bufferL[pLa] * (1.0f - fracL) + bufferL[pLb] * fracL;

        // 6. Interpolate Right
        int idxR = (int)delayR; float fracR = delayR - idxR;
        int pRa = (writePos - idxR + bufferSize) & 0xFFF;
        int pRb = (pRa - 1 + bufferSize) & 0xFFF;
        float wetR = bufferR[pRa] * (1.0f - fracR) + bufferR[pRb] * fracR;
        
        // 7. Write to Buffer
        bufferL[writePos] = inL; bufferR[writePos] = inR;
        writePos = (writePos + 1) & 0xFFF;
        
        // 8. Output Mix
        if (isMono) {
            // Sum to mono for that Leslie rotary feel
            float mix = (wetL + wetR) * 0.5f;
            outL = inL * 0.5f + mix * 0.5f;
            outR = inR * 0.5f + mix * 0.5f;
        } else {
            // Wide Stereo
            outL = inL * 0.5f + wetL * 0.5f;
            outR = inR * 0.5f + wetR * 0.5f;
        }
    }
};