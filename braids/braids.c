#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "dsp/braids_96_dsp.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/braids"

typedef struct {
    LV2_URID_Map* map;
    LV2_URID midi_MidiEvent;

    // Ports
    const LV2_Atom_Sequence* midi_in;
    LV2_Atom_Sequence*       midi_out;
    const float* in_l;
    const float* in_r;
    float* out_l;
    float* out_r;
    
    const float* p_shape;
    const float* p_timbre;
    const float* p_color;

    Braids96DSP* dsp;
    
    // State
    int current_note;
} Plugin;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* p, const LV2_Feature* const* f) {
    Plugin* self = (Plugin*)calloc(1, sizeof(Plugin));
    
    // Hardcoded URID for S2400 Compatibility
    for (int i = 0; f[i]; ++i) {
        if (!strcmp(f[i]->URI, LV2_URID__map)) {
            self->map = (LV2_URID_Map*)f[i]->data;
        }
    }
    if (self->map) {
        self->midi_MidiEvent = self->map->map(self->map->handle, "http://lv2plug.in/ns/ext/midi#MidiEvent");
    }

    self->dsp = braids96_new();
    braids96_init(self->dsp);

    // Default note
    self->current_note = 60;
    braids96_set_pitch(self->dsp, 60.0f);

    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
    Plugin* self = (Plugin*)instance;
    switch (port) {
        case 0: self->midi_in = (const LV2_Atom_Sequence*)data_location; break;
        case 1: self->midi_out = (LV2_Atom_Sequence*)data_location; break;
        case 2: self->in_l = (const float*)data_location; break;
        case 3: self->in_r = (const float*)data_location; break;
        case 4: self->out_l = (float*)data_location; break;
        case 5: self->out_r = (float*)data_location; break;
        case 6: self->p_shape = (const float*)data_location; break;
        case 7: self->p_timbre = (const float*)data_location; break;
        case 8: self->p_color = (const float*)data_location; break;
    }
}

static void activate(LV2_Handle instance) {
    Plugin* self = (Plugin*)instance;
    braids96_init(self->dsp);
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    Plugin* self = (Plugin*)instance;
    if (!self->out_l || !self->out_r) return;

    // 1. Handle Parameters
    if (self->p_shape)  braids96_set_model(self->dsp, (int)*self->p_shape);
    if (self->p_timbre) braids96_set_timbre(self->dsp, *self->p_timbre);
    if (self->p_color)  braids96_set_color(self->dsp, *self->p_color);

    // 2. Handle MIDI In & Passthrough
    if (self->midi_in && self->midi_out) {
        const uint32_t CAPACITY = self->midi_out->atom.size;
        lv2_atom_sequence_clear(self->midi_out);
        self->midi_out->atom.type = self->midi_in->atom.type;

        LV2_ATOM_SEQUENCE_FOREACH(self->midi_in, ev) {
            if (ev->body.type == self->midi_MidiEvent) {
                // Pass MIDI thru unconditionally
                lv2_atom_sequence_append_event(self->midi_out, CAPACITY, ev);

                // Internal Parsing for Braids
                const uint8_t* const msg = (const uint8_t*)(ev + 1);
                uint8_t status = msg[0] & 0xF0;
                
                if (status == 0x90) { // Note On
                    uint8_t note = msg[1];
                    uint8_t vel = msg[2];
                    if (vel > 0) {
                        self->current_note = note;
                        braids96_set_pitch(self->dsp, (float)note); 
                        braids96_trigger(self->dsp);
                    }
                }
            }
        }
    }

    // 3. Process Audio (Oversampled)
    braids96_process(self->dsp, self->out_l, self->out_r, n_samples);
}

static void cleanup(LV2_Handle instance) {
    Plugin* self = (Plugin*)instance;
    if (self->dsp) braids96_free(self->dsp);
    free(self);
}

static const LV2_Descriptor descriptor = { PLUGIN_URI, instantiate, connect_port, activate, run, NULL, cleanup, NULL };
__attribute__((visibility("default"))) const LV2_Descriptor* lv2_descriptor(uint32_t index) { return (index == 0) ? &descriptor : NULL; }