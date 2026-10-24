#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr76::echolink {

struct Station {
  std::string callsign;
  std::string description;
  int id = 0;
  std::string ip;
};

std::vector<uint8_t> makeSdesPacket(const std::string &callsign,
                                    const std::string &name,
                                    const std::string &priv = "");
std::vector<uint8_t> makeByePacket();
bool isRtcpBye(const uint8_t *data, size_t size);
bool isRtcpSdes(const uint8_t *data, size_t size);
bool parseSdesItem(const uint8_t *data, size_t size, uint8_t itemType,
                   std::string &value);

std::vector<uint8_t> makeVoicePacket(uint16_t sequence,
                                     const uint8_t *gsmFrames,
                                     size_t gsmSize);
bool parseVoicePacket(const uint8_t *data, size_t size,
                      const uint8_t *&gsmFrames, size_t &gsmSize);

std::string makeDirectoryLogin(const std::string &callsign,
                               const std::string &password,
                               const std::string &description, bool busy);
std::string makeDirectoryListRequest();
bool parseDirectoryList(const std::string &data, std::vector<Station> &stations);
bool findStation(const std::vector<Station> &stations, const std::string &target,
                 Station &result);

std::string makeInfoPacket(const std::string &info);

} // namespace vr76::echolink
