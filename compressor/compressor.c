#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef LV2_SYMBOL_EXPORT
#define LV2_SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

#define COMPRESSOR_URI "https://github.com/lilbrimstone/compressor"
#define PI 3.14159265358979323846f

typedef struct {
    // Audio I/O
    const float* in_l;         // 0
    const float* in_r;         // 1
    float* out_l;              // 2
    float* out_r;              // 3
    
    // Controls
    const float* p_threshold;  // 4
    const float* p_ratio;      // 5
    const float* p_knee;       // 6
    const float* p_attack;     // 7
    const float* p_release;    // 8
    const float* p_makeup;     // 9
    const float* p_mix;        // 10
    const float* p_sc_lpf;     // 11
    const float* p_link;       // 12
    const float* p_trigger;    // 13 (0=Audio, 1=MIDI)
    
    // MIDI Atom Port
    const LV2_Atom_Sequence* midi_in; // 14
    
    // MIDI Channel Control
    const float* p_midi_ch;    // 15 (0=Omni, 1-16)

    // URID Mapping
    LV2_URID_Map* map;
    LV2_URID midi_MidiEvent;
    
    // DSP State
    float envelope_l;          
    float envelope_r;
    float rms_accum_l;         
    float rms_accum_r;         
    float sc_lpf_z1_l;         
    float sc_lpf_z1_r;         
    
    // Ghost Kick State
    float ghost_env;            // Current level of the ghost signal
    float ghost_decay_coeff;    // Decay for the ghost signal
    
    double sample_rate;
} Plugin;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* p, const LV2_Feature* const* f) {
    Plugin* self = (Plugin*)calloc(1, sizeof(Plugin));
    if (!self) return NULL;
    
    self->sample_rate = rate;
    self->envelope_l = 1.0f;
    self->envelope_r = 1.0f;

    // Set a fast decay for the ghost kick (approx 100ms)
    self->ghost_decay_coeff = expf(-1.0f / (0.1f * rate)); 

    // Locate URID Map feature
    for (int i = 0; f[i]; ++i) {
        if (!strcmp(f[i]->URI, LV2_URID__map)) {
            self->map = (LV2_URID_Map*)f[i]->data;
        }
    }

    if (self->map) {
        self->midi_MidiEvent = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
    } else {
        free(self); 
        return NULL;
    }

    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
    Plugin* self = (Plugin*)instance;
    switch (port) {
        case 0:  self->in_l        = (const float*)data_location; break;
        case 1:  self->in_r        = (const float*)data_location; break;
        case 2:  self->out_l       = (float*)data_location;       break;
        case 3:  self->out_r       = (float*)data_location;       break;
        case 4:  self->p_threshold = (const float*)data_location; break;
        case 5:  self->p_ratio     = (const float*)data_location; break;
        case 6:  self->p_knee      = (const float*)data_location; break;
        case 7:  self->p_attack    = (const float*)data_location; break;
        case 8:  self->p_release   = (const float*)data_location; break;
        case 9:  self->p_makeup    = (const float*)data_location; break;
        case 10: self->p_mix       = (const float*)data_location; break;
        case 11: self->p_sc_lpf    = (const float*)data_location; break;
        case 12: self->p_link      = (const float*)data_location; break;
        case 13: self->p_trigger   = (const float*)data_location; break;
        case 14: self->midi_in     = (const LV2_Atom_Sequence*)data_location; break;
        case 15: self->p_midi_ch   = (const float*)data_location; break;
        default: break;
    }
}

static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

static inline float pow_to_db(float pow_val) {
    return 10.0f * log10f(pow_val + 1e-12f);
}

static inline float soft_knee_gain(float level_db, float threshold_db, float ratio, float knee_db) {
    float over_db = level_db - threshold_db;
    if (knee_db <= 0.0f) {
        if (over_db <= 0.0f) return 0.0f; 
        return over_db * (1.0f - 1.0f / ratio);
    }
    float knee_half = knee_db * 0.5f;
    if (over_db < -knee_half) return 0.0f;
    if (over_db > knee_half)  return over_db * (1.0f - 1.0f / ratio);
    
    float x = over_db + knee_half;
    float slope = 1.0f - 1.0f / ratio;
    return slope * x * x / (2.0f * knee_db);
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    Plugin* self = (Plugin*)instance;
    if (!self->out_l || !self->out_r) return;
    
    // --- 1. Load Params ---
    float threshold_db = self->p_threshold ? *self->p_threshold : -12.0f;
    if (threshold_db > 0.0f) threshold_db = 0.0f;
    if (threshold_db < -60.0f) threshold_db = -60.0f;
    
    float ratio = self->p_ratio ? *self->p_ratio : 4.0f;
    float knee_db = self->p_knee ? *self->p_knee : 3.0f;
    
    float attack_ms = self->p_attack ? *self->p_attack : 5.0f;
    if (attack_ms < 0.1f) attack_ms = 0.1f;
    float release_ms = self->p_release ? *self->p_release : 100.0f;
    if (release_ms < 10.0f) release_ms = 10.0f;

    float makeup_gain = db_to_lin(self->p_makeup ? *self->p_makeup : 0.0f);
    float mix = self->p_mix ? *self->p_mix : 1.0f;
    float sc_lpf_freq = self->p_sc_lpf ? *self->p_sc_lpf : 500.0f;
    float link_amount = 1.0f; 
    if (self->p_link) link_amount = *self->p_link / 100.0f;

    // Trigger Mode: 0 = Audio, 1 = MIDI (Ghost)
    int trigger_mode = (self->p_trigger && *self->p_trigger > 0.5f) ? 1 : 0;
    
    // MIDI Channel: 0 = Omni, 1-16 = Specific Channel
    int target_ch = (self->p_midi_ch) ? (int)*self->p_midi_ch : 0;

    // --- 2. Process MIDI events ---
    if (self->midi_in) {
        LV2_ATOM_SEQUENCE_FOREACH(self->midi_in, ev) {
            if (ev->body.type == self->midi_MidiEvent) {
                const uint8_t* msg = (const uint8_t*)(ev + 1);
                
                // Parse Status Byte
                // 0x90-0x9F is Note On (Channel 0-15)
                uint8_t status = msg[0] & 0xF0;
                uint8_t channel = msg[0] & 0x0F;
                
                if (status == 0x90) {
                    // Check Channel Filter
                    // If target_ch == 0, we listen to everything (Omni)
                    // If target_ch > 0, we look for channel == target_ch - 1
                    bool ch_match = (target_ch == 0) || (channel == (target_ch - 1));
                    
                    if (ch_match) {
                        uint8_t velocity = msg[2];
                        if (velocity > 0) {
                            // Trigger Ghost Kick
                            float ghost_amp = (float)velocity / 127.0f;
                            self->ghost_env = ghost_amp * 2.0f; 
                        }
                    }
                }
            }
        }
    }

    // --- 3. Coeffs ---
    float attack_coeff = expf(-1.0f / (attack_ms * 0.001f * (float)self->sample_rate));
    float release_coeff = expf(-1.0f / (release_ms * 0.001f * (float)self->sample_rate));
    float sc_lpf_coeff = expf(-2.0f * PI * sc_lpf_freq / (float)self->sample_rate);
    float rms_coeff = expf(-1.0f / (0.002f * (float)self->sample_rate));

    // --- 4. Sample Loop ---
    for (uint32_t n = 0; n < n_samples; ++n) {
        float xL = self->in_l ? self->in_l[n] : 0.0f;
        float xR = self->in_r ? self->in_r[n] : 0.0f;

        float det_L, det_R;

        if (trigger_mode == 1) {
            // -- MIDI MODE --
            self->ghost_env *= self->ghost_decay_coeff;
            if (self->ghost_env < 1.0e-6f) self->ghost_env = 0.0f;
            det_L = self->ghost_env;
            det_R = self->ghost_env;
        } else {
            // -- AUDIO MODE --
            float absL = fabsf(xL);
            float absR = fabsf(xR);
            self->sc_lpf_z1_l = sc_lpf_coeff * self->sc_lpf_z1_l + (1.0f - sc_lpf_coeff) * absL;
            self->sc_lpf_z1_r = sc_lpf_coeff * self->sc_lpf_z1_r + (1.0f - sc_lpf_coeff) * absR;
            det_L = self->sc_lpf_z1_l;
            det_R = self->sc_lpf_z1_r;
        }

        // RMS / Power Calculation
        float sq_L = det_L * det_L;
        float sq_R = det_R * det_R;
        
        self->rms_accum_l = rms_coeff * self->rms_accum_l + (1.0f - rms_coeff) * sq_L;
        self->rms_accum_r = rms_coeff * self->rms_accum_r + (1.0f - rms_coeff) * sq_R;

        // Gain Calc
        float gr_L = soft_knee_gain(pow_to_db(self->rms_accum_l), threshold_db, ratio, knee_db);
        float gr_R = soft_knee_gain(pow_to_db(self->rms_accum_r), threshold_db, ratio, knee_db);

        // Stereo Link
        float max_gr = (gr_L > gr_R) ? gr_L : gr_R;
        gr_L = (1.0f - link_amount) * gr_L + link_amount * max_gr;
        gr_R = (1.0f - link_amount) * gr_R + link_amount * max_gr;

        float target_l = db_to_lin(-gr_L);
        float target_r = db_to_lin(-gr_R);

        // Ballistics
        if (target_l < self->envelope_l)
            self->envelope_l = attack_coeff * self->envelope_l + (1.0f - attack_coeff) * target_l;
        else
            self->envelope_l = release_coeff * self->envelope_l + (1.0f - release_coeff) * target_l;
            
        if (target_r < self->envelope_r)
            self->envelope_r = attack_coeff * self->envelope_r + (1.0f - attack_coeff) * target_r;
        else
            self->envelope_r = release_coeff * self->envelope_r + (1.0f - release_coeff) * target_r;

        // Application
        float comp_l = xL * self->envelope_l * makeup_gain;
        float comp_r = xR * self->envelope_r * makeup_gain;
        
        self->out_l[n] = xL * (1.0f - mix) + comp_l * mix;
        self->out_r[n] = xR * (1.0f - mix) + comp_r * mix;
    }
}

static const void* extension_data(const char* uri) {
    return NULL; 
}

static void cleanup(LV2_Handle instance) {
    free(instance);
}

static const LV2_Descriptor descriptor = {
    COMPRESSOR_URI,
    instantiate,
    connect_port,
    NULL,
    run,
    NULL,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return (index == 0) ? &descriptor : NULL;
}