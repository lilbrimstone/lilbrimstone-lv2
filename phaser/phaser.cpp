#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include "lv2/core/lv2.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/midi/midi.h"
#include "lv2/urid/urid.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/phaser"
#define STAGES 6
#define PI 3.14159265359f
#define TWO_PI 6.28318530718f

// Power of 2 buffer for easy masking
#define FLANGE_BUF_SIZE 2048
#define FLANGE_BUF_MASK 2047

namespace LBPhaser {

    enum PortIndex {
        PORT_MIDI_IN = 0,
        PORT_MIDI_OUT = 1,
        PORT_IN_L = 2,
        PORT_IN_R = 3,
        PORT_OUT_L = 4,
        PORT_OUT_R = 5,
        PORT_RATE = 6,
        PORT_DEPTH = 7,
        PORT_FEEDBACK = 8,
        PORT_MANUAL = 9,  
        PORT_SPREAD = 10,
        PORT_MIX = 11,
        PORT_MODE = 12
    };

    enum Mode {
        MODE_PHASER = 0,
        MODE_FLANGER = 1
    };

    struct AllPass {
        float z1; 
        AllPass() : z1(0.0f) {}
        inline float process(float x, float c) {
            float y = -c * x + z1;
            z1 = x + c * y;
            if (fabsf(z1) < 1e-20f) z1 = 0.0f; // Denormal protection
            return y;
        }
    };

    class Phaser {
    private:
        LV2_URID_Map* map;
        
        const LV2_Atom_Sequence* midiIn;
        LV2_Atom_Sequence* midiOut;
        
        const float* inL; const float* inR; float* outL; float* outR;
        const float* rate; const float* depth; const float* feedback;
        const float* manual; const float* spread; const float* mix;
        const float* mode;

        double sampleRate;
        float lfoPhase;
        
        AllPass apL[STAGES];
        AllPass apR[STAGES];

        // Flanger
        float* dBufferL;
        float* dBufferR;
        int writePos; 

        float fbStateL;
        float fbStateR;

        const float PH_MIN_FREQ = 150.0f;
        const float PH_MAX_FREQ = 3800.0f;
        const float FL_MIN_DELAY_MS = 0.5f;
        const float FL_MAX_DELAY_MS = 10.0f;
        
    public:
        Phaser(double sr, LV2_URID_Map* mapFeature) : 
            map(mapFeature), midiIn(NULL), midiOut(NULL),
            inL(NULL), inR(NULL), outL(NULL), outR(NULL),
            rate(NULL), depth(NULL), feedback(NULL), manual(NULL), spread(NULL), mix(NULL), mode(NULL),
            sampleRate(sr), 
            writePos(0), fbStateL(0.0f), fbStateR(0.0f)
        {
            if (sampleRate < 44100.0) sampleRate = 44100.0;
            
            // Randomize starting LFO phase to prevent constructive interference
            lfoPhase = (float)rand() / (float)RAND_MAX;
            
            dBufferL = (float*)calloc(FLANGE_BUF_SIZE, sizeof(float));
            dBufferR = (float*)calloc(FLANGE_BUF_SIZE, sizeof(float));
        }

        ~Phaser() {
            if (dBufferL) free(dBufferL);
            if (dBufferR) free(dBufferR);
        }

        void connect_port(uint32_t port, void* data) {
            switch ((PortIndex)port) {
                case PORT_MIDI_IN: midiIn = (const LV2_Atom_Sequence*)data; break;
                case PORT_MIDI_OUT: midiOut = (LV2_Atom_Sequence*)data; break;
                case PORT_IN_L: inL = (const float*)data; break;
                case PORT_IN_R: inR = (const float*)data; break;
                case PORT_OUT_L: outL = (float*)data; break;
                case PORT_OUT_R: outR = (float*)data; break;
                case PORT_RATE: rate = (const float*)data; break;
                case PORT_DEPTH: depth = (const float*)data; break;
                case PORT_FEEDBACK: feedback = (const float*)data; break;
                case PORT_MANUAL: manual = (const float*)data; break;
                case PORT_SPREAD: spread = (const float*)data; break;
                case PORT_MIX: mix = (const float*)data; break;
                case PORT_MODE: mode = (const float*)data; break;
            }
        }

        inline float get_coef(float f) {
            if (f > sampleRate * 0.45f) f = sampleRate * 0.45f;
            if (f < 10.0f) f = 10.0f;
            float t = tanf(PI * f / (float)sampleRate);
            return (1.0f - t) / (1.0f + t);
        }

        void run(uint32_t n_samples) {
            if (sampleRate <= 0.0f || inL == NULL || outL == NULL) return;

            // --- MIDI PASSTHROUGH ---
            if (midiOut) {
                const uint32_t capacity = midiOut->atom.size;
                lv2_atom_sequence_clear(midiOut);
                if (midiIn) {
                    midiOut->atom.type = midiIn->atom.type;
                    LV2_ATOM_SEQUENCE_FOREACH(midiIn, ev) {
                        lv2_atom_sequence_append_event(midiOut, capacity, ev);
                    }
                }
            }

            float r_hz = *rate; if (r_hz < 0.01f) r_hz = 0.01f;
            const float d = *depth;
            const float fb = *feedback;
            const float m_ctrl = *manual; 
            const float sp = *spread;
            const float mixVal = *mix;
            const int currentMode = (int)(*mode + 0.1f);

            // Equal power crossfading coefficients
            float dryGain = cosf(mixVal * PI * 0.5f);
            float wetGain = sinf(mixVal * PI * 0.5f);

            float lfoInc = r_hz / (float)sampleRate;
            
            const float phLogMin = log2f(PH_MIN_FREQ);
            const float phLogRange = log2f(PH_MAX_FREQ) - phLogMin;

            const float flMinSamps = (FL_MIN_DELAY_MS * 0.001f) * sampleRate;
            const float flRangeSamps = ((FL_MAX_DELAY_MS - FL_MIN_DELAY_MS) * 0.001f) * sampleRate;

            for (uint32_t i = 0; i < n_samples; ++i) {
                lfoPhase += lfoInc;
                if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;

                float lfoL = sinf(lfoPhase * TWO_PI);
                float phaseR = lfoPhase + sp; if (phaseR >= 1.0f) phaseR -= 1.0f;
                float lfoR = sinf(phaseR * TWO_PI);

                float inS_L = inL[i];
                float inS_R = inR[i];
                
                float wetInL = inS_L + (fbStateL * fb * 0.95f);
                float wetInR = inS_R + (fbStateR * fb * 0.95f);

                float effectL = 0.0f;
                float effectR = 0.0f;

                if (currentMode == MODE_PHASER) {
                    float modL = m_ctrl + (lfoL * d * 0.5f);
                    float modR = m_ctrl + (lfoR * d * 0.5f);
                    if (modL < 0.0f) modL = 0.0f; if (modL > 1.0f) modL = 1.0f;
                    if (modR < 0.0f) modR = 0.0f; if (modR > 1.0f) modR = 1.0f;

                    float freqL = exp2f(phLogMin + (modL * phLogRange));
                    float freqR = exp2f(phLogMin + (modR * phLogRange));
                    
                    float coefL = get_coef(freqL);
                    float coefR = get_coef(freqR);

                    float curL = wetInL;
                    float curR = wetInR;
                    for (int s = 0; s < STAGES; ++s) {
                        curL = apL[s].process(curL, coefL);
                        curR = apR[s].process(curR, coefR);
                    }
                    effectL = curL;
                    effectR = curR;

                } else { // FLANGER
                    dBufferL[writePos] = wetInL;
                    dBufferR[writePos] = wetInR;

                    float modL = m_ctrl + (lfoL * d * 0.5f);
                    float modR = m_ctrl + (lfoR * d * 0.5f);
                    
                    if (modL < 0.0f) modL = 0.0f; if (modL > 1.0f) modL = 1.0f;
                    if (modR < 0.0f) modR = 0.0f; if (modR > 1.0f) modR = 1.0f;

                    float delaySampsL = flMinSamps + (modL * flRangeSamps);
                    float delaySampsR = flMinSamps + (modR * flRangeSamps);

                    float readIdxL = (float)writePos - delaySampsL;
                    float readIdxR = (float)writePos - delaySampsR;
                    
                    while (readIdxL < 0.0f) readIdxL += FLANGE_BUF_SIZE;
                    while (readIdxR < 0.0f) readIdxR += FLANGE_BUF_SIZE;
                    
                    int idxIntL = (int)readIdxL;
                    int idxIntR = (int)readIdxR;
                    float fracL = readIdxL - idxIntL;
                    float fracR = readIdxR - idxIntR;

                    float s1L = dBufferL[idxIntL & FLANGE_BUF_MASK];
                    float s2L = dBufferL[(idxIntL + 1) & FLANGE_BUF_MASK];
                    effectL = s1L + fracL * (s2L - s1L);

                    float s1R = dBufferR[idxIntR & FLANGE_BUF_MASK];
                    float s2R = dBufferR[(idxIntR + 1) & FLANGE_BUF_MASK];
                    effectR = s1R + fracR * (s2R - s1R);

                    writePos = (writePos + 1) & FLANGE_BUF_MASK;
                }

                fbStateL = effectL;
                fbStateR = effectR;

                // Equal Power Mix
                outL[i] = inS_L * dryGain + effectL * wetGain;
                outR[i] = inS_R * dryGain + effectR * wetGain;
            }
        }
    };
} // end namespace

// --- C-Linkage Interface Layer ---
extern "C" {
    static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundle_path, const LV2_Feature* const* features) {
        LV2_URID_Map* map = NULL;
        for (int i = 0; features[i]; ++i) {
            if (!strcmp(features[i]->URI, LV2_URID__map)) {
                map = (LV2_URID_Map*)features[i]->data;
            }
        }
        void* ptr = calloc(1, sizeof(LBPhaser::Phaser));
        if (!ptr) return NULL;
        return new(ptr) LBPhaser::Phaser(rate, map);
    }

    static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
        ((LBPhaser::Phaser*)instance)->connect_port(port, data);
    }

    static void run(LV2_Handle instance, uint32_t n_samples) {
        ((LBPhaser::Phaser*)instance)->run(n_samples);
    }

    static void cleanup(LV2_Handle instance) {
        LBPhaser::Phaser* self = (LBPhaser::Phaser*)instance;
        self->~Phaser();
        free(self);
    }

    static const void* extension_data(const char* uri) { return NULL; }

    static const LV2_Descriptor descriptor = {
        PLUGIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data
    };

    #ifdef _WIN32
    __declspec(dllexport)
    #else
    __attribute__((visibility("default")))
    #endif
    const LV2_Descriptor* lv2_descriptor(uint32_t index) {
        return index == 0 ? &descriptor : NULL;
    }
}