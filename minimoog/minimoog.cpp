#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#define PLUGIN_URI "https://github.com/lilbrimstone/minimoog"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

inline float clampf(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

inline float mtof(float note) {
    return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
}

inline float polyblep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    } else if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

inline float render_osc(float phase, float phase_inc, int wave_idx) {
    float out = 0.0f;
    if (wave_idx == 0) {
        out = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
    } 
    else if (wave_idx == 1) {
        float tri = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase);
        float saw = 1.0f - 2.0f * phase + polyblep(phase, phase_inc);
        out = (tri * 0.5f) + (saw * 0.5f);
    }
    else if (wave_idx == 2) {
        out = 1.0f - 2.0f * phase;
        out += polyblep(phase, phase_inc);
    }
    else {
        float pw = 0.5f; 
        if (wave_idx == 4) pw = 0.25f;  
        if (wave_idx == 5) pw = 0.16f;  
        
        out = (phase < pw) ? 1.0f : -1.0f;
        out -= (2.0f * pw - 1.0f); // Strip innate DC Offset
        
        out += polyblep(phase, phase_inc); 
        float fall_phase = phase + 1.0f - pw;
        if (fall_phase >= 1.0f) fall_phase -= 1.0f;
        out -= polyblep(fall_phase, phase_inc);
    }
    return out;
}

struct EnvADS {
    float val = 0.0f;
    int state = 0; 
    float a_rate = 0.0f, d_rate = 0.0f, r_rate = 0.0f, sus_level = 0.0f;

    void set(float a_ms, float d_ms, float sus_ui, float sr, bool decay_sw_on) {
        a_rate = 1.0f - std::exp(-1.0f / (a_ms * 0.001f * sr));
        d_rate = 1.0f - std::exp(-1.0f / (d_ms * 0.001f * sr));
        r_rate = decay_sw_on ? d_rate : (1.0f - std::exp(-1.0f / (10.0f * 0.001f * sr)));
        sus_level = clampf(sus_ui, 0.0f, 10.0f) * 0.1f;
    }

    void trigger() { state = 1; }
    void release() { state = 3; }

    float process() {
        if (state == 1) {
            val += a_rate * (1.2f - val); 
            if (val >= 1.0f) {
                val = 1.0f;
                state = 2; 
            }
        } else if (state == 2) {
            val += d_rate * (sus_level - val);
        } else if (state == 3) {
            val += r_rate * (0.0f - val);
            if (val < 0.001f) { val = 0.0f; state = 0; }
        }
        return val;
    }
};

struct ResonantDroop {
    float lp = 0.0f, bp = 0.0f, f = 0.0f, q = 0.0f;
    void init(float hz, float res, float sample_rate) {
        f = 2.0f * std::sin((float)M_PI * hz / sample_rate);
        float Q = 0.5f + (res * 9.5f);
        q = 1.0f / Q;
    }
    float process(float in) {
        float hp = in - lp - q * bp;
        bp += f * hp;
        lp += f * bp;
        return hp; 
    }
};

struct MoogLadder {
    float y[4];
    float g, res, comp_gain;
    
    MoogLadder() {
        for(int i=0; i<4; i++) y[i] = 0.0f;
        g = 0.0f; res = 0.0f; comp_gain = 1.0f;
    }

    inline float fast_tanh(float x) {
        float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    void set(float cutoff, float resonance_ui, float sr, bool bass_comp) {
        if (cutoff > 20000.0f) cutoff = 20000.0f;
        if (cutoff < 20.0f) cutoff = 20.0f;
        g = 1.0f - std::exp(-2.0f * (float)M_PI * cutoff / sr);
        res = resonance_ui * 4.0f; 
        comp_gain = bass_comp ? 1.0f + (resonance_ui * 1.0f) : 1.0f;
    }

    float process(float in) {
        float x = in - res * y[3];
        y[0] += g * (fast_tanh(x) - fast_tanh(y[0]));
        y[1] += g * (fast_tanh(y[0]) - fast_tanh(y[1]));
        y[2] += g * (fast_tanh(y[1]) - fast_tanh(y[2]));
        y[3] += g * (fast_tanh(y[2]) - fast_tanh(y[3]));
        return y[3] * comp_gain; 
    }
};

enum PortIndex {
    PORT_MIDI_IN = 0, PORT_OUT_L, PORT_OUT_R,
    
    // Performance & Controllers
    PORT_MASTER_VOL, PORT_MASTER_TUNE, PORT_GLIDE, 
    PORT_MOD_WHEEL, PORT_MOD_MIX, 
    PORT_MOD_SRC_1, PORT_MOD_SRC_2, 
    PORT_OSC_MOD, PORT_FILT_MOD, 
    PORT_LFO_RATE, PORT_LFO_WAVE, 
    
    // Oscillator Bank
    PORT_OSC1_RANGE, PORT_OSC1_WAVE,
    PORT_OSC2_RANGE, PORT_OSC2_FREQ, PORT_OSC2_WAVE,
    PORT_OSC3_RANGE, PORT_OSC3_FREQ, PORT_OSC3_WAVE, PORT_OSC3_CTRL,
    
    // Mixer
    PORT_MIX_OSC1_VOL, PORT_MIX_OSC2_VOL, PORT_MIX_OSC3_VOL, 
    PORT_MIX_NOISE_VOL, PORT_MIX_NOISE_TYPE, 
    
    // Modifiers (Filter)
    PORT_FILT_CUTOFF, PORT_FILT_EMPHASIS, PORT_FILT_CONTOUR,
    PORT_FILT_KB_TRACK, PORT_FILT_BASS_COMP, 
    
    // Modifiers (Envelopes)
    PORT_FILT_ATTACK, PORT_FILT_DECAY, PORT_FILT_SUSTAIN,
    PORT_AMP_ATTACK, PORT_AMP_DECAY, PORT_AMP_SUSTAIN,
    PORT_DECAY_SW,
    
    PORT_COUNT
};

struct MiniClone {
    const LV2_Atom_Sequence* midi_in;
    float* out_l;
    float* out_r;
    float* controls[PORT_COUNT];
    
    LV2_URID_Map* map;
    LV2_URID midi_event_uri;
    
    double sample_rate;
    double os_rate;
    
    float osc_phase[3];
    float lfo_phase;
    
    int held_keys[16];
    int num_held;
    int current_note;
    float current_note_cv;
    float target_note_cv;
    
    bool gate;
    float midi_mod_wheel_val;
    float midi_pitch_bend_val;
    
    EnvADS vcf_env;
    EnvADS vca_env;
    
    ResonantDroop droop_filter;
    MoogLadder vcf;
    
    float pink_b0, pink_b1, pink_b2, pink_b3, pink_b4;
    float red_noise_lp;

    MiniClone(double rate, LV2_URID_Map* map_feature) 
        : midi_in(nullptr), out_l(nullptr), out_r(nullptr), map(map_feature),
          sample_rate(rate), 
          lfo_phase(0.0f),
          num_held(0), current_note(-1), current_note_cv(60.0f), target_note_cv(60.0f), 
          gate(false), midi_mod_wheel_val(0.0f), midi_pitch_bend_val(0.0f),
          pink_b0(0.0f), pink_b1(0.0f), pink_b2(0.0f), pink_b3(0.0f), pink_b4(0.0f), red_noise_lp(0.0f)
    {
        for (int i = 0; i < PORT_COUNT; ++i) controls[i] = nullptr;
        for (int i = 0; i < 16; ++i) held_keys[i] = 0;
        os_rate = sample_rate * 8.0;
        for (int i=0; i<3; ++i) osc_phase[i] = 0.0f;
        if (map) { midi_event_uri = map->map(map->handle, LV2_MIDI__MidiEvent); }
        droop_filter.init(17.52f, 0.06f, os_rate);
    }
    
    void process_midi(const LV2_Atom_Event* ev) {
        if (ev->body.type == midi_event_uri) {
            const uint8_t* msg = (const uint8_t*)(ev + 1);
            int status = msg[0] & 0xF0;
            
            if (status == LV2_MIDI_MSG_CONTROLLER && msg[1] == 1) {
                midi_mod_wheel_val = msg[2] / 127.0f;
            }
            else if (status == 0xE0) {
                int pb = ((msg[2] & 0x7F) << 7) | (msg[1] & 0x7F);
                midi_pitch_bend_val = (pb - 8192) / 8192.0f;
            }
            else if (status == LV2_MIDI_MSG_NOTE_ON && msg[2] > 0) {
                int note = msg[1];
                bool found = false;
                for (int i=0; i<num_held; i++) { if (held_keys[i] == note) { found = true; break; } }
                
                if (!found && num_held < 16) {
                    held_keys[num_held++] = note;
                    current_note = note;
                    target_note_cv = (float)current_note;
                    
                    if (num_held == 1) { 
                        gate = true;
                        vca_env.trigger();
                        vcf_env.trigger();
                    }
                }
            }
            else if (status == LV2_MIDI_MSG_NOTE_OFF || (status == LV2_MIDI_MSG_NOTE_ON && msg[2] == 0)) {
                int note = msg[1];
                int pos = -1;
                for (int i=0; i<num_held; i++) {
                    if (held_keys[i] == note) { pos = i; break; }
                }
                
                if (pos != -1) {
                    for (int i=pos; i<num_held-1; i++) held_keys[i] = held_keys[i+1];
                    num_held--;

                    if (num_held == 0) {
                        gate = false;
                        vca_env.release();
                        vcf_env.release();
                    } else if (note == current_note) {
                        current_note = held_keys[num_held - 1];
                        target_note_cv = (float)current_note;
                    }
                }
            }
        }
    }
};

extern "C" {

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundle_path, const LV2_Feature* const* features) {
    LV2_URID_Map* map = nullptr;
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) { map = (LV2_URID_Map*)features[i]->data; }
    }
    if (!map) return nullptr;
    void* ptr = calloc(1, sizeof(MiniClone));
    if (!ptr) return nullptr;
    return new(ptr) MiniClone(rate, map);
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    MiniClone* self = (MiniClone*)instance;
    if (port == PORT_MIDI_IN) self->midi_in = (const LV2_Atom_Sequence*)data;
    else if (port == PORT_OUT_L) self->out_l = (float*)data;
    else if (port == PORT_OUT_R) self->out_r = (float*)data;
    else if (port >= PORT_MASTER_VOL && port < PORT_COUNT) self->controls[port] = (float*)data;
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    MiniClone* self = (MiniClone*)instance;
    
    if (!self->out_l || !self->out_r || !self->midi_in) return;
    for (int i = PORT_MASTER_VOL; i < PORT_COUNT; ++i) {
        if (!self->controls[i]) return;
    }
    
    bool decay_sw_on = (*self->controls[PORT_DECAY_SW] > 0.5f);

    float f_att_ui = clampf(*self->controls[PORT_FILT_ATTACK], 0.0f, 10.0f);
    float f_dec_ui = clampf(*self->controls[PORT_FILT_DECAY], 0.0f, 10.0f);
    float f_sus_ui = clampf(*self->controls[PORT_FILT_SUSTAIN], 0.0f, 10.0f);
    float a_att_ui = clampf(*self->controls[PORT_AMP_ATTACK], 0.0f, 10.0f);
    float a_dec_ui = clampf(*self->controls[PORT_AMP_DECAY], 0.0f, 10.0f);
    float a_sus_ui = clampf(*self->controls[PORT_AMP_SUSTAIN], 0.0f, 10.0f);

    float f_att_ms = 1.0f * std::pow(104000.0f, f_att_ui / 10.0f);
    float f_dec_ms = 4.0f * std::pow(10900.0f, f_dec_ui / 10.0f); 
    float a_att_ms = 1.0f * std::pow(10400.0f, a_att_ui / 10.0f);
    float a_dec_ms = 4.0f * std::pow(8150.0f, a_dec_ui / 10.0f);

    self->vcf_env.set(f_att_ms, f_dec_ms, f_sus_ui, self->sample_rate, decay_sw_on);
    self->vca_env.set(a_att_ms, a_dec_ms, a_sus_ui, self->sample_rate, decay_sw_on);

    LV2_ATOM_SEQUENCE_FOREACH(self->midi_in, ev) { self->process_midi(ev); }
    
    float vol = *self->controls[PORT_MASTER_VOL] * 0.1f; 

    float glide_ui = clampf(*self->controls[PORT_GLIDE], 0.0f, 10.0f);
    float glide_coeff = 1.0f;
    if (glide_ui > 0.1f) {
        float glide_ms = 1.0f * std::pow(10000.0f, glide_ui / 10.0f);
        glide_coeff = 1.0f - std::exp(-1.0f / (glide_ms * 0.001f * self->sample_rate)); 
    }
    
    float master_tune_val = clampf(*self->controls[PORT_MASTER_TUNE], -3.0f, 3.0f);
    float tune_mult = std::pow(2.0f, master_tune_val / 12.0f);
    
    float pb_mult = std::pow(2.0f, (self->midi_pitch_bend_val * 7.0f) / 12.0f);
    int track_mode = (int)clampf(*self->controls[PORT_FILT_KB_TRACK], 0.0f, 3.0f);

    float mix[3], noise_mix;
    mix[0] = clampf(*self->controls[PORT_MIX_OSC1_VOL] * 0.1f, 0.0f, 1.0f);
    mix[1] = clampf(*self->controls[PORT_MIX_OSC2_VOL] * 0.1f, 0.0f, 1.0f);
    mix[2] = clampf(*self->controls[PORT_MIX_OSC3_VOL] * 0.1f, 0.0f, 1.0f);
    noise_mix = clampf(*self->controls[PORT_MIX_NOISE_VOL] * 0.1f, 0.0f, 1.0f);
    
    int wave[3];
    wave[0] = (int)clampf(*self->controls[PORT_OSC1_WAVE], 0.0f, 5.0f);
    wave[1] = (int)clampf(*self->controls[PORT_OSC2_WAVE], 0.0f, 5.0f);
    wave[2] = (int)clampf(*self->controls[PORT_OSC3_WAVE], 0.0f, 5.0f);

    float oct_mult[3];
    int r1 = (int)*self->controls[PORT_OSC1_RANGE];
    int r2 = (int)*self->controls[PORT_OSC2_RANGE];
    int r3 = (int)*self->controls[PORT_OSC3_RANGE];
    
    oct_mult[0] = std::pow(2.0f, (r1 == 0) ? -4.0f : (float)(r1 - 3));
    oct_mult[1] = std::pow(2.0f, (r2 == 0) ? -4.0f : (float)(r2 - 3));
    oct_mult[2] = std::pow(2.0f, (r3 == 0) ? -4.0f : (float)(r3 - 3));

    float det_mult[3];
    det_mult[0] = 1.0f; 
    det_mult[1] = std::pow(2.0f, *self->controls[PORT_OSC2_FREQ] / 12.0f);
    det_mult[2] = std::pow(2.0f, *self->controls[PORT_OSC3_FREQ] / 12.0f);

    bool osc3_ctrl = (*self->controls[PORT_OSC3_CTRL] > 0.5f);
    bool mod_src1_is_filt = (*self->controls[PORT_MOD_SRC_1] > 0.5f);
    bool mod_src2_is_lfo = (*self->controls[PORT_MOD_SRC_2] > 0.5f);
    float mod_mix_val = clampf(*self->controls[PORT_MOD_MIX], 0.0f, 10.0f) * 0.1f;
    float lfo_inc = clampf(*self->controls[PORT_LFO_RATE], 0.05f, 200.0f) / self->os_rate;
    bool lfo_is_sq = (*self->controls[PORT_LFO_WAVE] > 0.5f);
    
    float active_mod_wheel = std::fmax(clampf(*self->controls[PORT_MOD_WHEEL], 0.0f, 1.0f), self->midi_mod_wheel_val);
    
    bool pitch_mod_active = (*self->controls[PORT_OSC_MOD] > 0.5f);
    bool filt_mod_active = (*self->controls[PORT_FILT_MOD] > 0.5f);

    float cutoff_ui = clampf(*self->controls[PORT_FILT_CUTOFF], 0.0f, 10.0f);
    float contour_ui = clampf(*self->controls[PORT_FILT_CONTOUR], 0.0f, 10.0f);
    float emphasis_ui = clampf(*self->controls[PORT_FILT_EMPHASIS] * 0.1f, 0.0f, 1.0f);
    bool bass_comp = (*self->controls[PORT_FILT_BASS_COMP] > 0.5f);
    
    float p[3] = {self->osc_phase[0], self->osc_phase[1], self->osc_phase[2]};
    bool use_pink_audio = (*self->controls[PORT_MIX_NOISE_TYPE] > 0.5f);

    for (uint32_t i = 0; i < n_samples; ++i) {
        
        if (glide_ui < 0.1f) {
            self->current_note_cv = self->target_note_cv; 
        } else {
            self->current_note_cv += glide_coeff * (self->target_note_cv - self->current_note_cv);
        }

        // Apply global Tune and global Pitch Bend 
        float mtof_base = mtof(self->current_note_cv) * pb_mult * tune_mult;
        float track_cv_boost = ((self->current_note_cv - 41.0f) / 12.0f) * (track_mode / 3.0f) * 0.9966f; 

        float base_inc[3] = {0.0f};
        if (mtof_base > 0.0f) {
            for(int o=0; o<3; ++o) {
                float hz = mtof_base;
                if (o == 2 && !osc3_ctrl) { hz = 65.406f; } // Osc3 fixed to sub/low rate if untracked
                
                float f = hz * oct_mult[o] * det_mult[o];
                if (f > 20000.0f) f = 20000.0f; 
                base_inc[o] = f / self->os_rate;
            }
        }

        float env_f = self->vcf_env.process();
        float env_a = self->vca_env.process();
        
        float mod_noise = 0.0f;
        float audio_noise = 0.0f;
        
        float white = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        self->pink_b0 = 0.99886f * self->pink_b0 + white * 0.0555179f;
        self->pink_b1 = 0.99332f * self->pink_b1 + white * 0.0750759f;
        self->pink_b2 = 0.96900f * self->pink_b2 + white * 0.1538520f;
        self->pink_b3 = 0.86650f * self->pink_b3 + white * 0.3104856f;
        self->pink_b4 = 0.55000f * self->pink_b4 + white * 0.5329522f;
        float pink = self->pink_b0 + self->pink_b1 + self->pink_b2 + self->pink_b3 + self->pink_b4 + white * 0.5362f;
        pink *= 0.11f; 
        
        self->red_noise_lp += (20.0f / self->sample_rate) * (pink - self->red_noise_lp);
        float red = self->red_noise_lp * 3.0f; 
        
        if (use_pink_audio) {
            audio_noise = pink;
            mod_noise = red; 
        } else {
            audio_noise = white;
            mod_noise = pink; 
        }
        
        float out = 0.0f;
        
        if (env_a > 0.001f || env_f > 0.001f || self->num_held > 0) { 
            float block_sum = 0.0f;
            
            for(int k = 0; k < 8; ++k) {
                float lfo_out = lfo_is_sq ? ((self->lfo_phase < 0.5f) ? 1.0f : -1.0f) : 
                                            ((self->lfo_phase < 0.5f) ? (4.0f * self->lfo_phase - 1.0f) : (3.0f - 4.0f * self->lfo_phase));
                self->lfo_phase += lfo_inc; 
                if (self->lfo_phase >= 1.0f) self->lfo_phase -= 1.0f;

                float osc3_mod_out = render_osc(p[2], base_inc[2], wave[2]);
                
                float min_src1 = mod_src1_is_filt ? env_f : osc3_mod_out;
                float min_src2 = mod_src2_is_lfo ? lfo_out : mod_noise;
                
                float raw_mod = (min_src1 * (1.0f - mod_mix_val)) + (min_src2 * mod_mix_val);
                float final_mod = raw_mod * active_mod_wheel;
                
                float active_inc[3];
                for(int j=0; j<3; ++j) active_inc[j] = base_inc[j];
                
                if (pitch_mod_active) {
                    float pitch_mult = exp2f(final_mod * 1.0f);
                    active_inc[0] *= pitch_mult;
                    active_inc[1] *= pitch_mult;
                }
                
                float current_cv = cutoff_ui + track_cv_boost + (contour_ui * env_f);
                if (filt_mod_active) {
                    current_cv += (final_mod * 4.0f); 
                }
                current_cv = clampf(current_cv, 0.0f, 10.0f);
                float current_cutoff = 14.5f * exp2f((current_cv * 0.1f) * 10.0337f); 
                self->vcf.set(current_cutoff, emphasis_ui, self->os_rate, bass_comp);
                
                float osc_sum = 0.0f;
                for(int o = 0; o < 3; ++o) {
                    if (mix[o] > 0.001f) {
                        osc_sum += render_osc(p[o], active_inc[o], wave[o]) * mix[o];
                    }
                    p[o] += active_inc[o];
                    if (p[o] >= 1.0f) p[o] -= 1.0f;
                }
                
                osc_sum += audio_noise * noise_mix;
                osc_sum = self->droop_filter.process(osc_sum);
                osc_sum += 0.03f;
                osc_sum = std::tanh(osc_sum);
                osc_sum = self->vcf.process(osc_sum);
                
                block_sum += osc_sum;
            }
            
            out = (block_sum * 0.125f);
            out *= env_a;
            out *= 0.8f; 
        }

        self->out_l[i] = out * vol;
        self->out_r[i] = out * vol;
    }
    
    for(int i=0; i<3; ++i) self->osc_phase[i] = p[i];
}

static void cleanup(LV2_Handle instance) {
    MiniClone* self = (MiniClone*)instance;
    self->~MiniClone();
    free(self);
}

static const LV2_Descriptor minimoog_descriptor = {
    PLUGIN_URI, instantiate, connect_port, nullptr, run, nullptr, cleanup, nullptr 
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &minimoog_descriptor : nullptr;
}

} // extern "C"