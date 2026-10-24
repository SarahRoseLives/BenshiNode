#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include <sbc.h>
}

namespace vr76 {

class SbcCodec {
public:
  SbcCodec();
  ~SbcCodec();
  SbcCodec(const SbcCodec &) = delete;
  SbcCodec &operator=(const SbcCodec &) = delete;

  void setBitpool(int bitpool) { bitpool_ = bitpool; }
  int bitpool() const { return bitpool_; }

  // Encode one 16-block / 8-subband mono frame (256 samples).
  bool encode(const int16_t *samples, size_t sampleCount,
              std::vector<uint8_t> &out);

  // Decode all complete SBC frames contained in [data, data+size).
  // Returns mono PCM samples at 32 kHz.
  bool decode(const uint8_t *data, size_t size,
              std::vector<int16_t> &pcm);

  void reset();

  static constexpr int kSampleRate = 32000;
  static constexpr int kSamplesPerFrame = 16 * 8;

private:
  struct sbc encoder_{};
  struct sbc decoder_{};
  int bitpool_ = 18;
  struct sbc_frame encoderFrame_{};
};

} // namespace vr76
