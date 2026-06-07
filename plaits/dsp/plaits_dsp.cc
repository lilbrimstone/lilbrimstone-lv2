#define TEST 

#include "plaits_dsp.h"
#include "plaits/dsp/voice.h"
#include <cstring>
#include <algorithm>

#define PLAITS_ENGINE_BUFFER_SIZE 32768 

struct PlaitsDSP {
    plaits::Voice voice;
    plaits::Modulations modulations;
    plaits::Patch patch;
    uint8_t engine_buffer[PLAITS_ENGINE_BUFFER_SIZE];
    stmlib::BufferAllocator allocator;
    plaits::Voice::Frame frames[plaits::kMaxBlockSize];
};

extern "C" {

PlaitsDSP* plaits_new(void) {
    return new PlaitsDSP();
}

void plaits_free(PlaitsDSP* dsp) {
    delete dsp;
}

void plaits_init(PlaitsDSP* dsp, float sample_rate) {
    dsp->allocator.Init(dsp->engine_buffer, PLAITS_ENGINE_BUFFER_SIZE);
    dsp->voice.Init(&dsp->allocator);
    std::memset(&dsp->patch, 0, sizeof(dsp->patch));
    std::memset(&dsp->modulations, 0, sizeof(dsp->modulations));
    dsp->patch.note = 48.0f;
    dsp->modulations.trigger_patched = true;
    dsp->modulations.frequency_patched = false;
    dsp->modulations.timbre_patched = false;
    dsp->modulations.morph_patched = false; 
}

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
                   bool trigger) {
    
    // Cast float (0.0, 1.0, 2.0...) to int safely
    int engine_idx = (int)engine;
    if (engine_idx < 0) engine_idx = 0;
    if (engine_idx > 23) engine_idx = 23;

    dsp->patch.engine = (float)engine_idx;
    
    dsp->patch.note = note; 
    dsp->patch.harmonics = harmonics;
    dsp->patch.timbre = timbre;
    dsp->patch.morph = morph;
    dsp->patch.decay = decay;
    dsp->patch.lpg_colour = lpg_colour;
    
    dsp->modulations.trigger = trigger ? 1.0f : 0.0f;

    int current_sample = 0;
    while (current_sample < block_size) {
        int remaining = block_size - current_sample;
        int size = std::min(remaining, (int)plaits::kBlockSize); 
        
        dsp->voice.Render(dsp->patch, dsp->modulations, dsp->frames, size);
        
        for (int i = 0; i < size; ++i) {
            output_l[current_sample + i] = (float)dsp->frames[i].out / 32768.0f;
            output_r[current_sample + i] = (float)dsp->frames[i].aux / 32768.0f;
        }
        current_sample += size;
    }
}

}
