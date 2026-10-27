#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "allstar/Iax2Protocol.h"
#include "link/Resampler.h"

class Config;
class Logger;

namespace vr76 {

class NodeEngine;

class AllStarClient {
public:
  AllStarClient(Logger &logger, const Config &config, NodeEngine &node);
  ~AllStarClient();

  AllStarClient(const AllStarClient &) = delete;
  AllStarClient &operator=(const AllStarClient &) = delete;

  bool start(std::string &error);
  void stop();
  bool connect(std::string &error);
  void disconnect();

  bool isConnected() const;
  bool isConnecting() const;
  bool configured() const;
  bool autoConnect() const;
  std::string nodeNumber() const;
  std::string host() const;

  void setConnectionCallback(std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    connectionCallback_ = std::move(callback);
  }

private:
  using Clock = std::chrono::steady_clock;

  enum class State {
    Idle,
    Connecting,
    Up,
    HungUp,
  };

  struct PendingFrame {
    std::vector<uint8_t> bytes;
    int retries = 0;
  };

  void workerLoop();
  void tickLocked(Clock::time_point now);
  void handleDatagramLocked(const uint8_t *data, size_t size);
  void handleFullFrameLocked(const iax2::Frame &frame);
  void handleMiniFrameLocked(const uint8_t *data, size_t size);
  void handleIaxLocked(const iax2::Frame &frame);
  void handleControlLocked(const iax2::Frame &frame);
  void sendVoiceLocked();

  void sendReliableLocked(uint8_t frameType, uint8_t subclass,
                          const std::vector<uint8_t> &payload);
  void sendAckLocked(uint8_t subclass, uint32_t timestamp);
  void sendRejectLocked(const std::string &cause);
  void sendRawLocked(const std::vector<uint8_t> &bytes);

  void ackPendingUpToLocked(uint8_t peerISeqno);
  void teardownLocked(const std::string &reason);
  void notifyConnectionLocked(bool connected);
  void pushUplink(const std::vector<int16_t> &pcm);
  uint32_t timestampLocked() const;
  bool resolveServer(std::string &error);

  static bool seqBefore(uint8_t a, uint8_t b);
  static bool frameCountsForSeq(uint8_t frameType, uint8_t subclass);
  static std::string md5Hex(const std::string &input);
  static uint32_t randomCallNumber();

  Logger &logger_;
  const Config &config_;
  NodeEngine &node_;

  std::string nodeNumber_;
  std::string host_;
  uint16_t port_ = 4569;
  std::string username_;
  std::string secret_;
  std::string callerId_;
  std::string callerNumber_;
  bool autoConnect_ = false;

  mutable std::mutex mutex_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  SOCKET socket_ = INVALID_SOCKET;
  sockaddr_in serverAddress_{};
  uint64_t rxCallbackId_ = 0;

  State state_ = State::Idle;
  uint16_t srcCall_ = 0;
  uint16_t dstCall_ = 0;
  uint8_t oseq_ = 0;
  uint8_t iseq_ = 0;
  uint32_t format_ = 0x04;
  bool hangingUp_ = false;
  bool connectionNotified_ = false;
  bool rxVoiceStreamStarted_ = false;
  bool txVoiceStreamStarted_ = false;
  uint32_t lastFullVoiceTs_ = 0;

  Clock::time_point callStart_{};
  Clock::time_point nextRetransmit_{};
  Clock::time_point nextPing_{};
  Clock::time_point nextTx_{};
  Clock::time_point lastVoiceTx_{};
  Clock::time_point connectDeadline_{};

  std::map<uint8_t, PendingFrame> pending_;
  std::vector<int16_t> uplink32k_;
  std::vector<int16_t> uplink8k_;
  vr76::Downsampler32to8 downsampler_;
  vr76::Upsampler8to32 upsampler_;

  std::function<void(bool)> connectionCallback_;
};

} // namespace vr76
