#pragma once

#include "Logger.h"
#include "link/LinkProtocol.h"
#include "link/Resampler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>

namespace vr76 {
class NodeEngine;
}

namespace vr76::link {

class LinkClient {
public:
  LinkClient(Logger &logger, NodeEngine &node);
  ~LinkClient();

  bool start(const std::string &host, uint16_t port, const std::string &key,
             const std::string &callsign, std::string &error);
  void stop();
  bool isConnected() const { return connected_.load(); }

private:
  bool authenticate(SOCKET &tcp, uint32_t &session, uint16_t &udpPort,
                    sockaddr_in &serverAddress, std::string &error);
  void run();
  void pushUplinkPcm(const std::vector<int16_t> &pcm);
  void processDownlink(const AudioPacket &packet);
  void sendUplinkFrames(SOCKET udp, const sockaddr_in &serverAddress);
  void sendEnd(SOCKET udp, const sockaddr_in &serverAddress);

  Logger &logger_;
  NodeEngine &node_;
  std::string host_;
  uint16_t port_ = kDefaultControlPort;
  std::string key_;
  std::string callsign_;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread thread_;

  std::mutex uplinkMutex_;
  std::vector<int16_t> uplink32k_;
  std::vector<int16_t> uplink8k_;
  std::chrono::steady_clock::time_point lastUplink_{};

  Downsampler32to8 downsampler_;
  Upsampler8to32 upsampler_;
  uint32_t session_ = 0;
  uint32_t sequence_ = 0;
  bool talking_ = false;
};

} // namespace vr76::link
