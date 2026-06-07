#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <cmath>
#include <cstdint>
#include <algorithm> 
#include <cstring>

#include "JunoFilter.h"
#include "JunoDsps.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/juno"
#define SAFE_REF(p) (ports[p] ? *ports[p] : 0.0f)
#define NORM10(x) ((x) * 0.1f)
#define BLOCK_SIZE 16

enum PortIndex {
    PORT_MIDI = 0, 
    PORT_PULSE_ON = 1, PORT_SAW_ON = 2, PORT_SUB_LEVEL = 3, PORT_NOISE_LEVEL = 4,
    PORT_PWM_VAL = 5, PORT_PWM_MODE = 6, PORT_DCO_LFO = 7, PORT_HPF = 8,
    PORT_VCF_FREQ = 9, PORT_VCF_RES = 10, PORT_VCF_ENV = 11, PORT_VCF_ENV_POL = 12,
    PORT_VCF_LFO = 13, PORT_VCF_KYBD = 14, PORT_VCA_MODE = 15,
    PORT_ENV_A = 16, PORT_ENV_D = 17, PORT_ENV_S = 18, PORT_ENV_R = 19,
    PORT_LFO_RATE = 20, PORT_LFO_DELAY = 21, PORT_CHORUS = 22,
    PORT_MASTER_VOL = 23, PORT_PORTAMENTO = 24, PORT_VOICE_MODE = 25,
    PORT_DETUNE = 26, PORT_SPREAD = 27, PORT_OVERSAMPLE = 28,
    PORT_OUT_L = 29, PORT_OUT_R = 30
};

struct JunoURIs {
    LV2_URID atom_Sequence;
    LV2_URID atom_Long;
    LV2_URID midi_MidiEvent;
};

class JunoPlugin {
private:
    float* ports[31]; 
    float fs;
    
    LV2_URID_Map* map;
    JunoURIs uris;
    JunoChorus chorus;
    
    struct Voice {
        bool active = false;
        int note = -1;
        float freq = 0.0f;
        float phase = 0.0f;
        float subPhase = 0.0f; 
        float currentFreq = 0.0f;
        float dcoHpState = 0.0f;
        float detuneRatio = 0.0f;
        float pan = 0.0f;
        float gateSmoother = 0.0f;
        
        // Cache
        float blockPhaseIncOS = 0.0f;
        float blockPwmWidth = 0.5f;
        float blockPanL = 0.707f;
        float blockPanR = 0.707f;
        float lastFc = -1.0f;
        float lastRes = -1.0f;
        float lastPanPos = -999.0f;
        
        JunoEnvelope env;
        JunoFilter vcf;
        JunoHighPass hpf; 
    };
    
    static const int NUM_VOICES = 6;
    Voice voices[NUM_VOICES];
    int voiceRoundRobin = 0;
    float lfoPhase = 0.0f;
    float lfoDelaySamples = 0.0f;
    unsigned int noiseSeed = 12345;
    int currentMode = 0;
    
    static const int MAX_STACK = 16;
    int noteStack[MAX_STACK];
    int stackSize = 0;
    
public:
    JunoPlugin(double sampleRate, const LV2_Feature* const* features) : fs((float)sampleRate) {
        for(int i=0; i<31; i++) ports[i] = nullptr; 

        map = nullptr;
        for (int i = 0; features[i]; ++i) {
            if (!strcmp(features[i]->URI, LV2_URID__map)) {
                map = (LV2_URID_Map*)features[i]->data;
            }
        }
        if (map) {
            uris.atom_Sequence = map->map(map->handle, LV2_ATOM__Sequence);
            uris.atom_Long = map->map(map->handle, LV2_ATOM__Long);
            uris.midi_MidiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
        }

        chorus.init(fs);
        for(int i=0; i<NUM_VOICES; i++) {
            voices[i].vcf.init(fs);
            voices[i].env.init(fs);
            voices[i].hpf.init(fs);
            voices[i].lastFc = -1.0f;
            voices[i].lastPanPos = -999.0f;
        }
        stackSize = 0;
    }

    void connect_port(uint32_t port, void* data) {
        if (port < 31) ports[port] = (float*)data;
    }

    // --- MIDI LOGIC ---
    void noteOn(int note, int velocity) {
        lfoDelaySamples = 0.0f;
        
        // --- MONO (2) & LEGATO (3) ---
        if (currentMode == 2 || currentMode == 3) {
            // FIX: Handle Stack Overflow to prevent Stuck Notes
            if (stackSize >= MAX_STACK) {
                // Drop oldest note (index 0) to make room
                for(int i=0; i<MAX_STACK-1; i++) noteStack[i] = noteStack[i+1];
                stackSize = MAX_STACK - 1;
            }
            
            bool wasEmpty = (stackSize == 0);
            noteStack[stackSize++] = note;
            
            Voice& v = voices[0]; 
            v.note = note; 
            v.freq = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
            v.pan = 0.0f; 
            
            if (currentMode == 2) { 
                // Mono: Always retrigger
                if (!v.active) v.currentFreq = v.freq; // Portamento restart if first note
                v.active = true; 
                v.env.trigger(true); 
            } else { 
                // Legato: Only trigger if staccato
                if (wasEmpty) { 
                    v.currentFreq = v.freq; 
                    v.active = true; 
                    v.env.trigger(true); 
                }
                v.active = true; 
            }
            return;
        }
        
        // --- UNISON (4) ---
        if (currentMode == 4) {
            float baseFreq = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
            for(int i=0; i<NUM_VOICES; i++) {
                Voice& v = voices[i]; v.note = note; v.freq = baseFreq;
                float spread = (float)(i - 2.5f) / 2.5f;
                // Unison Spread
                v.detuneRatio = spread; v.pan = spread;
                if (!v.active) v.currentFreq = v.freq;
                v.active = true; v.env.trigger(true);
            }
            return;
        }
        
        // --- POLY (0 & 1) ---
        // 1. Try to reuse same note (retrigger)
        for(int i=0; i<NUM_VOICES; i++) {
            if (voices[i].note == note && voices[i].env.state != JunoEnvelope::IDLE) {
                voices[i].active = true; voices[i].env.trigger(true); return;
            }
        }
        
        // 2. Find voice
        int target = -1;
        if (currentMode == 0) { // Poly 1 (Round Robin)
            target = voiceRoundRobin; 
            voiceRoundRobin = (voiceRoundRobin + 1) % NUM_VOICES; 
        } else { // Poly 2 (Steal Idle)
            for(int i=0; i<NUM_VOICES; i++) if (voices[i].env.state == JunoEnvelope::IDLE) { target = i; break; }
            if (target == -1) { 
                target = voiceRoundRobin; 
                voiceRoundRobin = (voiceRoundRobin + 1) % NUM_VOICES; 
            }
        }
        
        Voice& v = voices[target];
        v.note = note;
        v.freq = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
        v.detuneRatio = getNoise(noiseSeed);
        v.pan = getNoise(noiseSeed);
        if (v.env.state == JunoEnvelope::IDLE) v.currentFreq = v.freq; 
        v.active = true; v.env.trigger(true);
        v.lastFc = -1.0f;
    }

    void noteOff(int note) {
        // --- MONO & LEGATO ---
        if (currentMode == 2 || currentMode == 3) {
            int foundIdx = -1;
            for (int i = 0; i < stackSize; i++) {
                if (noteStack[i] == note) { foundIdx = i; break; }
            }
            
            if (foundIdx != -1) {
                // Check if this was the TOP note (the one playing)
                bool wasTop = (foundIdx == stackSize - 1);
                
                // Remove from stack
                for (int j = foundIdx; j < stackSize - 1; j++) {
                    noteStack[j] = noteStack[j + 1];
                }
                stackSize--;
                
                Voice& v = voices[0];

                if (stackSize > 0) {
                    if (wasTop) {
                        // We released the playing note -> Return to previous note
                        int prevNote = noteStack[stackSize - 1];
                        v.note = prevNote; 
                        v.freq = 440.0f * powf(2.0f, (prevNote - 69.0f) / 12.0f);
                        
                        // FIX: Only retrigger if in Mono Mode (2)
                        // If in Legato Mode (3), just glide freq, DO NOT retrigger
                        if (currentMode == 2) v.env.trigger(true);
                    } 
                    // else: We released a background note. Do nothing.
                } else {
                    // Stack empty, kill voice
                    v.active = false; 
                    v.env.trigger(false);
                }
            }
            return;
        }
        
        // --- POLY & UNISON ---
        for(int i=0; i<NUM_VOICES; i++) {
            if (voices[i].active && voices[i].note == note) {
                voices[i].active = false; voices[i].env.trigger(false); 
            }
        }
    }

    void run(uint32_t n_samples) {
        if (!ports[PORT_OUT_L]) return;

        const LV2_Atom_Sequence* midiIn = (const LV2_Atom_Sequence*)ports[PORT_MIDI];
        LV2_ATOM_SEQUENCE_FOREACH(midiIn, ev) {
            if (ev->body.type == uris.midi_MidiEvent) {
                const uint8_t* msg = (const uint8_t*)(ev + 1);
                int status = msg[0] & 0xF0; int n = msg[1] & 0x7F; int v = msg[2] & 0x7F;
                if (status == 0x90 && v > 0) noteOn(n, v);
                else if (status == 0x80 || (status == 0x90 && v == 0)) noteOff(n);
            }
        }

        int newMode = (int)SAFE_REF(PORT_VOICE_MODE);
        if(newMode != currentMode) { 
            currentMode = newMode; stackSize=0; 
            for(int i=0;i<NUM_VOICES;i++) voices[i].active=false; 
        }

        int osMode = (int)SAFE_REF(PORT_OVERSAMPLE);
        int osRatio = osMode + 1;
        float osInv = 1.0f / (float)osRatio;

        float lfoRateParam = NORM10(SAFE_REF(PORT_LFO_RATE)); 
        float lfoFreq = mapCurve(lfoRateParam, 0.3f, 20.0f, 2.5f);
        float lfoDelayParam = NORM10(SAFE_REF(PORT_LFO_DELAY));
        float lfoDelayTime = lfoDelayParam * 3.0f;
        
        float envA = NORM10(SAFE_REF(PORT_ENV_A));
        float envD = NORM10(SAFE_REF(PORT_ENV_D));
        float envS = NORM10(SAFE_REF(PORT_ENV_S));
        float envR = NORM10(SAFE_REF(PORT_ENV_R));
        
        float raw_aCoeff, raw_dCoeff, raw_rCoeff;
        {
            float aTime = 0.001f + envA * envA * envA * 3.0f;
            raw_aCoeff = 1.0f - expf(-1.0f / (aTime * fs));
            float dTime = 0.002f + envD * envD * envD * 10.0f;
            raw_dCoeff = 1.0f - expf(-1.0f / (dTime * fs));
            float rTime = 0.002f + envR * envR * envR * 10.0f;
            raw_rCoeff = 1.0f - expf(-1.0f / (rTime * fs));
        }
        
        float vcfCutoffRaw = NORM10(SAFE_REF(PORT_VCF_FREQ));
        float vcfResRaw = NORM10(SAFE_REF(PORT_VCF_RES)) * 3.4f; 
        float vcfEnvMod = NORM10(SAFE_REF(PORT_VCF_ENV));
        bool vcfEnvNeg = (SAFE_REF(PORT_VCF_ENV_POL) > 0.5f);
        float vcfLfoRaw = NORM10(SAFE_REF(PORT_VCF_LFO)); float vcfLfoMod = vcfLfoRaw * vcfLfoRaw; 
        float vcfKybd = NORM10(SAFE_REF(PORT_VCF_KYBD));
        
        int hpfPos = (int)SAFE_REF(PORT_HPF);
        float hpfParam = (hpfPos == 1) ? 0.1f : (hpfPos == 2 ? 0.3f : (hpfPos == 3 ? 0.6f : 0.0f));
        JunoHighPass tempHp;
        float globalHpfAlpha = tempHp.getAlpha(hpfParam, fs);
        
        bool pulseOn = (SAFE_REF(PORT_PULSE_ON) > 0.5f);
        bool sawOn = (SAFE_REF(PORT_SAW_ON) > 0.5f);
        float subLevel = NORM10(SAFE_REF(PORT_SUB_LEVEL)) * 0.5f;
        float noiseLevel = NORM10(SAFE_REF(PORT_NOISE_LEVEL)) * 0.4f;
        bool noiseActive = (noiseLevel > 0.001f);
        
        float pwmVal = NORM10(SAFE_REF(PORT_PWM_VAL));
        int pwmMode = (int)SAFE_REF(PORT_PWM_MODE);
        
        float dcoLfoDepth = NORM10(SAFE_REF(PORT_DCO_LFO));
        int vcaMode = (int)SAFE_REF(PORT_VCA_MODE);
        
        chorus.setMode((int)SAFE_REF(PORT_CHORUS));
        float masterVol = SAFE_REF(PORT_MASTER_VOL) * 0.2f;

        float portaTime = NORM10(SAFE_REF(PORT_PORTAMENTO));
        float portaCoeff = 1.0f;
        if(portaTime > 0.01f) {
            float s = mapCurve(portaTime, 0.0f, 5.0f, 3.0f);
            portaCoeff = 1.0f - expf(-1.0f / (s * fs * 0.1f)); 
        }

        float detuneAmount = NORM10(SAFE_REF(PORT_DETUNE)) * 0.05946f;
        float spreadAmount = NORM10(SAFE_REF(PORT_SPREAD));
        
        float* outL = ports[PORT_OUT_L];
        float* outR = ports[PORT_OUT_R];

        uint32_t currentSample = 0;
        
        while (currentSample < n_samples) {
            uint32_t samplesRem = n_samples - currentSample;
            uint32_t blockSize = (samplesRem < BLOCK_SIZE) ? samplesRem : BLOCK_SIZE;
            
            lfoPhase += (lfoFreq / fs) * (float)blockSize;
            if(lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            float rawLfo = fast_sin(lfoPhase * 6.28318f); 
            
            float lfoMod = 1.0f;
            if (lfoDelayTime > 0.01f) {
                float total = lfoDelayTime * fs;
                lfoDelaySamples += (float)blockSize;
                if (lfoDelaySamples < total) lfoMod = 0.0f;
                else if (lfoDelaySamples < total * 1.5f) lfoMod = (lfoDelaySamples - total) / (total * 0.5f);
                else lfoMod = 1.0f;
            }
            float currentLfoOut = rawLfo * lfoMod;
            
            float pulseAmp = pulseOn ? 0.5f : 0.0f;
            float sawAmp = sawOn ? 1.0f : 0.0f; 
            float subAmp = subLevel;
            float globalPwmWidth = (pwmMode == 1) ? (0.5f + 0.45f * currentLfoOut * pwmVal) : (0.5f + 0.45f * pwmVal);

            for(int i=0; i<NUM_VOICES; i++) {
                Voice& v = voices[i];
                if(v.env.state == JunoEnvelope::IDLE) continue;

                v.env.setCoeffs(raw_aCoeff, raw_dCoeff, envS, raw_rCoeff);
                v.hpf.setAlpha(globalHpfAlpha);

                v.currentFreq += (v.freq - v.currentFreq) * portaCoeff;
                float pitchMod = 1.0f + (currentLfoOut * dcoLfoDepth * 0.05946f) + (v.detuneRatio * detuneAmount);
                float f = v.currentFreq * pitchMod;
                v.blockPhaseIncOS = (f / fs) / (float)osRatio;
                
                v.blockPwmWidth = globalPwmWidth;
                
                float snapEnv = v.env.currentLevel; 
                float noteOffset = (v.note - 60) / 12.0f;
                float mod = (vcfEnvNeg ? -snapEnv : snapEnv) * vcfEnvMod;
                mod += (currentLfoOut * vcfLfoMod * 0.3f) + (noteOffset * vcfKybd * 0.10034f);
                float fc = fminf(fmaxf(vcfCutoffRaw + mod, 0.0f), 1.0f);
                
                if (std::abs(fc - v.lastFc) > 0.0001f || std::abs(vcfResRaw - v.lastRes) > 0.001f) {
                    v.vcf.setOversample(osRatio);
                    v.vcf.calcCoeffs(fc, vcfResRaw);
                    v.lastFc = fc;
                    v.lastRes = vcfResRaw;
                }
                
                float p = v.pan * spreadAmount;
                if (std::abs(p - v.lastPanPos) > 0.001f) {
                    v.blockPanL = cosf((p + 1.0f) * 0.785398f); 
                    v.blockPanR = sinf((p + 1.0f) * 0.785398f);
                    v.lastPanPos = p;
                }
            }
            
            std::memset(outL + currentSample, 0, blockSize * sizeof(float));
            std::memset(outR + currentSample, 0, blockSize * sizeof(float));

            for(int i=0; i<NUM_VOICES; i++) {
                Voice& v = voices[i];
                if(v.env.state == JunoEnvelope::IDLE) continue;

                for (uint32_t s = 0; s < blockSize; ++s) {
                    float noise = noiseActive ? (getNoise(noiseSeed) * noiseLevel) : 0.0f;
                    
                    float envVal = v.env.process(); 
                    float width = (pwmMode == 2) ? (0.5f + 0.45f * envVal * pwmVal) : v.blockPwmWidth;
                    
                    float phInc = v.blockPhaseIncOS; 
                    float osAccum = 0.0f;
                    
                    for (int os = 0; os < osRatio; os++) {
                        v.phase += phInc;
                        if(v.phase >= 1.0f) v.phase -= 1.0f;
                        v.subPhase += (phInc * 0.5f); 
                        if(v.subPhase >= 1.0f) v.subPhase -= 1.0f;

                        float oscSum = 0.0f;
                        
                        oscSum += (v.phase < width) ? pulseAmp : -pulseAmp;
                        oscSum += (0.5f - v.phase) * sawAmp;
                        oscSum += (v.subPhase < 0.5f) ? subAmp : -subAmp;
                        oscSum += noise; 
                        
                        osAccum += v.vcf.process(oscSum);
                    }
                    
                    float voiceOut = osAccum * osInv;
                    v.dcoHpState += 0.002f * (voiceOut - v.dcoHpState);
                    voiceOut -= v.dcoHpState;
                    
                    voiceOut = v.hpf.process(voiceOut);
                    voiceOut *= (vcaMode == 0) ? v.gateSmoother : envVal;
                    if (vcaMode == 0) v.gateSmoother += 0.005f * ((v.active?1.0f:0.0f) - v.gateSmoother);

                    outL[currentSample + s] += voiceOut * v.blockPanL;
                    outR[currentSample + s] += voiceOut * v.blockPanR;
                }
            }

            float scl = (currentMode == 4) ? 0.15f : 0.25f;
            
            for (uint32_t s = 0; s < blockSize; ++s) {
                float mixL = outL[currentSample + s] * scl;
                float mixR = outR[currentSample + s] * scl;
                
                if (chorus.mode > 0) {
                    float chL, chR;
                    chorus.process(mixL, mixR, chL, chR); 
                    outL[currentSample + s] = SafetyClamp(chL * masterVol * 0.8f);
                    outR[currentSample + s] = SafetyClamp(chR * masterVol * 0.8f);
                } else {
                    outL[currentSample + s] = SafetyClamp(mixL * masterVol * 0.8f);
                    outR[currentSample + s] = SafetyClamp(mixR * masterVol * 0.8f);
                }
            }
            
            currentSample += blockSize;
        }
    }

    static LV2_Handle instantiate(const LV2_Descriptor* r, double t, const char* p, const LV2_Feature* const* f) { return new JunoPlugin(t, f); }
    static void connect_port_static(LV2_Handle i, uint32_t p, void* d) { ((JunoPlugin*)i)->connect_port(p, d); }
    static void run_static(LV2_Handle i, uint32_t n) { ((JunoPlugin*)i)->run(n); }
    static void cleanup(LV2_Handle i) { delete (JunoPlugin*)i; }
    static const void* extension_data(const char* u) { return NULL; }
};

static const LV2_Descriptor descriptor = { PLUGIN_URI, JunoPlugin::instantiate, JunoPlugin::connect_port_static, NULL, JunoPlugin::run_static, NULL, JunoPlugin::cleanup, JunoPlugin::extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : NULL; }