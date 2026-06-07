#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define CHORUS_URI "https://github.com/lilbrimstone/stereo_chorus"

#ifndef LV2_SYMBOL_EXPORT
#define LV2_SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

#define MAX_DELAY_MS 250.0f 
#define PI 3.14159265358979323846f
#define MAX_VOICES 8

// 4-Point Hermite Interpolation
static inline float hermite(float frac, float y0, float y1, float y2, float y3) {
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

typedef struct {
  float* out_l;
  float* out_r;
  LV2_Atom_Sequence* midi_out;
  
  const float* in_l;
  const float* in_r;
  const LV2_Atom_Sequence* midi_in;
  
  const float* p_rate;
  const float* p_depth;
  const float* p_feedback;
  const float* p_mix;
  const float* p_center;
  const float* p_hpf_cutoff;
  const float* p_voices;
  const float* p_spread;
  const float* p_width;
  
  float* delay_buffer[MAX_VOICES][2];
  uint32_t buffer_size;
  uint32_t write_pos;
  float lfo_phase[MAX_VOICES];
  float sample_rate;
  
  float lpf_state_l;
  float lpf_state_r;
  float hpf_state_l;
  float hpf_state_r;
  float hpf_prev_l;
  float hpf_prev_r;
} Plugin;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* p, const LV2_Feature* const* f) {
  (void)d; (void)p; (void)f;
  
  Plugin* self = (Plugin*)calloc(1, sizeof(Plugin));
  if (!self) return NULL;
  
  self->sample_rate = (float)rate;
  self->buffer_size = (uint32_t)(MAX_DELAY_MS * 0.001f * rate) + 1;
  
  for (int v = 0; v < MAX_VOICES; v++) {
    self->delay_buffer[v][0] = (float*)calloc(self->buffer_size, sizeof(float));
    self->delay_buffer[v][1] = (float*)calloc(self->buffer_size, sizeof(float));
    
    if (!self->delay_buffer[v][0] || !self->delay_buffer[v][1]) {
      for (int i = 0; i <= v; i++) free(self->delay_buffer[i][0]);
      for (int i = 0; i <= v; i++) free(self->delay_buffer[i][1]);
      free(self);
      return NULL;
    }
    self->lfo_phase[v] = (v * 0.25f * PI);
  }
  
  return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
  Plugin* self = (Plugin*)instance;
  switch (port) {
    case 0:  self->out_l        = (float*)data;                   break;
    case 1:  self->out_r        = (float*)data;                   break;
    case 2:  self->midi_out     = (LV2_Atom_Sequence*)data;       break;
    case 3:  self->in_l         = (const float*)data;             break;
    case 4:  self->in_r         = (const float*)data;             break;
    case 5:  self->midi_in      = (const LV2_Atom_Sequence*)data; break;
    case 6:  self->p_rate       = (const float*)data;             break;
    case 7:  self->p_depth      = (const float*)data;             break;
    case 8:  self->p_feedback   = (const float*)data;             break;
    case 9:  self->p_mix        = (const float*)data;             break;
    case 10: self->p_center     = (const float*)data;             break;
    case 11: self->p_hpf_cutoff = (const float*)data;             break;
    case 12: self->p_voices     = (const float*)data;             break;
    case 13: self->p_spread     = (const float*)data;             break;
    case 14: self->p_width      = (const float*)data;             break;
    default: break;
  }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
  Plugin* self = (Plugin*)instance;
  
  if (!self->out_l || !self->out_r) return;
  
  if (self->midi_in && self->midi_out) {
    const uint32_t capacity = self->midi_out->atom.size;
    lv2_atom_sequence_clear(self->midi_out);
    self->midi_out->atom.type = self->midi_in->atom.type;
    LV2_ATOM_SEQUENCE_FOREACH(self->midi_in, ev) {
        lv2_atom_sequence_append_event(self->midi_out, capacity, ev);
    }
  }

  // Parameter read with safe fallbacks
  float rate       = self->p_rate       ? *self->p_rate       : 0.5f;
  float depth_ms   = self->p_depth      ? *self->p_depth      : 2.0f;
  float feedback   = self->p_feedback   ? *self->p_feedback   : 0.0f;
  float mix        = self->p_mix        ? *self->p_mix        : 0.5f;
  float center_ms  = self->p_center     ? *self->p_center     : 3.0f;
  float hpf_cutoff = self->p_hpf_cutoff ? *self->p_hpf_cutoff : 40.0f;
  int num_voices   = self->p_voices     ? (int)(*self->p_voices) : 2;
  float spread     = self->p_spread     ? *self->p_spread     : 0.0f;
  float width      = self->p_width      ? *self->p_width      : 1.0f;
  
  const float* inL_ptr = self->in_l;
  const float* inR_ptr = self->in_r ? self->in_r : self->in_l;
  float* outL = self->out_l;
  float* outR = self->out_r;
  
  // Base Parameter Clamps
  if (rate < 0.01f) rate = 0.01f;
  
  if (center_ms < 1.0f) center_ms = 1.0f; 
  if (center_ms > 100.0f) center_ms = 100.0f; 
  
  // CRITICAL FIX: Limit Max Depth dynamically based on Current Center!
  // This prevents the read head from hitting the 2.0f safety boundary and 'jumping'.
  if (depth_ms < 0.0f) depth_ms = 0.0f;
  if (depth_ms > center_ms) depth_ms = center_ms; 
  
  if (feedback < 0.0f) feedback = 0.0f; 
  if (feedback > 0.95f) feedback = 0.95f;
  
  if (mix < 0.0f) mix = 0.0f; 
  if (mix > 1.0f) mix = 1.0f;
  
  if (hpf_cutoff < 20.0f) hpf_cutoff = 20.0f;
  
  if (num_voices < 1) num_voices = 1; 
  if (num_voices > 8) num_voices = 8;
  
  if (spread < 0.0f) spread = 0.0f; 
  if (spread > 1.0f) spread = 1.0f;
  
  if (width < 0.0f) width = 0.0f; 
  if (width > 1.0f) width = 1.0f;
  
  float dry_gain = cosf(mix * PI * 0.5f);
  float wet_gain = sinf(mix * PI * 0.5f);
  
  float phase_offset = width * PI; 
  float depth_samples = (depth_ms * 0.001f * self->sample_rate) * 0.5f;
  float lpf_coeff = 1.0f - expf(-2.0f * PI * 5000.0f / self->sample_rate);
  float hpf_alpha = expf(-2.0f * PI * hpf_cutoff / self->sample_rate);
  float lfo_inc_base = (2.0f * PI * rate) / self->sample_rate;
  
  float rate_multi[8] = {
    1.0f, 1.0f + spread*0.1f, 1.0f - spread*0.1f, 1.0f + spread*0.2f,
    1.0f - spread*0.2f, 1.0f + spread*0.3f, 1.0f - spread*0.3f, 1.0f + spread*0.4f
  };
  
  // Precalculate Static Equal-Power Panning
  float pan_l[8] = {0};
  float pan_r[8] = {0};
  for (int v = 0; v < num_voices; v++) {
    float pan_pos = 0.0f;
    if (num_voices > 1) {
      pan_pos = -1.0f + 2.0f * ((float)v / (num_voices - 1));
    }
    pan_pos *= width; 
    
    float angle = (pan_pos + 1.0f) * 0.25f * PI;
    pan_l[v] = cosf(angle) * 1.41421356f;
    pan_r[v] = sinf(angle) * 1.41421356f;
  }
  
  float norm_factor = sqrtf((float)num_voices);
  uint32_t buf_size = self->buffer_size;
  
  for (uint32_t n = 0; n < n_samples; ++n) {
    float input_l = inL_ptr ? inL_ptr[n] : 0.0f; 
    float input_r = inR_ptr ? inR_ptr[n] : 0.0f;
    float sum_l = 0.0f;
    float sum_r = 0.0f;
    
    for (int v = 0; v < num_voices; v++) {
      float spread_frac = (num_voices > 1) ? ((float)v / (float)(num_voices - 1)) : 0.0f;
      float v_center_samples = (center_ms + (center_ms * spread * spread_frac)) * 0.001f * self->sample_rate;
      
      float mod_l = sinf(self->lfo_phase[v]) * depth_samples;
      float ph_r = self->lfo_phase[v] + phase_offset;
      if (ph_r >= 2.0f * PI) ph_r -= 2.0f * PI;
      float mod_r = sinf(ph_r) * depth_samples;
      
      float d_l = v_center_samples + mod_l;  
      float d_r = v_center_samples + mod_r;
      
      if (d_l < 2.0f) d_l = 2.0f; 
      if (d_l >= buf_size - 3.0f) d_l = buf_size - 3.0f;
      
      if (d_r < 2.0f) d_r = 2.0f; 
      if (d_r >= buf_size - 3.0f) d_r = buf_size - 3.0f;
      
      // LEFT READ
      float rp_l = (float)self->write_pos - d_l;
      if (rp_l < 0.0f) rp_l += (float)buf_size;
      uint32_t i1l = (uint32_t)rp_l;
      float fl= rp_l - (float)i1l;
      uint32_t i0l = (i1l == 0) ? buf_size - 1 : i1l - 1;
      uint32_t i2l = i1l + 1; if (i2l >= buf_size) i2l = 0;
      uint32_t i3l = i2l + 1; if (i3l >= buf_size) i3l = 0;
      float out_vl = hermite(fl, self->delay_buffer[v][0][i0l], self->delay_buffer[v][0][i1l],
                                 self->delay_buffer[v][0][i2l], self->delay_buffer[v][0][i3l]);
                                 
      // RIGHT READ
      float rp_r = (float)self->write_pos - d_r;
      if (rp_r < 0.0f) rp_r += (float)buf_size;
      uint32_t i1r = (uint32_t)rp_r;
      float fr= rp_r - (float)i1r;
      uint32_t i0r = (i1r == 0) ? buf_size - 1 : i1r - 1;
      uint32_t i2r = i1r + 1; if (i2r >= buf_size) i2r = 0;
      uint32_t i3r = i2r + 1; if (i3r >= buf_size) i3r = 0;
      float out_vr = hermite(fr, self->delay_buffer[v][1][i0r], self->delay_buffer[v][1][i1r],
                                 self->delay_buffer[v][1][i2r], self->delay_buffer[v][1][i3r]);
      
      // Write Feedback
      self->delay_buffer[v][0][self->write_pos] = input_l + (out_vl * feedback);
      self->delay_buffer[v][1][self->write_pos] = input_r + (out_vr * feedback);
      
      // Sum with Panning
      sum_l += (out_vl * pan_l[v]) / norm_factor;
      sum_r += (out_vr * pan_r[v]) / norm_factor;
      
      self->lfo_phase[v] += (lfo_inc_base * rate_multi[v]);
      if (self->lfo_phase[v] >= 2.0f * PI) self->lfo_phase[v] -= 2.0f * PI;
    }
    
    // Filters
    self->lpf_state_l += lpf_coeff * (sum_l - self->lpf_state_l);
    self->lpf_state_r += lpf_coeff * (sum_r - self->lpf_state_r);
    self->hpf_state_l = hpf_alpha * (self->hpf_state_l + self->lpf_state_l - self->hpf_prev_l);
    self->hpf_state_r = hpf_alpha * (self->hpf_state_r + self->lpf_state_r - self->hpf_prev_r);
    self->hpf_prev_l = self->lpf_state_l;
    self->hpf_prev_r = self->lpf_state_r;
    
    // Mix out
    outL[n] = (input_l * dry_gain) + (self->hpf_state_l * wet_gain);
    outR[n] = (input_r * dry_gain) + (self->hpf_state_r * wet_gain);
    
    self->write_pos++;
    if (self->write_pos >= buf_size) self->write_pos = 0;
  }
}

static void cleanup(LV2_Handle instance) {
  Plugin* self = (Plugin*)instance;
  if (self) {
    for (int v = 0; v < MAX_VOICES; v++) {
      free(self->delay_buffer[v][0]);
      free(self->delay_buffer[v][1]);
    }
    free(self);
  }
}

static const LV2_Descriptor descriptor = {
  CHORUS_URI,
  instantiate,
  connect_port,
  NULL,
  run,
  NULL,
  cleanup,
  NULL
};

#ifdef __cplusplus
extern "C" {
#endif
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  return (index == 0) ? &descriptor : NULL;
}
#ifdef __cplusplus
}
#endif