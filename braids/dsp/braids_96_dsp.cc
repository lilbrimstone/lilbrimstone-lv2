#include "braids_96_dsp.h"
#include "braids/macro_oscillator.h"
#include <stdlib.h>
#include <vector>
#include <algorithm>

using namespace braids;

// Braids native processing block size (prevents stack overflows in internal buffers)
const size_t LOOP_BLOCK_SIZE = 24;

struct Braids96DSP {
    MacroOscillator osc;
    
    // Detailed buffers
    std::vector<int16_t> temp_buffer; // Holds the raw 96kHz output
    std::vector<uint8_t> sync_buffer; // Dummy sync buffer
    
    int current_model;
    int32_t pitch_val;
    int16_t timbre_val;
    int16_t color_val;
};

extern "C" {

Braids96DSP* braids96_new() {
    Braids96DSP* dsp = new Braids96DSP();
    // Reserve enough for S2400 max block (256) * 2 (oversample)
    dsp->temp_buffer.resize(512); 
    dsp->sync_buffer.resize(512);
    dsp->current_model = 0;
    
    // Initialize defaults
    dsp->pitch_val = 60 << 7;
    dsp->timbre_val = 0;
    dsp->color_val = 0;
    
    return dsp;
}

void braids96_free(Braids96DSP* dsp) {
    delete dsp;
}

void braids96_init(Braids96DSP* dsp) {
    dsp->osc.Init();
    dsp->osc.set_shape(MACRO_OSC_SHAPE_CSAW);
    dsp->osc.set_pitch(48 << 7);
    dsp->osc.set_parameters(0, 0);
}

void braids96_set_model(Braids96DSP* dsp, int model_index) {
    if (dsp->current_model != model_index) {
        if (model_index < 0) model_index = 0;
        if (model_index >= MACRO_OSC_SHAPE_LAST) model_index = MACRO_OSC_SHAPE_LAST - 1;
        
        // This Init() call is critical when changing models
        dsp->osc.set_shape((MacroOscillatorShape)model_index);
        dsp->current_model = model_index;
    }
}

void braids96_set_pitch(Braids96DSP* dsp, float pitch_semitones) {
    // Braids native pitch is (N * 128)
    int32_t pitch = (int32_t)(pitch_semitones * 128.0f);
    dsp->pitch_val = pitch;
}

void braids96_set_timbre(Braids96DSP* dsp, float timbre_01) {
    // Original Safe Range: 0 to 32767
    dsp->timbre_val = (int16_t)(timbre_01 * 32767.0f);
}

void braids96_set_color(Braids96DSP* dsp, float color_01) {
    // Original Safe Range: 0 to 32767
    dsp->color_val = (int16_t)(color_01 * 32767.0f);
}

void braids96_trigger(Braids96DSP* dsp) {
    dsp->osc.Strike();
}

void braids96_process(Braids96DSP* dsp, float* out_l, float* out_r, size_t block_size) {
    // 1. Calculate required 96k samples
    size_t total_samples_96k = block_size * 2;
    
    // 2. Resize buffers if host changes block size wildly (unlikely but safe)
    if (dsp->temp_buffer.size() < total_samples_96k) {
        dsp->temp_buffer.resize(total_samples_96k);
        dsp->sync_buffer.resize(total_samples_96k);
    }

    // 3. Clear Sync Buffer
    std::fill(dsp->sync_buffer.begin(), dsp->sync_buffer.end(), 0);

    // 4. Update Parameters
    dsp->osc.set_pitch(dsp->pitch_val);
    dsp->osc.set_parameters(dsp->timbre_val, dsp->color_val);

    // 5. RENDER LOOP
    // We process in small chunks (e.g., 24 samples) to avoid stack overflows
    int16_t* pOutput = dsp->temp_buffer.data();
    uint8_t* pSync   = dsp->sync_buffer.data();
    size_t remaining = total_samples_96k;

    while (remaining > 0) {
        size_t chunk = (remaining > LOOP_BLOCK_SIZE) ? LOOP_BLOCK_SIZE : remaining;
        
        dsp->osc.Render(pSync, pOutput, chunk);
        
        pOutput += chunk;
        pSync += chunk;
        remaining -= chunk;
    }

    // 6. DOWNSAMPLE (96k -> 48k)
    // Simple averaging boxcar filter
    int16_t* pRaw = dsp->temp_buffer.data();
    
    for (size_t i = 0; i < block_size; ++i) {
        int32_t s1 = *pRaw++;
        int32_t s2 = *pRaw++;
        
        // Average and convert to float
        int32_t avg = (s1 + s2) >> 1;
        float fOut = (float)avg / 32768.0f;
        
        out_l[i] = fOut;
        out_r[i] = fOut;
    }
}

} // extern "C"