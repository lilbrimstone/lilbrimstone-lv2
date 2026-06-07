#ifndef OPEN303_DSP_H
#define OPEN303_DSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Open303Bridge Open303Bridge;

Open303Bridge* open303_new(float sample_rate);
void open303_free(Open303Bridge* dsp);
void open303_process(Open303Bridge* dsp, float* out_l, float* out_r, uint32_t frames);

// Parameters (mapped 0.0-1.0 from LV2)
void open303_set_waveform(Open303Bridge* dsp, float value);
void open303_set_tune(Open303Bridge* dsp, float value);
void open303_set_cutoff(Open303Bridge* dsp, float value);
void open303_set_resonance(Open303Bridge* dsp, float value);
void open303_set_env_mod(Open303Bridge* dsp, float value);
void open303_set_decay(Open303Bridge* dsp, float value);
void open303_set_accent(Open303Bridge* dsp, float value);
void open303_set_slide(Open303Bridge* dsp, float value);
void open303_set_volume(Open303Bridge* dsp, float value);

// MIDI
void open303_note_on(Open303Bridge* dsp, int note, int velocity);
void open303_note_off(Open303Bridge* dsp, int note);

#ifdef __cplusplus
}
#endif
#endif
