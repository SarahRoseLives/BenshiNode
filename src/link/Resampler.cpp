#include "Resampler.h"

namespace vr76 {

void Downsampler32to8::reset() {
  sum_ = 0;
  count_ = 0;
}

void Downsampler32to8::process(const int16_t *samples, size_t count,
                               std::vector<int16_t> &out) {
  if (samples == nullptr) {
    return;
  }
  out.reserve(out.size() + count / 4 + 1);
  for (size_t i = 0; i < count; ++i) {
    sum_ += samples[i];
    if (++count_ == 4) {
      out.push_back(static_cast<int16_t>(sum_ / 4));
      sum_ = 0;
      count_ = 0;
    }
  }
}

void Upsampler8to32::reset() {
  previous_ = 0;
  havePrevious_ = false;
}

void Upsampler8to32::process(const int16_t *samples, size_t count,
                             std::vector<int16_t> &out) {
  if (samples == nullptr) {
    return;
  }
  out.reserve(out.size() + count * 4);
  for (size_t i = 0; i < count; ++i) {
    const int16_t current = samples[i];
    const int16_t start = havePrevious_ ? previous_ : current;
    for (int k = 0; k < 4; ++k) {
      const int32_t value =
          start + (static_cast<int32_t>(current) - start) * k / 4;
      out.push_back(static_cast<int16_t>(value));
    }
    previous_ = current;
    havePrevious_ = true;
  }
}

} // namespace vr76
