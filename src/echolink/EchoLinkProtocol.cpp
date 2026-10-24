#include "EchoLinkProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <sstream>

namespace vr76::echolink {

namespace {

constexpr uint8_t kRtcpSr = 200;
constexpr uint8_t kRtcpRr = 201;
constexpr uint8_t kRtcpSdes = 202;
constexpr uint8_t kRtcpBye = 203;

void put16be(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t get16be(const uint8_t *data) {
  return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

void appendSdesItem(std::vector<uint8_t> &data, uint8_t type,
                    const std::string &value) {
  const size_t length = std::min<size_t>(value.size(), 255);
  data.push_back(type);
  data.push_back(static_cast<uint8_t>(length));
  data.insert(data.end(), value.begin(), value.begin() + length);
}

void appendRrHeader(std::vector<uint8_t> &data) {
  data.push_back(0xC0); // RTP version 3, RR, zero report count
  data.push_back(kRtcpRr);
  data.push_back(0);
  data.push_back(1);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
}

void appendSdesHeader(std::vector<uint8_t> &data) {
  data.push_back(0xC1); // version 3, count 1
  data.push_back(kRtcpSdes);
  data.push_back(0);
  data.push_back(0); // length patched later
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
}

void patchLength(std::vector<uint8_t> &data, size_t offset) {
  const size_t words = data.size() - offset;
  const uint16_t length = static_cast<uint16_t>(words / 4 - 1);
  put16be(data, offset + 2, length);
}

std::string trim(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  return value.substr(first);
}

} // namespace

std::vector<uint8_t> makeSdesPacket(const std::string &callsign,
                                    const std::string &name,
                                    const std::string &priv) {
  std::vector<uint8_t> data;
  appendRrHeader(data);

  const size_t sdesOffset = data.size();
  appendSdesHeader(data);

  appendSdesItem(data, 1, callsign); // CNAME
  std::string displayName = callsign;
  if (displayName.size() < 15) {
    displayName.resize(15, ' ');
  }
  displayName += name;
  appendSdesItem(data, 2, displayName); // NAME
  appendSdesItem(data, 3, callsign);    // EMAIL
  appendSdesItem(data, 4, "08:30");     // PHONE
  if (!priv.empty()) {
    appendSdesItem(data, 8, priv); // PRIV
  }
  data.push_back(0); // SDES END
  while ((data.size() - sdesOffset) % 4 != 0) {
    data.push_back(0);
  }
  patchLength(data, sdesOffset);
  return data;
}

std::vector<uint8_t> makeByePacket() {
  std::vector<uint8_t> data;
  appendRrHeader(data);
  const size_t byeOffset = data.size();
  data.push_back(0xC1);
  data.push_back(kRtcpBye);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
  data.push_back(0);
  appendSdesItem(data, 0, ""); // placeholder replaced below
  data.pop_back();
  data.pop_back();
  const std::string text = "jan2002";
  data.push_back(static_cast<uint8_t>(text.size()));
  data.insert(data.end(), text.begin(), text.end());
  while ((data.size() - byeOffset) % 4 != 0) {
    data.push_back(0);
  }
  patchLength(data, byeOffset);
  return data;
}

bool isRtcpSdes(const uint8_t *data, size_t size) {
  if (data == nullptr || size < 8) {
    return false;
  }
  size_t offset = 0;
  while (offset + 4 <= size) {
    const uint8_t version = data[offset] >> 6;
    if (version != 3 && version != 1) {
      break;
    }
    const size_t packetLength = (get16be(data + offset + 2) + 1) * 4;
    if (packetLength < 4 || offset + packetLength > size) {
      break;
    }
    if (data[offset + 1] == kRtcpSdes && (data[offset] & 0x1F) > 0) {
      return true;
    }
    offset += packetLength;
  }
  return false;
}

bool isRtcpBye(const uint8_t *data, size_t size) {
  if (data == nullptr || size < 4) {
    return false;
  }
  size_t offset = 0;
  while (offset + 4 <= size) {
    const uint8_t version = data[offset] >> 6;
    if (version != 3 && version != 1) {
      break;
    }
    const size_t packetLength = (get16be(data + offset + 2) + 1) * 4;
    if (packetLength < 4 || offset + packetLength > size) {
      break;
    }
    if (data[offset + 1] == kRtcpBye) {
      return true;
    }
    offset += packetLength;
  }
  return false;
}

bool parseSdesItem(const uint8_t *data, size_t size, uint8_t itemType,
                   std::string &value) {
  value.clear();
  if (data == nullptr || size < 8) {
    return false;
  }
  size_t offset = 0;
  while (offset + 4 <= size) {
    const uint8_t version = data[offset] >> 6;
    if (version != 3 && version != 1) {
      break;
    }
    const size_t packetLength = (get16be(data + offset + 2) + 1) * 4;
    if (packetLength < 4 || offset + packetLength > size) {
      break;
    }
    if (data[offset + 1] == kRtcpSdes && (data[offset] & 0x1F) > 0) {
      const uint8_t *cursor = data + offset + 8;
      const uint8_t *end = data + offset + packetLength;
      while (cursor + 2 <= end) {
        const uint8_t type = cursor[0];
        const uint8_t length = cursor[1];
        if (type == 0) {
          break;
        }
        if (cursor + 2 + length > end) {
          break;
        }
        if (type == itemType) {
          value.assign(reinterpret_cast<const char *>(cursor + 2), length);
          return true;
        }
        cursor += 2 + length;
      }
      return false;
    }
    offset += packetLength;
  }
  return false;
}

std::vector<uint8_t> makeVoicePacket(uint16_t sequence,
                                     const uint8_t *gsmFrames,
                                     size_t gsmSize) {
  std::vector<uint8_t> data(12 + gsmSize);
  data[0] = 0xC0;
  data[1] = 0x03; // GSM
  put16be(data, 2, sequence);
  data[4] = 0;
  data[5] = 0;
  data[6] = 0;
  data[7] = 0;
  data[8] = 0;
  data[9] = 0;
  data[10] = 0;
  data[11] = 0;
  if (gsmSize > 0) {
    std::memcpy(data.data() + 12, gsmFrames, gsmSize);
  }
  return data;
}

bool parseVoicePacket(const uint8_t *data, size_t size,
                      const uint8_t *&gsmFrames, size_t &gsmSize) {
  if (data == nullptr || size < 12 || data[0] != 0xC0) {
    return false;
  }
  gsmFrames = data + 12;
  gsmSize = size - 12;
  return gsmSize > 0;
}

std::string makeDirectoryLogin(const std::string &callsign,
                               const std::string &password,
                               const std::string &description, bool busy) {
  std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char timeText[8] = {};
  std::strftime(timeText, sizeof(timeText), "%H:%M", &local);

  std::string status = busy ? "BUSY3.40(" : "ONLINE3.40(";
  status += timeText;
  status += ")";

  std::string command = "l";
  command += callsign;
  command += "\xAC\xAC";
  command += password;
  command += "\r";
  command += status;
  command += "\r";
  command += description;
  command += "\r";
  return command;
}

std::string makeDirectoryListRequest() { return "s"; }

bool parseDirectoryList(const std::string &data,
                        std::vector<Station> &stations) {
  stations.clear();
  std::istringstream input(data);
  std::string line;
  if (!std::getline(input, line) || trim(line) != "@@@") {
    return false;
  }
  if (!std::getline(input, line)) {
    return false;
  }
  int count = 0;
  try {
    count = std::stoi(trim(line));
  } catch (...) {
    return false;
  }

  for (int i = 0; i < count; ++i) {
    std::string callsign;
    std::string description;
    std::string idText;
    std::string ip;
    if (!std::getline(input, callsign) || !std::getline(input, description) ||
        !std::getline(input, idText) || !std::getline(input, ip)) {
      return false;
    }
    callsign = trim(callsign);
    description = trim(description);
    idText = trim(idText);
    ip = trim(ip);
    if (callsign.empty() || callsign == "." || idText.empty()) {
      continue;
    }
    Station station;
    station.callsign = callsign;
    station.description = description;
    try {
      station.id = std::stoi(idText);
    } catch (...) {
      continue;
    }
    station.ip = ip;
    if (!station.ip.empty() && station.ip != "127.0.0.1") {
      stations.push_back(std::move(station));
    }
  }
  return true;
}

bool findStation(const std::vector<Station> &stations, const std::string &target,
                 Station &result) {
  std::string upperTarget;
  for (char c : target) {
    upperTarget.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  for (const auto &station : stations) {
    if (std::to_string(station.id) == target) {
      result = station;
      return true;
    }
    std::string callsign = station.callsign;
    for (char &c : callsign) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (callsign == upperTarget) {
      result = station;
      return true;
    }
  }
  return false;
}

std::string makeInfoPacket(const std::string &info) {
  return std::string("oNDATA\r") + info + "\r";
}

} // namespace vr76::echolink
