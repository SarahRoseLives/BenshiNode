#include "G711.h"

namespace vr76 {

namespace {

constexpr int16_t kUlawBias = 0x84;
constexpr int16_t kUlawClip = 32635;

} // namespace

int16_t ulawToLinear(uint8_t ulaw) {
  const uint8_t value = static_cast<uint8_t>(~ulaw);
  const int sign = value & 0x80;
  const int exponent = (value >> 4) & 0x07;
  const int mantissa = value & 0x0F;
  int magnitude = ((mantissa << 3) + kUlawBias) << exponent;
  magnitude -= kUlawBias;
  const int16_t sample =
      static_cast<int16_t>(sign ? -magnitude : magnitude);
  return sample;
}

uint8_t linearToUlaw(int16_t pcm) {
  int sample = pcm;
  int sign = 0;
  if (sample < 0) {
    sample = -sample;
    sign = 0x80;
  }
  sample = sample > kUlawClip ? kUlawClip : sample;
  sample += kUlawBias;

  int exponent = 7;
  for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
    --exponent;
  }
  const int mantissa = (sample >> (exponent + 3)) & 0x0F;
  const uint8_t ulaw =
      static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
  return ulaw;
}

} // namespace vr76
