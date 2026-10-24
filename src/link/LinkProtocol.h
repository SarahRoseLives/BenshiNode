#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr76::link {

constexpr uint16_t kDefaultControlPort = 7300;
constexpr uint16_t kDefaultAudioPort = 7301;
constexpr uint8_t kProtocolVersion = 1;

constexpr uint32_t kAuthMagic = 0x31534E42;    // "BNS1"
constexpr uint32_t kAuthOkMagic = 0x4B4F4E42;  // "BNOK"
constexpr uint32_t kAuthFailMagic = 0x52454E42;// "BNER"
constexpr uint32_t kAudioMagic = 0x31444E42;   // "BND1"

enum class PacketType : uint8_t {
  Register = 1,
  Audio = 2,
  End = 3,
  Heartbeat = 4,
  ServerEnd = 5
};

enum PacketFlags : uint8_t {
  FlagStart = 0x01,
  FlagEnd = 0x02
};

struct AudioPacket {
  PacketType type = PacketType::Audio;
  uint8_t flags = 0;
  uint32_t session = 0;
  uint32_t sequence = 0;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> encodeAudioPacket(const AudioPacket &packet);
bool decodeAudioPacket(const uint8_t *data, size_t size, AudioPacket &packet);

struct AuthChallenge {
  uint8_t nonce[32] = {};
};

struct AuthResponse {
  std::string callsign;
  uint8_t digest[32] = {};
};

struct AuthOk {
  uint32_t session = 0;
  uint16_t udpPort = 0;
};

std::vector<uint8_t> encodeChallenge(const AuthChallenge &challenge);
bool decodeChallenge(const uint8_t *data, size_t size, AuthChallenge &challenge);

std::vector<uint8_t> encodeAuthResponse(const AuthResponse &response);
bool decodeAuthResponse(const uint8_t *data, size_t size, AuthResponse &response);

std::vector<uint8_t> encodeAuthOk(const AuthOk &ok);
bool decodeAuthOk(const uint8_t *data, size_t size, AuthOk &ok);

std::vector<uint8_t> encodeAuthFail();

} // namespace vr76::link
