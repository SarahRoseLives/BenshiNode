#include "WavClip.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

namespace vr76 {

namespace {

uint16_t read16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t read32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

int16_t clamp16(double value) {
  if (value > 32767.0) {
    return 32767;
  }
  if (value < -32768.0) {
    return -32768;
  }
  return static_cast<int16_t>(std::lround(value));
}

} // namespace

bool WavClip::load(const std::string &path, std::vector<int16_t> &pcm,
                   std::string &error, int targetSampleRate) {
  pcm.clear();
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Cannot open WAV file: " + path;
    return false;
  }

  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  if (bytes.size() < 44) {
    error = "WAV file is too short: " + path;
    return false;
  }
  if (std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
      std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
    error = "Not a RIFF/WAVE file: " + path;
    return false;
  }

  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  const uint8_t *data = nullptr;
  size_t dataSize = 0;

  size_t offset = 12;
  while (offset + 8 <= bytes.size()) {
    const uint8_t *chunk = bytes.data() + offset;
    const uint32_t chunkSize = read32(chunk + 4);
    const size_t payload = offset + 8;
    if (payload + chunkSize > bytes.size()) {
      break;
    }
    if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
      format = read16(bytes.data() + payload);
      channels = read16(bytes.data() + payload + 2);
      sampleRate = read32(bytes.data() + payload + 4);
      bitsPerSample = read16(bytes.data() + payload + 14);
    } else if (std::memcmp(chunk, "data", 4) == 0) {
      data = bytes.data() + payload;
      dataSize = chunkSize;
    }
    offset = payload + chunkSize + (chunkSize & 1U);
  }

  if (data == nullptr || channels == 0 || sampleRate == 0 || bitsPerSample == 0) {
    error = "WAV file has no usable fmt/data chunk: " + path;
    return false;
  }
  if (format != 1) {
    error = "Only uncompressed PCM WAV files are supported: " + path;
    return false;
  }
  if (bitsPerSample != 8 && bitsPerSample != 16) {
    error = "Only 8-bit or 16-bit PCM WAV files are supported: " + path;
    return false;
  }

  const size_t bytesPerSample = bitsPerSample / 8;
  const size_t frameSize = bytesPerSample * channels;
  if (frameSize == 0) {
    error = "Invalid WAV frame size: " + path;
    return false;
  }
  const size_t frames = dataSize / frameSize;
  if (frames == 0) {
    error = "WAV file has no audio frames: " + path;
    return false;
  }

  std::vector<int16_t> mono(frames);
  for (size_t i = 0; i < frames; ++i) {
    int32_t sum = 0;
    for (uint16_t ch = 0; ch < channels; ++ch) {
      const uint8_t *sample = data + i * frameSize + ch * bytesPerSample;
      int32_t value = 0;
      if (bitsPerSample == 8) {
        value = (static_cast<int32_t>(sample[0]) - 128) * 256;
      } else {
        value = static_cast<int16_t>(read16(sample));
      }
      sum += value;
    }
    mono[i] = static_cast<int16_t>(sum / channels);
  }

  if (static_cast<uint32_t>(targetSampleRate) == sampleRate) {
    pcm = std::move(mono);
    return true;
  }

  const double ratio = static_cast<double>(targetSampleRate) /
                       static_cast<double>(sampleRate);
  const size_t outFrames = std::max<size_t>(
      1, static_cast<size_t>(std::llround(frames * ratio)));
  pcm.resize(outFrames);
  for (size_t i = 0; i < outFrames; ++i) {
    const double inputPos = static_cast<double>(i) / ratio;
    const size_t index = static_cast<size_t>(inputPos);
    const double fraction = inputPos - static_cast<double>(index);
    const double a = mono[std::min(index, mono.size() - 1)];
    const double b = mono[std::min(index + 1, mono.size() - 1)];
    pcm[i] = clamp16(a + (b - a) * fraction);
  }

  return true;
}

} // namespace vr76
