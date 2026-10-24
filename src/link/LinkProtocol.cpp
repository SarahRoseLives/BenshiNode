#include "LinkProtocol.h"

#include <algorithm>
#include <cstring>

namespace vr76::link {

namespace {

constexpr size_t kChallengeSize = 40;
constexpr size_t kResponseSize = 52;
constexpr size_t kOkSize = 12;
constexpr size_t kFailSize = 8;
constexpr size_t kAudioHeaderSize = 16;

void put32le(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void put16le(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint32_t get32le(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t get16le(const uint8_t *data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

} // namespace

std::vector<uint8_t> encodeAudioPacket(const AudioPacket &packet) {
  const size_t payloadSize = std::min<size_t>(packet.payload.size(), 1024);
  std::vector<uint8_t> data(kAudioHeaderSize + payloadSize, 0);
  put32le(data, 0, kAudioMagic);
  data[4] = static_cast<uint8_t>(packet.type);
  data[5] = packet.flags;
  put16le(data, 6, static_cast<uint16_t>(payloadSize));
  put32le(data, 8, packet.session);
  put32le(data, 12, packet.sequence);
  if (payloadSize > 0) {
    std::memcpy(data.data() + kAudioHeaderSize, packet.payload.data(),
                payloadSize);
  }
  return data;
}

bool decodeAudioPacket(const uint8_t *data, size_t size, AudioPacket &packet) {
  if (data == nullptr || size < kAudioHeaderSize ||
      get32le(data) != kAudioMagic) {
    return false;
  }
  const uint16_t payloadSize = get16le(data + 6);
  if (kAudioHeaderSize + payloadSize > size) {
    return false;
  }
  packet.type = static_cast<PacketType>(data[4]);
  packet.flags = data[5];
  packet.session = get32le(data + 8);
  packet.sequence = get32le(data + 12);
  packet.payload.assign(data + kAudioHeaderSize,
                        data + kAudioHeaderSize + payloadSize);
  return true;
}

std::vector<uint8_t> encodeChallenge(const AuthChallenge &challenge) {
  std::vector<uint8_t> data(kChallengeSize, 0);
  put32le(data, 0, kAuthMagic);
  data[4] = kProtocolVersion;
  std::memcpy(data.data() + 8, challenge.nonce, 32);
  return data;
}

bool decodeChallenge(const uint8_t *data, size_t size,
                     AuthChallenge &challenge) {
  if (data == nullptr || size < kChallengeSize ||
      get32le(data) != kAuthMagic) {
    return false;
  }
  std::memcpy(challenge.nonce, data + 8, 32);
  return true;
}

std::vector<uint8_t> encodeAuthResponse(const AuthResponse &response) {
  std::vector<uint8_t> data(kResponseSize, 0);
  put32le(data, 0, kAuthMagic);
  const size_t callsignLength = std::min<size_t>(response.callsign.size(), 15);
  std::memcpy(data.data() + 4, response.callsign.data(), callsignLength);
  std::memcpy(data.data() + 20, response.digest, 32);
  return data;
}

bool decodeAuthResponse(const uint8_t *data, size_t size,
                        AuthResponse &response) {
  if (data == nullptr || size < kResponseSize ||
      get32le(data) != kAuthMagic) {
    return false;
  }
  char callsign[16] = {};
  std::memcpy(callsign, data + 4, 15);
  response.callsign = callsign;
  std::memcpy(response.digest, data + 20, 32);
  return true;
}

std::vector<uint8_t> encodeAuthOk(const AuthOk &ok) {
  std::vector<uint8_t> data(kOkSize, 0);
  put32le(data, 0, kAuthOkMagic);
  put32le(data, 4, ok.session);
  put16le(data, 8, ok.udpPort);
  return data;
}

bool decodeAuthOk(const uint8_t *data, size_t size, AuthOk &ok) {
  if (data == nullptr || size < kOkSize || get32le(data) != kAuthOkMagic) {
    return false;
  }
  ok.session = get32le(data + 4);
  ok.udpPort = get16le(data + 8);
  return true;
}

std::vector<uint8_t> encodeAuthFail() {
  std::vector<uint8_t> data(kFailSize, 0);
  put32le(data, 0, kAuthFailMagic);
  return data;
}

} // namespace vr76::link
