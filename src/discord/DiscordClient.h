#pragma once

#include "Config.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dpp {
class cluster;
class voice_receive_t;
}

namespace vr76 {
class NodeEngine;
}

namespace vr76::discord {

class DiscordClient {
public:
  DiscordClient(Logger &logger, const Config &config, NodeEngine &node);
  ~DiscordClient();

  bool start(std::string &error);
  void stop();

  bool join(std::string &error);
  void leave();
  bool isVoiceReady() const { return voiceReady_.load(); }

private:
  void pushRadioPcm(const std::vector<int16_t> &pcm);
  void transmitLoop();
  void onVoiceReceive(const dpp::voice_receive_t &event);
  bool sendStereo(const std::vector<int16_t> &stereo);

  Logger &logger_;
  const Config &config_;
  NodeEngine &node_;

  std::string token_;
  uint64_t guildId_ = 0;
  uint64_t channelId_ = 0;
  bool autoJoin_ = false;

  std::unique_ptr<dpp::cluster> bot_;
  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> voiceReady_{false};
  std::thread transmitThread_;
  std::mutex txMutex_;
  std::vector<int16_t> tx32_;
  std::vector<int16_t> tx48Mono_;
};

} // namespace vr76::discord
