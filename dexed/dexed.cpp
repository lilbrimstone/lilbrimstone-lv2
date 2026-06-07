#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <new> 

// =========================================================================
// PLATFORM-SPECIFIC DEPLOYMENT
#ifndef _WIN32
    void* operator new(size_t size) { return calloc(1, size); }
    void* operator new[](size_t size) { return calloc(1, size); }
    void operator delete(void* ptr) noexcept { free(ptr); }
    void operator delete[](void* ptr) noexcept { free(ptr); }
    void operator delete(void* ptr, size_t) noexcept { free(ptr); }
    void operator delete[](void* ptr, size_t) noexcept { free(ptr); }

    extern "C" {
        void __cxa_pure_virtual() { while (1); }
        int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
        int __cxa_guard_acquire(long *g) { return !*(char *)(g); }
        void __cxa_guard_release(long *g) { *(char *)g = 1; }
        void __cxa_guard_abort(long *g) {}

        // --- GLIBC 2.17 MATH INTERCEPTORS ---
        double __wrap_exp(double x) { return expf((float)x); }
        double __wrap_exp2(double x) { return exp2f((float)x); }
        double __wrap_log(double x) { return logf((float)x); }
        double __wrap_pow(double x, double y) { return powf((float)x, (float)y); }
    }
    #define LV2_SYMBOL_EXPORT __attribute__((visibility("default")))
#else
    #define LV2_SYMBOL_EXPORT __declspec(dllexport)
#endif
// =========================================================================

#include "msfa/dx7note.h"
#include "msfa/synth.h"
#include "msfa/freqlut.h"
#include "msfa/exp2.h"
#include "msfa/sin.h"
#include "msfa/controllers.h"
#include "msfa/fm_core.h"
#include "msfa/lfo.h"
#include "msfa/env.h"
#include "msfa/pitchenv.h"
#include "msfa/porta.h"
#include "msfa/tuning.h"

// Mount the Python-generated patch bank
#include "patches/patches.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/dexed"

class StandardTuning : public TuningState {
public:
    bool is_standard_tuning() override { return true; }
    int32_t midinote_to_logfreq(int midinote) override {
        const int32_t base = 50857777; 
        const int32_t step = (1 << 24) / 12;
        return base + (step * midinote);
    }
};

// PORT MAP 
enum PortIndex {
    PORT_OUT_L = 0,
    PORT_OUT_R = 1,
    PORT_MIDI_IN = 2,
    PORT_GAIN = 3,
    PORT_OCTAVE = 4,   // Changed from Transpose to Octave
    PORT_PRESET = 5,
    PORT_ALGO = 6
};

class DexedPlugin {
public:
    const LV2_Atom_Sequence* midiIn;
    float* audioOutL;
    float* audioOutR;
    const float* gain;
    const float* octave;
    const float* preset;
    const float* algo;

    LV2_URID midi_MidiEvent;
    
    StandardTuning* tuningState;
    FmCore fmCore;
    Lfo lfo;
    Controllers controllers;
    
    // POLYPHONY! 16 voices
    static const int POLYPHONY = 16;
    Dx7Note* voices[POLYPHONY];
    int voiceNote[POLYPHONY]; // Tracks the physical key pressed
    int nextVoice;
    
    uint8_t currentPatch[156];
    int32_t audioBuf[64]; 
    int bufPos;

    int last_preset;
    int last_algo;

    DexedPlugin(double rate, LV2_URID_Map* map) : bufPos(0), nextVoice(0) {
        midi_MidiEvent = map->map(map->handle, "http://lv2plug.in/ns/ext/midi#MidiEvent");
        
        Exp2::init(); Tanh::init(); Sin::init(); 
        Freqlut::init(rate); Env::init_sr(rate); 
        Porta::init_sr(rate); Lfo::init(rate); PitchEnv::init(rate);

        tuningState = (StandardTuning*)calloc(1, sizeof(StandardTuning));
        tuningState = new(tuningState) StandardTuning();
        
        controllers.core = &fmCore;
        controllers.values_[kControllerPitch] = 0x2000;
        controllers.masterTune = 0;
        controllers.mpeEnabled = false;
        memcpy(controllers.opSwitch, "111111", 6);

        last_preset = -1;
        last_algo = -1;

        // Init 16 separate DX7 components
        for(int i = 0; i < POLYPHONY; i++) {
            voices[i] = (Dx7Note*)calloc(1, sizeof(Dx7Note));
            voices[i] = new(voices[i]) Dx7Note(tuningState, NULL);
            voiceNote[i] = -1;
        }
    }

    ~DexedPlugin() {
        for(int i=0; i<POLYPHONY; i++) free(voices[i]);
        free(tuningState);
    }

    void run(uint32_t nFrames) {
        if (!audioOutL || !audioOutR) return;

        // --- 1. Parameter Polling ---
        int cur_p = preset ? (int)*preset : 0;
        int cur_a = algo ? (int)*algo : 0;

        // Load new patch if parameters change
        if (cur_p != last_preset || cur_a != last_algo) {
            last_preset = cur_p;
            last_algo = cur_a;
            if (cur_p < 0) cur_p = 0;
            if (cur_p >= NUM_PATCHES) cur_p = NUM_PATCHES - 1;
            
            memcpy(currentPatch, FACTORY_BANK[cur_p], 156);
            if (cur_a >= 1 && cur_a <= 32) currentPatch[134] = cur_a - 1; 
            
            lfo.reset(currentPatch + 137);
        }

        // --- 2. MIDI Allocator ---
        if (midiIn) {
            int oct_offset = octave ? (int)round(*octave) * 12 : 0;

            LV2_ATOM_SEQUENCE_FOREACH(midiIn, ev) {
                if (ev->body.type == midi_MidiEvent) {
                    const uint8_t* msg = (const uint8_t*)(ev + 1);
                    uint8_t status = msg[0] & 0xF0;
                    
                    if (status == 0x90 && msg[2] > 0) { // NOTE ON
                        int physical_note = msg[1];
                        int playing_pitch = physical_note + oct_offset;
                        
                        // Enforce MIDI bounds on the synthesized pitch
                        if (playing_pitch < 0) playing_pitch = 0;
                        if (playing_pitch > 127) playing_pitch = 127;

                        // Find voice (1. check for identical physical note retrig, 2. find empty, 3. oldest)
                        int v = -1;
                        for(int i=0; i<POLYPHONY; ++i) if (voiceNote[i] == physical_note) { v = i; break; }
                        if (v == -1) {
                            for(int i=0; i<POLYPHONY; ++i) if (!voices[i]->isPlaying()) { v = i; break; }
                        }
                        if (v == -1) {
                            v = nextVoice;
                            nextVoice = (nextVoice + 1) % POLYPHONY;
                        }

                        // Send the transposed pitch to the synth, but store the physical key
                        voices[v]->init(currentPatch, playing_pitch, msg[2], 1, &controllers);
                        voiceNote[v] = physical_note;

                    } else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) { // NOTE OFF
                        int physical_note = msg[1];
                        
                        // Search by physical key to prevent stuck notes on octave shift
                        for(int i=0; i<POLYPHONY; ++i) {
                            if (voiceNote[i] == physical_note && voices[i]->isPlaying()) {
                                voices[i]->keyup();
                                voiceNote[i] = -1;
                            }
                        }
                    } else if (status == 0xE0) { // PITCH BEND
                        int bend = (msg[1] & 0x7F) | ((msg[2] & 0x7F) << 7);
                        controllers.values_[kControllerPitch] = bend;
                    }
                }
            }
        }

        // --- 3. Audio Loop ---
        float g = gain ? *gain : 1.0f;

        for (uint32_t i = 0; i < nFrames; i++) {
            if (bufPos == 0) {
                // Clear the Master Mix bus
                memset(audioBuf, 0, sizeof(audioBuf));
                
                int32_t lfo_val = lfo.getsample();
                int32_t lfo_delay = lfo.getdelay();
                
                // Additive Mixing! Every active DX7 voice compiles into the Master buffer
                for (int v = 0; v < POLYPHONY; v++) {
                    if (voices[v]->isPlaying()) {
                        voices[v]->compute(audioBuf, lfo_val, lfo_delay, &controllers);
                    }
                }
            }
            
            // Scaled by 0.0625f (1/16) to completely prevent headroom clipping at max polyphony
            float x = (float)audioBuf[bufPos] * 0.000000059604645f * g * 0.0625f;
            
            if (x > 1.0f) x = 1.0f;
            else if (x < -1.0f) x = -1.0f;
            
            audioOutL[i] = audioOutR[i] = x; 
            bufPos = (bufPos + 1) & 63;
        }
    }
};

extern "C" {
static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* path, const LV2_Feature* const* features) {
    LV2_URID_Map* map = NULL;
    const LV2_Feature* const* f = features;
    while (f && *f) {
        if (!strcmp((*f)->URI, LV2_URID__map)) { map = (LV2_URID_Map*)(*f)->data; break; }
        f++;
    }
    if (!map) return NULL;
    
    DexedPlugin* self = (DexedPlugin*)calloc(1, sizeof(DexedPlugin));
    if (!self) return NULL;
    
    return (LV2_Handle)new(self) DexedPlugin(rate, map);
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    DexedPlugin* p = (DexedPlugin*)instance;
    switch (port) {
        case PORT_OUT_L:      p->audioOutL  = (float*)data; break;
        case PORT_OUT_R:      p->audioOutR  = (float*)data; break;
        case PORT_MIDI_IN:    p->midiIn     = (const LV2_Atom_Sequence*)data; break;
        case PORT_GAIN:       p->gain       = (const float*)data; break;
        case PORT_OCTAVE:     p->octave     = (const float*)data; break;
        case PORT_PRESET:     p->preset     = (const float*)data; break;
        case PORT_ALGO:       p->algo       = (const float*)data; break;
    }
}

static void run(LV2_Handle instance, uint32_t nFrames) { ((DexedPlugin*)instance)->run(nFrames); }

static void cleanup(LV2_Handle instance) { 
    if (instance) {
        ((DexedPlugin*)instance)->~DexedPlugin();
        free(instance); 
    }
}

static const LV2_Descriptor descriptor = { PLUGIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL };

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { 
    return index == 0 ? &descriptor : NULL; 
}
} // extern "C"