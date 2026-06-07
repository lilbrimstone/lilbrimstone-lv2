#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/time/time.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#ifndef LV2_SYMBOL_EXPORT
#define LV2_SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

#define PLUGIN_URI "https://github.com/lilbrimstone/tape-delay"
#define MAX_DELAY_SECONDS 4.0f
#define PI 3.14159265358979323846f

typedef enum {
  MODE_STEREO = 0,
  MODE_PING_PONG = 1,
  MODE_MONO = 2,
  MODE_CROSS = 3
} StereoMode;

typedef struct {
  LV2_URID atom_Blank;
  LV2_URID atom_Object;
  LV2_URID atom_Float;
  LV2_URID atom_Double;
  LV2_URID atom_Int;
  LV2_URID atom_Long;
  LV2_URID time_Position;
  LV2_URID time_beatsPerMinute;
  LV2_URID time_speed;
  LV2_URID midi_Event;
} URIDs;

typedef struct {
  const LV2_Atom_Sequence* control;
  LV2_Atom_Sequence*       midi_out;
  const float* in_l;
  const float* in_r;
  float* out_l;
  float* out_r;
  
  const float* p_stereo_mode;
  const float* p_delay_time;
  const float* p_feedback;
  const float* p_mix;
  const float* p_flutter_depth;
  const float* p_flutter_rate;
  const float* p_age;
  const float* p_drive;
  
  float* delay_buffer_l;
  float* delay_buffer_r;
  uint32_t buffer_size;
  uint32_t write_pos;
  float sample_rate;
  
  float smooth_delay_samples;
  
  // Age filter (in feedback path)
  float filt_z1_l;
  float filt_z1_r;
  float filt_alpha;
  float last_age;
  
  // Wow/Flutter: Smoothed random walk 
  float wow_target;
  float wow_current;
  float flutter_target1;
  float flutter_current1;
  float flutter_target2;
  float flutter_current2;
  uint32_t wow_counter;
  uint32_t flutter_counter1;
  uint32_t flutter_counter2;
  
  uint32_t noise_seed;
  float bpm;
  
  LV2_URID_Map* map;
  URIDs uris;
  
} Plugin;

static void map_uris(LV2_URID_Map* map, URIDs* uris) {
  // Always use hardcoded string literal URIs for S2400 compatibility
  uris->atom_Blank          = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Blank");
  uris->atom_Object         = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Object");
  uris->atom_Float          = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Float");
  uris->atom_Double         = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Double");
  uris->atom_Int            = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Int");
  uris->atom_Long           = map->map(map->handle, "http://lv2plug.in/ns/ext/atom#Long");
  uris->time_Position       = map->map(map->handle, "http://lv2plug.in/ns/ext/time#Position");
  uris->time_beatsPerMinute = map->map(map->handle, "http://lv2plug.in/ns/ext/time#beatsPerMinute");
  uris->time_speed          = map->map(map->handle, "http://lv2plug.in/ns/ext/time#speed");
  uris->midi_Event          = map->map(map->handle, "http://lv2plug.in/ns/ext/midi#MidiEvent");
}

static double read_number(const LV2_Atom* a, URIDs* u) {
    if (a->type == u->atom_Float)  return ((const LV2_Atom_Float*)a)->body;
    if (a->type == u->atom_Double) return ((const LV2_Atom_Double*)a)->body;
    if (a->type == u->atom_Int)    return ((const LV2_Atom_Int*)a)->body;
    if (a->type == u->atom_Long)   return ((const LV2_Atom_Long*)a)->body;
    return 0.0;
}

static inline float note_division_to_beats(uint32_t division) {
  switch (division) {
    case 0:  return 4.0f;      case 1:  return 2.0f;      case 2:  return 1.5f;     case 3:  return 1.0f;
    case 4:  return 0.666f;    case 5:  return 0.75f;     case 6:  return 0.5f;     case 7:  return 0.333f;
    case 8:  return 0.375f;    case 9:  return 0.25f;     case 10: return 0.125f;   case 11: return 0.0625f;
    default: return 1.0f;
  }
}

static inline float generate_noise(uint32_t* seed) {
  *seed = (*seed * 1103515245U + 12345U) & 0x7FFFFFFFU;
  return ((float)(*seed) / 2147483648.0f) - 1.0f;
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* p, const LV2_Feature* const* f) {
  Plugin* self = (Plugin*)calloc(1, sizeof(Plugin));
  if (!self) return NULL;
  
  self->map = NULL;
  const LV2_Feature* const* features = f;
  while (features && *features) {
    if (!strcmp((*features)->URI, LV2_URID__map)) {
      self->map = (LV2_URID_Map*)(*features)->data;
      break;
    }
    features++;
  }
  
  if (!self->map) { free(self); return NULL; }
  
  map_uris(self->map, &self->uris);
  self->sample_rate = (float)rate;
  
  self->buffer_size = (uint32_t)(MAX_DELAY_SECONDS * rate);
  self->delay_buffer_l = (float*)calloc(self->buffer_size, sizeof(float));
  self->delay_buffer_r = (float*)calloc(self->buffer_size, sizeof(float));
  
  if (!self->delay_buffer_l || !self->delay_buffer_r) {
    free(self->delay_buffer_l);
    free(self->delay_buffer_r);
    free(self);
    return NULL;
  }
  
  self->write_pos = 0;
  self->filt_z1_l = 0.0f;
  self->filt_z1_r = 0.0f;
  self->filt_alpha = 1.0f;
  self->last_age = -1.0f;
  self->bpm = 120.0f;
  
  float default_ms = (60000.0f / 120.0f) * 1.0f;
  self->smooth_delay_samples = (default_ms / 1000.0f) * self->sample_rate;
  self->noise_seed = 12345U;
  
  return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
  Plugin* self = (Plugin*)instance;
  switch (port) {
    case 0:  self->control         = (const LV2_Atom_Sequence*)data_location; break;
    case 1:  self->midi_out        = (LV2_Atom_Sequence*)data_location; break;
    case 2:  self->in_l            = (const float*)data_location; break;
    case 3:  self->in_r            = (const float*)data_location; break;
    case 4:  self->out_l           = (float*)data_location; break;
    case 5:  self->out_r           = (float*)data_location; break;
    case 6:  self->p_stereo_mode   = (const float*)data_location; break;
    case 7:  self->p_delay_time    = (const float*)data_location; break;
    case 8:  self->p_feedback      = (const float*)data_location; break;
    case 9:  self->p_mix           = (const float*)data_location; break;
    case 10: self->p_flutter_depth = (const float*)data_location; break;
    case 11: self->p_flutter_rate  = (const float*)data_location; break;
    case 12: self->p_age           = (const float*)data_location; break;
    case 13: self->p_drive         = (const float*)data_location; break;
    default: break;
  }
}

static inline float read_delay(float* buffer, uint32_t write_pos, uint32_t buffer_size, float delay_samples) {
  if (delay_samples < 1.0f) delay_samples = 1.0f;
  if (delay_samples >= (float)buffer_size) 
    delay_samples = (float)buffer_size - 1.0f;
  
  float read_pos_float = (float)write_pos - delay_samples;
  if (read_pos_float < 0.0f) read_pos_float += (float)buffer_size;
  
  uint32_t pos0 = (uint32_t)read_pos_float % buffer_size;
  uint32_t pos1 = (pos0 + 1) % buffer_size;
  float frac = read_pos_float - floorf(read_pos_float);
  
  return buffer[pos0] + frac * (buffer[pos1] - buffer[pos0]);
}

static inline void update_age_filter(Plugin* self, float age) {
  float cutoff_hz = 10000.0f - (age * 9200.0f);
  if (cutoff_hz < 800.0f) cutoff_hz = 800.0f;
  float rc = 1.0f / (2.0f * PI * cutoff_hz);
  float dt = 1.0f / self->sample_rate;
  self->filt_alpha = dt / (rc + dt);
  self->last_age = age;
}

static inline float lpf_process(float* z1, float alpha, float input) {
  *z1 = alpha * input + (1.0f - alpha) * (*z1);
  if (fabsf(*z1) < 1e-15f) *z1 = 0.0f;
  return *z1;
}

static inline float soft_saturate(float x) {
  if (x > 1.5f) return 1.0f;
  if (x < -1.5f) return -1.0f;
  float x2 = x * x;
  return x / (1.0f + x2 * 0.3f);
}

static void run(LV2_Handle instance, uint32_t n_samples) {
  Plugin* self = (Plugin*)instance;
  
  const float* inL = self->in_l;
  const float* inR = self->in_r;
  float* outL = self->out_l;
  float* outR = self->out_r;
  if (!outL || !outR) return;

  // --- MIDI PASSTHROUGH & TRANSPORT PARSING ---
  if (self->midi_out && self->control) {
    const uint32_t midi_capacity = self->midi_out->atom.size;
    lv2_atom_sequence_clear(self->midi_out);
    self->midi_out->atom.type = self->control->atom.type;

    LV2_ATOM_SEQUENCE_FOREACH(self->control, ev) {
      // 1. Pass MIDI straight through 
      if (ev->body.type == self->uris.midi_Event) {
          lv2_atom_sequence_append_event(self->midi_out, midi_capacity, ev);
      }
      
      // 2. Parse Host Transport Data
      if (ev->body.type == self->uris.atom_Object || ev->body.type == self->uris.atom_Blank) {
        const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
        if (obj->body.otype == self->uris.time_Position) {
          const LV2_Atom* bpm_atom = NULL;
          lv2_atom_object_get(obj, self->uris.time_beatsPerMinute, &bpm_atom, NULL);
          if (bpm_atom) {
            double v = read_number(bpm_atom, &self->uris);
            if (v >= 20.0 && v <= 300.0) self->bpm = (float)v;
          }
        }
      }
    }
  }
  
  // --- DSP PREP ---
  uint32_t stereo_mode = (uint32_t)(self->p_stereo_mode ? *self->p_stereo_mode : 0.0f);
  float delay_param = self->p_delay_time ? *self->p_delay_time : 3.0f;
  float feedback = self->p_feedback ? *self->p_feedback : 0.5f;
  float mix = self->p_mix ? *self->p_mix : 0.5f;
  float flutter_depth = self->p_flutter_depth ? *self->p_flutter_depth : 0.0f;
  float flutter_rate = self->p_flutter_rate ? *self->p_flutter_rate : 0.5f;
  float age = self->p_age ? *self->p_age : 0.0f;
  float drive = self->p_drive ? *self->p_drive : 0.0f;
  
  if (stereo_mode > 3) stereo_mode = 0;
  
  uint32_t division = (uint32_t)delay_param;
  float beats = note_division_to_beats(division);
  float delay_ms = (60000.0f / self->bpm) * beats;
  if (delay_ms < 1.0f) delay_ms = 1.0f;
  if (delay_ms > 4000.0f) delay_ms = 4000.0f;
  
  if (fabsf(age - self->last_age) > 0.01f) update_age_filter(self, age);
  
  float target_delay_samples = (delay_ms / 1000.0f) * self->sample_rate;
  float smooth_coeff = 0.001f;
  
  uint32_t wow_update_rate = (uint32_t)(self->sample_rate * 0.5f);
  float wow_smooth = 2.0f / self->sample_rate;
  uint32_t flutter_update_rate1 = (uint32_t)(self->sample_rate / (flutter_rate * 2.0f));
  float flutter_smooth1 = (flutter_rate * 4.0f) / self->sample_rate;
  uint32_t flutter_update_rate2 = (uint32_t)(self->sample_rate / (flutter_rate * 10.0f));
  float flutter_smooth2 = (flutter_rate * 12.0f) / self->sample_rate;
  
  float flutter_max_samples = (flutter_depth * 50.0f / 1000.0f) * self->sample_rate;
  float hiss_amp = age * 0.001f;
  
  // --- DSP LOOP ---
  for (uint32_t n = 0; n < n_samples; ++n) {
    float xL = inL ? inL[n] : 0.0f;
    float xR = inR ? inR[n] : 0.0f;
    
    self->smooth_delay_samples += smooth_coeff * (target_delay_samples - self->smooth_delay_samples);
    
    self->wow_counter++;
    if (self->wow_counter >= wow_update_rate) { self->wow_counter = 0; self->wow_target = generate_noise(&self->noise_seed); }
    self->flutter_counter1++;
    if (self->flutter_counter1 >= flutter_update_rate1) { self->flutter_counter1 = 0; self->flutter_target1 = generate_noise(&self->noise_seed); }
    self->flutter_counter2++;
    if (self->flutter_counter2 >= flutter_update_rate2) { self->flutter_counter2 = 0; self->flutter_target2 = generate_noise(&self->noise_seed); }
    
    self->wow_current += wow_smooth * (self->wow_target - self->wow_current);
    self->flutter_current1 += flutter_smooth1 * (self->flutter_target1 - self->flutter_current1);
    self->flutter_current2 += flutter_smooth2 * (self->flutter_target2 - self->flutter_current2);
    
    float combined_modulation = (self->wow_current * 0.6f) + (self->flutter_current1 * 0.3f) + (self->flutter_current2 * 0.1f);
    float modulated_delay = self->smooth_delay_samples + combined_modulation * flutter_max_samples;
    
    float delayedL = read_delay(self->delay_buffer_l, self->write_pos, self->buffer_size, modulated_delay);
    float delayedR = read_delay(self->delay_buffer_r, self->write_pos, self->buffer_size, modulated_delay);
    
    float wetL, wetR;
    if (drive > 0.01f) {
      float drive_gain = 1.0f + (drive * 0.5f);
      wetL = soft_saturate(delayedL * drive_gain) * (1.0f + (drive * 0.1f));
      wetR = soft_saturate(delayedR * drive_gain) * (1.0f + (drive * 0.1f));
    } else {
      wetL = delayedL; wetR = delayedR;
    }
    
    wetL = lpf_process(&self->filt_z1_l, self->filt_alpha, wetL);
    wetR = lpf_process(&self->filt_z1_r, self->filt_alpha, wetR);
    
    float hiss = (age > 0.01f) ? generate_noise(&self->noise_seed) * hiss_amp : 0.0f;
    
    float write_to_L, write_to_R, output_wetL, output_wetR;
    
    switch (stereo_mode) {
      case MODE_MONO:
        write_to_L = write_to_R = ((xL + xR) * 0.5f) + (((wetL + wetR) * 0.5f) + hiss) * feedback;
        output_wetL = output_wetR = (wetL + wetR) * 0.5f;
        break;
      case MODE_PING_PONG:
        write_to_L = ((xL + xR) * 0.5f) + (wetR + hiss) * feedback;
        write_to_R = (wetL + hiss) * feedback;
        output_wetL = wetL; output_wetR = wetR;
        break;
      case MODE_CROSS:
        write_to_L = xL + (wetR + hiss) * feedback;
        write_to_R = xR + (wetL + hiss) * feedback;
        output_wetL = wetL; output_wetR = wetR;
        break;
      case MODE_STEREO:
      default:
        write_to_L = xL + (wetL + hiss) * feedback;
        write_to_R = xR + (wetR + hiss) * feedback;
        output_wetL = wetL; output_wetR = wetR;
        break;
    }
    
    self->delay_buffer_l[self->write_pos] = write_to_L;
    self->delay_buffer_r[self->write_pos] = write_to_R;
    
    float dry_gain = cosf(mix * PI * 0.5f);
    float wet_gain = sinf(mix * PI * 0.5f);
    
    outL[n] = xL * dry_gain + output_wetL * wet_gain;
    outR[n] = xR * dry_gain + output_wetR * wet_gain;
    
    self->write_pos = (self->write_pos + 1) % self->buffer_size;
  }
}

static void cleanup(LV2_Handle instance) {
  Plugin* self = (Plugin*)instance;
  if (self) {
    free(self->delay_buffer_l);
    free(self->delay_buffer_r);
    free(self);
  }
}

static const LV2_Descriptor descriptor = { PLUGIN_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return (index == 0) ? &descriptor : NULL; }