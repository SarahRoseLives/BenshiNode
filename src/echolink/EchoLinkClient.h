#pragma once

#include "Config.h"
#include "Logger.h"
#include "echolink/EchoLinkProtocol.h"
#include "link/Resampler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>

extern "C" {
#include <gsm.h>
}

namespace vr76 {
class NodeEngine;
}

namespace vr76::echolink {

class EchoLinkClient {
public:
  EchoLinkClient(Logger &logger, const Config &config, NodeEngine &node);
  ~EchoLinkClient();

  bool start(const std::string &target, std::string &error);
  void stop();
  bool isConnected() const { return connected_.load(); }

private:
  bool directoryLookup(const std::string &target, std::string &ip,
                       std::string &error);
  void run();
  void pushUplinkPcm(const std::vector<int16_t> &pcm);
  void sendSdes(SOCKET control, const sockaddr_in &remote);
  void sendInfo(SOCKET audio, const sockaddr_in &remote);
  void sendUplink(SOCKET audio, const sockaddr_in &remote, uint16_t &sequence);
  void processControl(const uint8_t *data, size_t size);
  void processAudio(const uint8_t *data, size_t size);

  Logger &logger_;
  const Config &config_;
  NodeEngine &node_;

  std::string callsign_;
  std::string password_;
  std::string name_;
  std::string info_;
  std::string directoryHost_;
  uint16_t directoryPort_ = 5200;
  uint16_t portBase_ = 5198;
  std::string directIp_;
  std::string target_;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread thread_;

  std::mutex uplinkMutex_;
  std::vector<int16_t> uplink32k_;
  std::vector<int16_t> uplink8k_;
  std::chrono::steady_clock::time_point lastUplink_{};
  Downsampler32to8 downsampler_;
  Upsampler8to32 upsampler_;

  gsm encoder_ = nullptr;
  gsm decoder_ = nullptr;
  std::string remoteCallsign_;
};

} // namespace vr76::echolink
