#ifndef BRAIDS_96_DSP_H
#define BRAIDS_96_DSP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Braids96DSP Braids96DSP;

Braids96DSP* braids96_new(void);
void braids96_free(Braids96DSP* dsp);
void braids96_init(Braids96DSP* dsp);
void braids96_process(Braids96DSP* dsp, float* out_l, float* out_r, size_t block_size);

// Parameters
void braids96_set_pitch(Braids96DSP* dsp, float pitch_semitones);
void braids96_set_timbre(Braids96DSP* dsp, float timbre_01);
void braids96_set_color(Braids96DSP* dsp, float color_01);
void braids96_set_model(Braids96DSP* dsp, int model_index);
void braids96_trigger(Braids96DSP* dsp);

#ifdef __cplusplus
}
#endif
#endif
