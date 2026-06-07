#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include "dsp/plaits_dsp.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/plaits"

typedef struct {
    const LV2_Atom_Sequence* p_midi_in;
    LV2_Atom_Sequence*       p_midi_out;
    const float* p_in_l;
    const float* p_in_r;
    float* p_out_l;
    float* p_out_r;
    
    const float* p_engine;
    const float* p_harmonics;
    const float* p_timbre;
    const float* p_morph;
    const float* p_decay;
    const float* p_lpg_colour;
    const float* p_out_routing;

    PlaitsDSP* dsp;
    float current_note;
    bool gate_active; 
    
    LV2_URID_Map* map;
    LV2_URID midi_midiEvent;
} Plaits;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor,
                              double rate,
                              const char* bundle_path,
                              const LV2_Feature* const* features) {
    Plaits* self = (Plaits*)calloc(1, sizeof(Plaits));
    if (!self) return NULL;

    // Bulletproof Feature checking (Prevents Insert Crash if Host passes NULL)
    const LV2_Feature* const* f = features;
    while (f && *f) {
        if (!strcmp((*f)->URI, LV2_URID__map)) {
            self->map = (LV2_URID_Map*)(*f)->data;
            break;
        }
        f++;
    }

    if (!self->map) {
        free(self);
        return NULL;
    }

    self->midi_midiEvent = self->map->map(self->map->handle, "http://lv2plug.in/ns/ext/midi#MidiEvent");

    self->dsp = plaits_new();
    plaits_init(self->dsp, (float)rate);
    
    self->current_note = 48.0f;
    self->gate_active = false;

    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    Plaits* self = (Plaits*)instance;
    switch (port) {
        case 0: self->p_midi_in    = (const LV2_Atom_Sequence*)data; break;
        case 1: self->p_midi_out   = (LV2_Atom_Sequence*)data; break;
        case 2: self->p_in_l       = (const float*)data; break;
        case 3: self->p_in_r       = (const float*)data; break;
        case 4: self->p_out_l      = (float*)data; break;
        case 5: self->p_out_r      = (float*)data; break;
        case 6: self->p_engine     = (const float*)data; break;
        case 7: self->p_harmonics  = (const float*)data; break;
        case 8: self->p_timbre     = (const float*)data; break;
        case 9: self->p_morph      = (const float*)data; break;
        case 10: self->p_decay     = (const float*)data; break;
        case 11: self->p_lpg_colour= (const float*)data; break;
        case 12: self->p_out_routing = (const float*)data; break;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    Plaits* self = (Plaits*)instance;
    if (!self->p_out_l || !self->p_out_r) return;

    // 1. Process MIDI & Passthrough
    if (self->p_midi_in && self->p_midi_out) {
        const uint32_t CAPACITY = self->p_midi_out->atom.size;
        lv2_atom_sequence_clear(self->p_midi_out);
        self->p_midi_out->atom.type = self->p_midi_in->atom.type;

        LV2_ATOM_SEQUENCE_FOREACH(self->p_midi_in, ev) {
            if (ev->body.type == self->midi_midiEvent) {
                lv2_atom_sequence_append_event(self->p_midi_out, CAPACITY, ev);

                const uint8_t* const msg = (const uint8_t*)(ev + 1);
                uint8_t status = msg[0] & 0xF0;
                
                if (status == 0x90 && msg[2] > 0) { 
                    self->current_note = (float)msg[1];
                    self->gate_active = true; // True Held Hardware Gate!
                } 
                else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
                    if ((float)msg[1] == self->current_note) {
                        self->gate_active = false; // Release the gate
                    }
                }
            }
        }
    }

    // 2. Safely Render DSP (Must be chunked in blocks <= 24 samples to prevent overflow)
    int routing = self->p_out_routing ? (int)(*self->p_out_routing) : 1;
    float headroom_gain = 0.5f;

    uint32_t pos = 0;
    while (pos < n_samples) {
        uint32_t chunk = n_samples - pos;
        if (chunk > 24) chunk = 24;

        plaits_render(self->dsp,
                      &self->p_out_l[pos],
                      &self->p_out_r[pos],
                      (int)chunk,
                      self->current_note,
                      self->p_engine ? *self->p_engine : 0.0f,
                      self->p_harmonics ? *self->p_harmonics : 0.5f,
                      self->p_timbre ? *self->p_timbre : 0.5f,
                      self->p_morph ? *self->p_morph : 0.5f,
                      self->p_lpg_colour ? *self->p_lpg_colour : 0.5f,
                      self->p_decay ? *self->p_decay : 0.5f,
                      self->gate_active
                      );

        // 3. Routing & Headroom Protection (applied to this specific chunk)
        for (uint32_t i = 0; i < chunk; i++) {
            float out_main = self->p_out_l[pos + i] * headroom_gain;
            float out_aux  = self->p_out_r[pos + i] * headroom_gain;

            if (routing == 1) { 
                self->p_out_l[pos + i] = out_main;
                self->p_out_r[pos + i] = out_main; // Main Dual Mono
            } else if (routing == 2) { 
                self->p_out_l[pos + i] = out_aux;
                self->p_out_r[pos + i] = out_aux;  // Aux Dual Mono
            } else { 
                self->p_out_l[pos + i] = out_main;
                self->p_out_r[pos + i] = out_aux;  // Stereo Split
            }
        }
        
        pos += chunk;
    }
}

static void cleanup(LV2_Handle instance) {
    Plaits* self = (Plaits*)instance;
    if (self->dsp) plaits_free(self->dsp);
    free(self);
}

static const LV2_Descriptor descriptor = { PLUGIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL };
__attribute__((visibility("default"))) const LV2_Descriptor* lv2_descriptor(uint32_t index) { return (index == 0) ? &descriptor : NULL; }