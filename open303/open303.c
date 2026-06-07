#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "lv2/core/lv2.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/midi/midi.h"
#include "lv2/urid/urid.h"
#include "dsp/open303_dsp.h"

typedef struct {
    LV2_URID_Map* map;
    LV2_URID midi_event_uri;
    
    // I/O Ports
    const LV2_Atom_Sequence* midi_in;
    LV2_Atom_Sequence* midi_out;
    float* out_l;
    float* out_r;

    // Control Ports
    const float* p_waveform;
    const float* p_tune;
    const float* p_cutoff;
    const float* p_resonance;
    const float* p_env_mod;
    const float* p_decay;
    const float* p_accent;
    const float* p_slide;
    const float* p_volume;

    Open303Bridge* dsp;
} Plugin;

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    Plugin* self = (Plugin*)instance;
    switch (port) {
        // I/O
        case 0: self->midi_in = (const LV2_Atom_Sequence*)data; break;
        case 1: self->midi_out = (LV2_Atom_Sequence*)data; break;
        case 2: self->out_l = (float*)data; break;
        case 3: self->out_r = (float*)data; break;
        
        // Controls
        case 4: self->p_waveform = (const float*)data; break;
        case 5: self->p_tune = (const float*)data; break;
        case 6: self->p_cutoff = (const float*)data; break;
        case 7: self->p_resonance = (const float*)data; break;
        case 8: self->p_env_mod = (const float*)data; break;
        case 9: self->p_decay = (const float*)data; break;
        case 10: self->p_accent = (const float*)data; break;
        case 11: self->p_slide = (const float*)data; break;
        case 12: self->p_volume = (const float*)data; break;
    }
}

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* path, const LV2_Feature* const* features) {
    Plugin* self = (Plugin*)calloc(1, sizeof(Plugin));
    
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            self->map = (LV2_URID_Map*)features[i]->data;
        }
    }
    if (self->map) {
        self->midi_event_uri = self->map->map(self->map->handle, LV2_MIDI__MidiEvent);
    }
    
    self->dsp = open303_new((float)rate);
    return (LV2_Handle)self;
}

static void activate(LV2_Handle instance) {}

static void run(LV2_Handle instance, uint32_t n_samples) {
    Plugin* self = (Plugin*)instance;
    if (!self->dsp) return;

    // 1. Update DSP Parameters
    open303_set_waveform(self->dsp, *self->p_waveform);
    open303_set_tune(self->dsp, *self->p_tune);
    open303_set_cutoff(self->dsp, *self->p_cutoff);
    open303_set_resonance(self->dsp, *self->p_resonance);
    open303_set_env_mod(self->dsp, *self->p_env_mod);
    open303_set_decay(self->dsp, *self->p_decay);
    open303_set_accent(self->dsp, *self->p_accent);
    open303_set_slide(self->dsp, *self->p_slide);
    open303_set_volume(self->dsp, *self->p_volume);

    // 2. Clear MIDI Out buffer & copy input type
    if (self->midi_out) {
        const uint32_t capacity = self->midi_out->atom.size;
        lv2_atom_sequence_clear(self->midi_out);
        if (self->midi_in) {
            self->midi_out->atom.type = self->midi_in->atom.type;
        }
        
        // 3. Process incoming MIDI and pass it through
        if (self->midi_in) {
            LV2_ATOM_SEQUENCE_FOREACH(self->midi_in, ev) {
                
                // Copy event strictly to output buffer (Passthrough)
                lv2_atom_sequence_append_event(self->midi_out, capacity, ev);

                // Handle event for internal DSP
                if (ev->body.type == self->midi_event_uri) {
                    const uint8_t* msg = (const uint8_t*)(ev + 1);
                    uint8_t status = msg[0] & 0xF0;
                    
                    // Note ON
                    if (status == 0x90 && msg[2] > 0) {
                        open303_note_on(self->dsp, msg[1], msg[2]);
                    } 
                    // Note OFF (or Note ON with vel 0)
                    else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) {
                        open303_note_off(self->dsp, msg[1]);
                    }
                }
            }
        }
    }

    // 4. Generate Audio
    open303_process(self->dsp, self->out_l, self->out_r, n_samples);
}

static void cleanup(LV2_Handle instance) {
    Plugin* self = (Plugin*)instance;
    open303_free(self->dsp);
    free(self);
}

static const LV2_Descriptor descriptor = {
    "https://github.com/lilbrimstone/open303", instantiate, connect_port, activate, run, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}