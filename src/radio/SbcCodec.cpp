#include "SbcCodec.h"

#include <algorithm>
#include <cstring>

namespace vr76 {

namespace {

constexpr int kHeaderSize = 4;

} // namespace

SbcCodec::SbcCodec() {
  encoderFrame_.msbc = false;
  encoderFrame_.freq = SBC_FREQ_32K;
  encoderFrame_.mode = SBC_MODE_MONO;
  encoderFrame_.bam = SBC_BAM_LOUDNESS;
  encoderFrame_.nblocks = 16;
  encoderFrame_.nsubbands = 8;
  encoderFrame_.bitpool = 18;
  reset();
}

SbcCodec::~SbcCodec() = default;

void SbcCodec::reset() {
  std::memset(&encoder_, 0, sizeof(encoder_));
  std::memset(&decoder_, 0, sizeof(decoder_));
  sbc_reset(&encoder_);
  sbc_reset(&decoder_);
  encoderFrame_.bitpool = bitpool_;
}

bool SbcCodec::encode(const int16_t *samples, size_t sampleCount,
                      std::vector<uint8_t> &out) {
  if (samples == nullptr || sampleCount < kSamplesPerFrame) {
    return false;
  }

  encoderFrame_.bitpool = bitpool_;
  const unsigned frameSize = sbc_get_frame_size(&encoderFrame_);
  if (frameSize == 0) {
    return false;
  }

  out.resize(frameSize);
  const int result =
      sbc_encode(&encoder_, samples, 1, nullptr, 0, &encoderFrame_,
                 out.data(), static_cast<unsigned>(out.size()));
  if (result != 0) {
    out.clear();
    return false;
  }
  return true;
}

bool SbcCodec::decode(const uint8_t *data, size_t size,
                      std::vector<int16_t> &pcm) {
  pcm.clear();
  if (data == nullptr || size < kHeaderSize) {
    return false;
  }

  size_t offset = 0;
  bool decodedAny = false;
  while (offset + kHeaderSize <= size) {
    // Standard SBC frames start with 0x9C. mSBC (0xAD) is not used by these
    // radios; stop if the stream is not standard SBC.
    if (data[offset] != 0x9C) {
      break;
    }

    struct sbc_frame frame{};
    if (sbc_probe(data + offset, &frame) != 0) {
      break;
    }
    const unsigned frameSize = sbc_get_frame_size(&frame);
    if (frameSize < kHeaderSize || offset + frameSize > size) {
      break;
    }

    const int samples = frame.nblocks * frame.nsubbands;
    if (samples <= 0 || samples > SBC_MAX_SAMPLES) {
      break;
    }

    std::vector<int16_t> block(static_cast<size_t>(samples));
    if (sbc_decode(&decoder_, data + offset, frameSize, &frame, block.data(), 1,
                   nullptr, 0) != 0) {
      break;
    }

    pcm.insert(pcm.end(), block.begin(), block.end());
    offset += frameSize;
    decodedAny = true;
  }

  return decodedAny;
}

} // namespace vr76
