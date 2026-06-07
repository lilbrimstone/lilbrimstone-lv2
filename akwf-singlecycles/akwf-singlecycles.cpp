#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <lv2/time/time.h>

#include "AkwfWaves.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/akwf-singlecycles"
#define MAX_VOICES 8
#define kPi 3.14159265358979323846f

// --- GLIBC SAFE MATH WRAPPERS FOR S2400 ---
static inline float safe_exp2f(float x) {
    volatile double vx = (double)x;
    volatile double vbase = 2.0;
    volatile double vmax = 20.0;
    if (vx < -20.0) return 0.0f;
    if (vx > 20.0) return (float)pow(vbase, vmax);
    return (float)pow(vbase, vx);
}
static inline float safe_sin(float x) { return (float)sin((double)x); }
static inline float safe_cos(float x) { return (float)cos((double)x); }
static inline float fast_randf() {
    static uint32_t seed = 22222;
    seed = (seed * 196314165) + 907633515;
    return (float)seed / (float)0xffffffff;
}
// -------------------------------------------------------------------

typedef enum {
    PORT_ATOM_IN = 0, PORT_ATOM_OUT, PORT_OUT_L, PORT_OUT_R,
    PORT_FOLDER_A, PORT_WAVE_A, PORT_COARSE_A, PORT_FINE_A,
    PORT_FOLDER_B, PORT_WAVE_B, PORT_COARSE_B, PORT_FINE_B,
    PORT_CROSSFADE, PORT_RING_MOD,
    PORT_VOICE_MODE, PORT_DETUNE, PORT_SPREAD
} PortIndex;

// --- STRICT MIDI PASSTHROUGH HELPER ---
static void send_midi(LV2_Atom_Sequence* seq, uint32_t capacity, LV2_URID midiEventUri, 
                      int64_t frames, uint8_t status, uint8_t d1, uint8_t d2) {
    if (!seq) return;
    struct MIDIEvent {
        LV2_Atom_Event event;
        uint8_t        msg[3];
    };
    MIDIEvent ev;
    ev.event.time.frames = frames;
    ev.event.body.type   = midiEventUri;
    ev.event.body.size   = 3;
    ev.msg[0] = status;
    ev.msg[1] = d1;
    ev.msg[2] = d2;
    lv2_atom_sequence_append_event(seq, capacity, &ev.event);
}

// --- VOICE STRUCT ---
struct Voice {
    bool    active;
    bool    releasing;
    uint8_t currentNote;
    
    float targetFreq;
    float currentFreq;
    float oscPhaseA;    
    float oscPhaseB;    
    float amp_vol;     
    
    float driftCents; 
    float pan_L;
    float pan_R;

    void init() {
        active = false;
        releasing = false;
        currentNote = 255;
        targetFreq = 440.0f;
        currentFreq = 440.0f;
        oscPhaseA = fast_randf() * 600.0f; // Boot up in random phase
        oscPhaseB = fast_randf() * 600.0f;
        amp_vol = 0.0f;
        driftCents = 0.0f;
        pan_L = 0.707f;
        pan_R = 0.707f;
    }
};

typedef struct {
    LV2_URID_Map*  map;
    LV2_Atom_Forge forge;
    LV2_URID midi_Event;

    Voice voices[MAX_VOICES];
    bool  heldNotes[128];
    uint8_t currentMonoNote;

    float hostRate;

    const LV2_Atom_Sequence* in_port;
    LV2_Atom_Sequence*       out_port;
    float* out_l;
    float* out_r;

    const float* p_folder_a; const float* p_wave_a; const float* p_coarse_a; const float* p_fine_a;
    const float* p_folder_b; const float* p_wave_b; const float* p_coarse_b; const float* p_fine_b;
    const float* p_crossfade; const float* p_ring_mod;
    const float* p_voice_mode; const float* p_detune; const float* p_spread;

} AkwfPlugin;

static void trigger_voice_attack(Voice* v, float baseFreq, float driftCents, float panPos) {
    v->driftCents = driftCents;
    v->targetFreq = baseFreq * safe_exp2f(driftCents / 1200.0f);
    v->currentFreq = v->targetFreq; // Hard Jump

    float angle = (panPos + 1.0f) * 0.25f * kPi;
    v->pan_L = safe_cos(angle);
    v->pan_R = safe_sin(angle);

    v->active = true;
    v->releasing = false;
    
    // IF starting from silence, randomize phase! 
    // This removes the unison math transient spike and simulates free-running analog oscillators.
    if (v->amp_vol < 0.01f) {
        v->oscPhaseA = fast_randf() * 600.0f; 
        v->oscPhaseB = fast_randf() * 600.0f;
    }
}

static void handle_midi(AkwfPlugin* self, const uint8_t* msg) {
    uint8_t status = msg[0] & 0xF0;
    uint8_t note   = msg[1] & 0x7F;
    uint8_t vel    = msg[2] & 0x7F;

    int mode = (int)*self->p_voice_mode;
    float detune_knob = *self->p_detune;
    float spread_knob = *self->p_spread;

    if (status == 0x90 && vel > 0) {
        self->heldNotes[note] = true;
        self->currentMonoNote = note;
        float baseFreq = 440.0f * safe_exp2f((note - 69.0f) / 12.0f);

        if (mode == 0) {
            // POLYPHONIC
            int v_idx = -1;
            for(int i = 0; i < MAX_VOICES; ++i) { if(!self->voices[i].active) { v_idx = i; break; } }
            if (v_idx == -1) {
                float min_vol = 9999.0f;
                for(int i = 0; i < MAX_VOICES; ++i) {
                    if (self->voices[i].releasing && self->voices[i].amp_vol < min_vol) {
                        min_vol = self->voices[i].amp_vol; v_idx = i;
                    }
                }
            }
            if (v_idx == -1) v_idx = 0; 
            
            Voice* v = &self->voices[v_idx];
            v->currentNote = note;
            
            float drift = (fast_randf() * 2.0f - 1.0f) * detune_knob;
            float panP = (fast_randf() * 2.0f - 1.0f) * spread_knob;
            trigger_voice_attack(v, baseFreq, drift, panP);

        } else if (mode == 1) {
            // MONOPHONIC
            for(int i = 1; i < MAX_VOICES; i++) { self->voices[i].active = false; }
            Voice* v = &self->voices[0];
            v->currentNote = note;
            
            float drift = (fast_randf() * 2.0f - 1.0f) * detune_knob;
            float panP = (fast_randf() * 2.0f - 1.0f) * spread_knob;
            trigger_voice_attack(v, baseFreq, drift, panP);

        } else if (mode == 2) {
            // UNISON
            for (int v = 0; v < MAX_VOICES; v++) {
                Voice* voice = &self->voices[v];
                voice->currentNote = note;
                
                float normalized = (float)v / (float)(MAX_VOICES - 1); 
                float drift = (normalized * 2.0f - 1.0f) * detune_knob;
                float panP  = (normalized * 2.0f - 1.0f) * spread_knob;
                trigger_voice_attack(voice, baseFreq, drift, panP);
            }
        }

    } else if (status == 0x80 || (status == 0x90 && vel == 0)) {
        self->heldNotes[note] = false;

        if (mode == 0) {
            // Poly: Release only the matching voices
            for(int i = 0; i < MAX_VOICES; ++i) {
                if(self->voices[i].active && self->voices[i].currentNote == note && !self->voices[i].releasing) {
                    self->voices[i].releasing = true;
                }
            }
        } else {
            // Mono/Unison Legato Fallback
            if (self->currentMonoNote == note) {
                int fallbackNote = -1;
                for (int i = 127; i >= 0; i--) {
                    if (self->heldNotes[i]) { fallbackNote = i; break; }
                }

                if (fallbackNote != -1) {
                    self->currentMonoNote = fallbackNote;
                    float fallbackBase = 440.0f * safe_exp2f((fallbackNote - 69.0f) / 12.0f);
                    for(int i = 0; i < MAX_VOICES; ++i) {
                        if (self->voices[i].active && !self->voices[i].releasing) {
                            self->voices[i].targetFreq = fallbackBase * safe_exp2f(self->voices[i].driftCents / 1200.0f);
                            self->voices[i].currentFreq = self->voices[i].targetFreq; 
                            self->voices[i].currentNote = fallbackNote;
                        }
                    }
                } else {
                    for(int i = 0; i < MAX_VOICES; ++i) {
                        if (self->voices[i].active && !self->voices[i].releasing) {
                            self->voices[i].releasing = true;
                        }
                    }
                }
            }
        }
    }
}

extern "C" {
static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, 
                              const char* bundle_path, const LV2_Feature* const* features) {
    AkwfPlugin* self = (AkwfPlugin*)calloc(1, sizeof(AkwfPlugin));
    if (!self) return NULL;

    self->hostRate = (float)rate;

    for (int i = 0; features && features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) { self->map = (LV2_URID_Map*)features[i]->data; }
    }
    
    if (!self->map) { free(self); return NULL; }
    lv2_atom_forge_init(&self->forge, self->map);
    self->midi_Event = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
    
    for (int i = 0; i < MAX_VOICES; i++) { self->voices[i].init(); }
    memset(self->heldNotes, 0, sizeof(self->heldNotes));
    self->currentMonoNote = 255;
    
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    AkwfPlugin* self = (AkwfPlugin*)instance;
    switch (port) {
        case PORT_ATOM_IN:    self->in_port      = (const LV2_Atom_Sequence*)data; break;
        case PORT_ATOM_OUT:   self->out_port     = (LV2_Atom_Sequence*)data;       break;
        case PORT_OUT_L:      self->out_l        = (float*)data;                   break;
        case PORT_OUT_R:      self->out_r        = (float*)data;                   break;
        case PORT_FOLDER_A:   self->p_folder_a   = (const float*)data;             break;
        case PORT_WAVE_A:     self->p_wave_a     = (const float*)data;             break;
        case PORT_COARSE_A:   self->p_coarse_a   = (const float*)data;             break;
        case PORT_FINE_A:     self->p_fine_a     = (const float*)data;             break;
        case PORT_FOLDER_B:   self->p_folder_b   = (const float*)data;             break;
        case PORT_WAVE_B:     self->p_wave_b     = (const float*)data;             break;
        case PORT_COARSE_B:   self->p_coarse_b   = (const float*)data;             break;
        case PORT_FINE_B:     self->p_fine_b     = (const float*)data;             break;
        case PORT_CROSSFADE:  self->p_crossfade  = (const float*)data;             break;
        case PORT_RING_MOD:   self->p_ring_mod   = (const float*)data;             break;
        case PORT_VOICE_MODE: self->p_voice_mode = (const float*)data;             break;
        case PORT_DETUNE:     self->p_detune     = (const float*)data;             break;
        case PORT_SPREAD:     self->p_spread     = (const float*)data;             break;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    AkwfPlugin* self = (AkwfPlugin*)instance;

    const uint32_t out_capacity = self->out_port ? self->out_port->atom.size : 0;

    if (self->out_port && self->in_port) {
        lv2_atom_sequence_clear(self->out_port);
        self->out_port->atom.type = self->in_port->atom.type;
    }

    if (self->in_port) {
        LV2_ATOM_SEQUENCE_FOREACH(self->in_port, ev) {
            if (ev->body.type == self->midi_Event) {
                const uint8_t* msg = (const uint8_t*)(ev + 1);
                handle_midi(self, msg);
                if (out_capacity > 0) { send_midi(self->out_port, out_capacity, self->midi_Event, ev->time.frames, msg[0], msg[1], msg[2]); }
            }
        }
    }
    
    // Wave Selectors 
    int fol_a = (int)*self->p_folder_a; if (fol_a < 0) fol_a = 0; if (fol_a >= NUM_FOLDERS) fol_a = NUM_FOLDERS - 1;
    int wA = (int)*self->p_wave_a; if (wA < 0) wA = 0; wA %= g_folders[fol_a].num_waves; 
    const float* waveA = g_folders[fol_a].waves[wA];

    int fol_b = (int)*self->p_folder_b; if (fol_b < 0) fol_b = 0; if (fol_b >= NUM_FOLDERS) fol_b = NUM_FOLDERS - 1;
    int wB = (int)*self->p_wave_b; if (wB < 0) wB = 0; wB %= g_folders[fol_b].num_waves; 
    const float* waveB = g_folders[fol_b].waves[wB];

    // Core Modifiers
    float tune_a = *self->p_coarse_a + (*self->p_fine_a / 100.0f);
    float mult_a = safe_exp2f(tune_a / 12.0f);

    float tune_b = *self->p_coarse_b + (*self->p_fine_b / 100.0f);
    float mult_b = safe_exp2f(tune_b / 12.0f);

    float mix_amt = *self->p_crossfade;
    float rm_amt  = *self->p_ring_mod;

    float amp_atk_inc = 1.0f / (self->hostRate * 0.002f);
    float amp_rel_inc = 1.0f / (self->hostRate * 0.005f);
    float fLen = (float)AKWF_LENGTH; 

    for (uint32_t i = 0; i < n_samples; ++i) {

        float sys_mix_L = 0.0f;
        float sys_mix_R = 0.0f;

        for (int v = 0; v < MAX_VOICES; v++) {
            Voice* voice = &self->voices[v];
            if (!voice->active) continue;

            if (!voice->releasing) { 
                voice->amp_vol += amp_atk_inc; 
                if (voice->amp_vol > 1.0f) { voice->amp_vol = 1.0f; } 
            } else { 
                voice->amp_vol -= amp_rel_inc; 
                if (voice->amp_vol <= 0.0f) { 
                    voice->amp_vol = 0.0f; voice->active = false; continue; 
                } 
            }

            voice->oscPhaseA += (voice->currentFreq * mult_a * fLen) / self->hostRate;
            while (voice->oscPhaseA >= fLen) { voice->oscPhaseA -= fLen; }

            voice->oscPhaseB += (voice->currentFreq * mult_b * fLen) / self->hostRate;
            while (voice->oscPhaseB >= fLen) { voice->oscPhaseB -= fLen; }

            // Bilinear Interpolations
            int   idx0A = (int)voice->oscPhaseA;
            int   idx1A = (idx0A + 1) % AKWF_LENGTH;
            float fracA = voice->oscPhaseA - (float)idx0A;
            float sampA = waveA[idx0A] * (1.0f - fracA) + waveA[idx1A] * fracA;

            int   idx0B = (int)voice->oscPhaseB;
            int   idx1B = (idx0B + 1) % AKWF_LENGTH;
            float fracB = voice->oscPhaseB - (float)idx0B;
            float sampB = waveB[idx0B] * (1.0f - fracB) + waveB[idx1B] * fracB;

            // Synthesis Matrix
            float mixed_out = sampA * (1.0f - mix_amt) + sampB * mix_amt;
            float ringmod_out = sampA * sampB;
            float final_osc = mixed_out * (1.0f - rm_amt) + ringmod_out * rm_amt;

            sys_mix_L += final_osc * voice->amp_vol * voice->pan_L;
            sys_mix_R += final_osc * voice->amp_vol * voice->pan_R;
        }

        self->out_l[i] = sys_mix_L * 0.25f;
        self->out_r[i] = sys_mix_R * 0.25f;
    }
}

static void cleanup(LV2_Handle instance) { free(instance); }
static const void* extension_data(const char* uri) { return NULL; }

static const LV2_Descriptor descriptor = {
    PLUGIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data
};

__attribute__((visibility("default"))) const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
} // extern "C"