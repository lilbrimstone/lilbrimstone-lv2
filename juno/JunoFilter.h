#pragma once
#include <cmath>
#include <algorithm>
#include "JunoDsps.h"

class JunoFilter {
private:
    float s1, s2, s3, s4;
    float alpha, alpha2, k, comp; 
    float fs;
    int osRatio;
    unsigned int seed;
    
public:
    JunoFilter() : s1(0), s2(0), s3(0), s4(0), 
                   alpha(0.0f), alpha2(0.0f), k(0.0f), comp(1.0f),
                   fs(48000.0f), osRatio(1), seed(54321) {}
    
    void init(float sampleRate) {
        fs = sampleRate;
        s1 = s2 = s3 = s4 = 0.0f;
        osRatio = 1;
        seed = 54321;
        calcCoeffs(1.0f, 0.0f);
    }
    
    void setOversample(int ratio) { osRatio = ratio; }
    
    inline void calcCoeffs(float cutoff01, float resRaw) {
        float effectiveFs = fs * osRatio;
        
        // Fast Exp2f for Frequency
        float fc = 20.0f * exp2f(cutoff01 * 9.96578428f);
        if (fc > effectiveFs * 0.45f) fc = effectiveFs * 0.45f;
        
        float g = tanf(3.14159265f * fc / effectiveFs);
        
        // BUG FIX: Input 'resRaw' is already scaled 0..4.25 by Plugin. 
        // Do not multiply by 4.0 again!
        k = resRaw; 
        if (k > 4.0f) k = 4.0f;
        
        alpha = g / (1.0f + g);
        alpha2 = alpha * 2.0f;
        comp = 1.0f + (k * 0.5f);
    }
    
    inline float process(float in) {
        float noise = getNoise(seed) * 1e-6f;
        in += noise;
        
        float fb = std::clamp(s4, -1.5f, 1.5f);
        fb = fb * (1.5f - 0.5f * fb * fb);
        
        float input = in - (k * fb);
        
        // Input Soft Clip
        if (input > 2.5f) input = 2.5f;
        else if (input < -2.5f) input = -2.5f;
        else input = input - (input * input * input) * 0.05333f;

        // 4-Pole Ladder
        s1 += (input - s1) * alpha2;
        s2 += (s1 - s2) * alpha2;
        s3 += (s2 - s3) * alpha2;
        s4 += (s3 - s4) * alpha2;
        
        return s4 * comp;
    }
};