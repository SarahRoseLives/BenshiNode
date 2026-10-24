#include "AudioFraming.h"

#include <algorithm>

namespace vr76 {

std::vector<uint8_t> escapePayload(const uint8_t *data, size_t size) {
  std::vector<uint8_t> result;
  result.reserve(size + 16);
  for (size_t i = 0; i < size; ++i) {
    const uint8_t value = data[i];
    if (value == kEscape || value == kFrameDelimiter) {
      result.push_back(kEscape);
      result.push_back(static_cast<uint8_t>(value ^ kEscapeXor));
    } else {
      result.push_back(value);
    }
  }
  return result;
}

std::vector<uint8_t> unescapePayload(const uint8_t *data, size_t size) {
  std::vector<uint8_t> result;
  result.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    if (data[i] == kEscape) {
      if (i + 1 < size) {
        result.push_back(static_cast<uint8_t>(data[i + 1] ^ kEscapeXor));
        ++i;
      }
    } else {
      result.push_back(data[i]);
    }
  }
  return result;
}

std::vector<uint8_t> frameAudio(uint8_t command,
                                const uint8_t *payload, size_t size) {
  std::vector<uint8_t> result;
  const size_t escapedSize = size + 8;
  result.reserve(escapedSize + 3);
  result.push_back(kFrameDelimiter);
  result.push_back(command);
  for (size_t i = 0; i < size; ++i) {
    const uint8_t value = payload[i];
    if (value == kEscape || value == kFrameDelimiter) {
      result.push_back(kEscape);
      result.push_back(static_cast<uint8_t>(value ^ kEscapeXor));
    } else {
      result.push_back(value);
    }
  }
  result.push_back(kFrameDelimiter);
  return result;
}

std::vector<uint8_t> frameAudio(uint8_t command,
                                const std::vector<uint8_t> &payload) {
  return frameAudio(command, payload.data(), payload.size());
}

std::vector<uint8_t> endAudioFrame() {
  return {0x7E, 0x01, 0x00, 0x01, 0x00,
          0x00, 0x00, 0x00, 0x00, 0x00, 0x7E};
}

void AudioFrameParser::push(const uint8_t *data, size_t size) {
  if (data != nullptr && size > 0) {
    buffer_.insert(buffer_.end(), data, data + size);
  }
  if (buffer_.size() > 256 * 1024) {
    buffer_.erase(buffer_.begin(), buffer_.end() - 4096);
  }
}

bool AudioFrameParser::next(std::vector<uint8_t> &frame) {
  for (;;) {
    if (buffer_.size() < 2) {
      return false;
    }

    const auto start = std::find(buffer_.begin(), buffer_.end(), kFrameDelimiter);
    if (start == buffer_.end()) {
      buffer_.clear();
      return false;
    }
    if (start != buffer_.begin()) {
      buffer_.erase(buffer_.begin(), start);
    }
    if (buffer_.size() < 2) {
      return false;
    }

    const auto end =
        std::find(buffer_.begin() + 1, buffer_.end(), kFrameDelimiter);
    if (end == buffer_.end()) {
      return false;
    }

    if (end == buffer_.begin() + 1) {
      buffer_.erase(buffer_.begin());
      continue;
    }

    const size_t payloadSize =
        static_cast<size_t>(std::distance(buffer_.begin() + 1, end));
    frame = unescapePayload(buffer_.data() + 1, payloadSize);
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + std::distance(buffer_.begin(), end) + 1);
    return !frame.empty();
  }
}

} // namespace vr76
