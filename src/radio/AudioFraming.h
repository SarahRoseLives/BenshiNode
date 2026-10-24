#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vr76 {

enum class AudioCommand : uint8_t {
  TransmitAudio = 0x00,
  AudioEnd = 0x01,
  AudioAck = 0x02,
  ReceivedAudioAlt = 0x03,
  TransmitEcho = 0x09
};

constexpr uint8_t kFrameDelimiter = 0x7E;
constexpr uint8_t kEscape = 0x7D;
constexpr uint8_t kEscapeXor = 0x20;

std::vector<uint8_t> frameAudio(uint8_t command,
                                const uint8_t *payload, size_t size);
std::vector<uint8_t> frameAudio(uint8_t command,
                                const std::vector<uint8_t> &payload);
std::vector<uint8_t> endAudioFrame();

std::vector<uint8_t> escapePayload(const uint8_t *data, size_t size);
std::vector<uint8_t> unescapePayload(const uint8_t *data, size_t size);

class AudioFrameParser {
public:
  void push(const uint8_t *data, size_t size);
  void clear() { buffer_.clear(); }
  bool next(std::vector<uint8_t> &frame);

private:
  std::vector<uint8_t> buffer_;
};

} // namespace vr76
