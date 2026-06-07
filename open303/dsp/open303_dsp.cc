#include "open303_dsp.h"
#include "rosic/rosic_Open303.h"
#include <math.h>

using namespace rosic;

struct Open303Bridge {
    Open303* engine;
    float sample_rate;
};

Open303Bridge* open303_new(float sample_rate) {
    Open303Bridge* dsp = (Open303Bridge*)calloc(1, sizeof(Open303Bridge));
    dsp->sample_rate = sample_rate;
    
    dsp->engine = new Open303();
    dsp->engine->setSampleRate(sample_rate);
    
    dsp->engine->setWaveform(0.0); // Saw
    dsp->engine->setCutoff(1000.0);
    dsp->engine->setResonance(0.0);
    dsp->engine->setTuning(220.0); // Fixed base tuning
    
    return dsp;
}

void open303_free(Open303Bridge* dsp) {
    if (dsp) {
        if (dsp->engine) delete dsp->engine;
        free(dsp);
    }
}

void open303_process(Open303Bridge* dsp, float* out_l, float* out_r, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        float val = (float)dsp->engine->getSample();
        out_l[i] = val;
        out_r[i] = val;
    }
}

void open303_set_waveform(Open303Bridge* dsp, float value) {
    // 0.0 = Saw, 1.0 = Square
    dsp->engine->setWaveform((double)value);
}

void open303_set_tune(Open303Bridge* dsp, float value) {
    // TARGET: +/- 1 Semitone
    // 0.0 -> -1.0 semitone
    // 0.5 ->  0.0 semitone
    // 1.0 -> +1.0 semitone
    
    double semitones = (value - 0.5) * 2.0;
    
    // We use setPitchBend because it updates the frequency immediately 
    // in the audio loop (via pitchWheelFactor).
    // setTuning only affects the calculation at the start of a note.
    dsp->engine->setPitchBend(semitones);
}

void open303_set_cutoff(Open303Bridge* dsp, float value) {
    // Map 0..1 to 300Hz..3000Hz approx
    double min_f = 200.0;
    double max_f = 2800.0;
    dsp->engine->setCutoff(min_f + (max_f - min_f) * value * value); 
}

void open303_set_resonance(Open303Bridge* dsp, float value) {
    // 0..100
    dsp->engine->setResonance(value * 100.0);
}

void open303_set_env_mod(Open303Bridge* dsp, float value) {
    // 0..100
    dsp->engine->setEnvMod(value * 100.0);
}

void open303_set_decay(Open303Bridge* dsp, float value) {
    // 200ms .. 2000ms
    dsp->engine->setDecay(200.0 + (value * 1800.0));
}

void open303_set_accent(Open303Bridge* dsp, float value) {
    // 0..100
    dsp->engine->setAccent(value * 100.0);
}

void open303_set_slide(Open303Bridge* dsp, float value) {
    // TB-303 slide is usually fixed at ~60ms, but we allow control here
    dsp->engine->setSlideTime(value * 400.0);
}

void open303_set_volume(Open303Bridge* dsp, float value) {
    if (value < 0.01) dsp->engine->setVolume(-144.0);
    else dsp->engine->setVolume(20.0 * log10(value));
}

void open303_note_on(Open303Bridge* dsp, int note, int velocity) {
    dsp->engine->noteOn(note, velocity);
}

void open303_note_off(Open303Bridge* dsp, int note) {
    dsp->engine->noteOn(note, 0);
}