#ifndef PLAITS_DSP_H
#define PLAITS_DSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PlaitsDSP PlaitsDSP;

PlaitsDSP* plaits_new(void);
void plaits_free(PlaitsDSP* dsp);
void plaits_init(PlaitsDSP* dsp, float sample_rate);

void plaits_render(PlaitsDSP* dsp,
                   float* output_l,
                   float* output_r,
                   int block_size,
                   float note,
                   float engine,
                   float harmonics,
                   float timbre,
                   float morph,
                   float lpg_colour,
                   float decay,
                   bool trigger
                   );

#ifdef __cplusplus
}
#endif

#endif
