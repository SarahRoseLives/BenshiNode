#pragma once

#include "Config.h"
#include "Logger.h"
#include "link/LinkProtocol.h"
#include "link/LinkSocket.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vr76::link {

class LinkServer {
public:
  LinkServer(Logger &logger, const Config &config);
  ~LinkServer();

  bool start(std::string &error);
  void stop();

  uint16_t controlPort() const { return controlPort_; }
  uint16_t audioPort() const { return audioPort_; }

private:
  struct Session {
    uint32_t id = 0;
    std::string callsign;
    sockaddr_storage address{};
    int addressLength = 0;
    bool hasAddress = false;
    std::chrono::steady_clock::time_point lastSeen{};
  };

  void acceptLoop();
  void clientThread(SOCKET socket);
  void udpLoop();
  void cleanupSessions(std::chrono::steady_clock::time_point now);
  void relayAudio(const AudioPacket &packet, uint32_t fromSession);

  Logger &logger_;
  uint16_t controlPort_ = kDefaultControlPort;
  uint16_t audioPort_ = kDefaultAudioPort;
  std::string key_;
  size_t maxClients_ = 16;

  std::atomic<bool> running_{false};
  SOCKET listener_ = static_cast<SOCKET>(-1);
  SOCKET udp_ = static_cast<SOCKET>(-1);
  std::thread acceptThread_;
  std::thread udpThread_;
  std::vector<std::thread> clientThreads_;
  std::mutex clientThreadsMutex_;

  std::mutex sessionsMutex_;
  std::map<uint32_t, Session> sessions_;
  uint32_t nextSession_ = 1;
  uint32_t activeTalker_ = 0;
  std::chrono::steady_clock::time_point lastTalkerAudio_{};
};

} // namespace vr76::link
