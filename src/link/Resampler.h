#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vr76 {

// 32 kHz radio audio -> 8 kHz link audio. Averaging four samples acts as a
// simple anti-alias filter and keeps the link rate low.
class Downsampler32to8 {
public:
  void reset();
  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &out);

private:
  int32_t sum_ = 0;
  int count_ = 0;
};

// 8 kHz link audio -> 32 kHz radio audio, linear interpolation.
class Upsampler8to32 {
public:
  void reset();
  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &out);

private:
  int16_t previous_ = 0;
  bool havePrevious_ = false;
};

} // namespace vr76
