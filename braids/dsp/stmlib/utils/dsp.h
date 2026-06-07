#ifndef STMLIB_DSP_DSP_H_
#define STMLIB_DSP_DSP_H_

#include "stmlib/stmlib.h"
#include <math.h>

namespace stmlib {

#define MAKE_INTEGRAL_FRACTIONAL(x) \
  int32_t x ## _integral = static_cast<int32_t>(x); \
  float x ## _fractional = x - static_cast<float>(x ## _integral);

// --- Integer Interpolation ---

// 8 bits Integer (Index), 24 bits Fractional
template<typename T>
inline T Interpolate824(const T* table, uint32_t phase) {
  uint32_t integral = phase >> 24;
  uint32_t fractional = phase & 0xffffff;
  int32_t a = table[integral];
  int32_t b = table[integral + 1];
  // Cast to int64 to prevent overflow during multiply
  return a + (( (int64_t)(b - a) * fractional ) >> 24);
}

// 10 bits Integer, 22 bits Fractional
template<typename T>
inline T Interpolate1022(const T* table, uint32_t phase) {
  uint32_t integral = phase >> 22;
  uint32_t fractional = phase & 0x3fffff;
  int32_t a = table[integral];
  int32_t b = table[integral + 1];
  return a + (( (int64_t)(b - a) * fractional ) >> 22);
}

// 8 bits Integer, 8 bits Fractional
template<typename T>
inline T Interpolate88(const T* table, uint16_t phase) {
  uint16_t integral = phase >> 8;
  uint16_t fractional = phase & 0xff;
  int32_t a = table[integral];
  int32_t b = table[integral + 1];
  return a + (( (int64_t)(b - a) * fractional ) >> 8);
}

// --- Crossfades ---

inline float Crossfade(float a, float b, float fade) {
  return a + (b - a) * fade;
}

// Generic 4-arg Integer Crossfade (Preserves Input Type T)
template<typename T>
inline T Crossfade(const T* t1, const T* t2, uint32_t phase, uint16_t balance) {
  int32_t a = Interpolate824(t1, phase);
  int32_t b = Interpolate824(t2, phase);
  return a + (( (int64_t)(b - a) * balance ) >> 16);
}

// SPECIALIZATION for 8-bit Wavetables (uint8_t) -> outputs 16-bit (int16_t)
// Fixes low volume on wavetable models by expanding 0..255 to -32768..32767
inline int16_t Crossfade(const uint8_t* t1, const uint8_t* t2, uint32_t phase, uint16_t balance) {
  // 1. Interpolate to get smoothed 8-bit values (0..255)
  int32_t a_8 = Interpolate824(t1, phase);
  int32_t b_8 = Interpolate824(t2, phase);

  // 2. Expand to 16-bit bipolar range
  // (val - 128) centers it around 0. << 8 scales to 16-bit.
  int32_t a_16 = (a_8 - 128) << 8;
  int32_t b_16 = (b_8 - 128) << 8;

  // 3. Blend
  return a_16 + (( (int64_t)(b_16 - a_16) * balance ) >> 16);
}

inline int16_t Mix(int16_t a, int16_t b, uint16_t balance) {
  return (a * (65535 - balance) + b * balance) >> 16;
}

inline float InterpolateHermite(float y0, float y1, float y2, float y3, float x) {
  float c0 = y1;
  float c1 = 0.5f * (y2 - y0);
  float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
  return ((c3 * x + c2) * x + c1) * x + c0;
}

inline float Interpolate(const float* table, float index, float size) {
  index *= size;
  MAKE_INTEGRAL_FRACTIONAL(index)
  float a = table[index_integral];
  float b = table[index_integral + 1];
  return a + (b - a) * index_fractional;
}

#define CLIP(x) if (x < -32767) x = -32767; if (x > 32767) x = 32767;
#define M_PI 3.14159265358979323846f

}  // namespace stmlib

#endif  // STMLIB_DSP_DSP_H_