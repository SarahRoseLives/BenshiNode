#include "AllStarClient.h"

#include "Config.h"
#include "Logger.h"
#include "node/NodeEngine.h"

#include "link/G711.h"
#include "link/LinkSocket.h"

#include <bcrypt.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

namespace vr76 {

namespace {

constexpr uint32_t kConnectTimeoutMs = 10000;
constexpr uint32_t kRetransmitMs = 1000;
constexpr int kMaxRetransmits = 5;
constexpr uint32_t kPingIntervalMs = 20000;
constexpr uint32_t kFullVoiceResyncMs = 32768;
constexpr size_t kFrameSamples = 160;
constexpr uint32_t kTxFrameMs = 20;
constexpr uint32_t kTxIdleResetMs = 300;
constexpr int kRxAudibleThreshold = 655;

bool isAudible(const std::vector<int16_t> &pcm) {
  int peak = 0;
  for (int16_t sample : pcm) {
    int value = sample;
    if (value < 0) {
      value = -value;
    }
    if (value > peak) {
      peak = value;
    }
  }
  return peak >= kRxAudibleThreshold;
}

} // namespace

AllStarClient::AllStarClient(Logger &logger, const Config &config,
                             NodeEngine &node)
    : logger_(logger), config_(config), node_(node) {
  nodeNumber_ = config.getString("ALLSTAR", "NODE", "");
  host_ = config.getString("ALLSTAR", "HOST", "");
  port_ = static_cast<uint16_t>(config.getInt("ALLSTAR", "PORT", 4569));
  if (port_ == 0) {
    port_ = iax2::kDefaultPort;
  }
  username_ = config.getString("ALLSTAR", "USER", "");
  secret_ = config.getString("ALLSTAR", "SECRET", "");
  callerId_ = config.getString(
      "ALLSTAR", "CALLERID", config.getString("NODE", "CALLSIGN", ""));
  callerNumber_ = config.getString("ALLSTAR", "CALLERNUMBER", "");
  autoConnect_ = config.getBool("ALLSTAR", "ENABLED", false);
  if (host_.empty() && !nodeNumber_.empty()) {
    host_ = nodeNumber_ + ".nodes.allstarlink.org";
  }
}

AllStarClient::~AllStarClient() { stop(); }

bool AllStarClient::configured() const {
  return !nodeNumber_.empty() && !host_.empty() && !username_.empty();
}

bool AllStarClient::autoConnect() const { return autoConnect_; }

std::string AllStarClient::nodeNumber() const { return nodeNumber_; }

std::string AllStarClient::host() const { return host_; }

bool AllStarClient::isConnected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == State::Up;
}

bool AllStarClient::isConnecting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == State::Connecting;
}

bool AllStarClient::resolveServer(std::string &error) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  addrinfo *result = nullptr;
  char portText[16]{};
  std::snprintf(portText, sizeof(portText), "%u",
                static_cast<unsigned>(port_));
  const int status =
      getaddrinfo(host_.c_str(), portText, &hints, &result);
  if (status != 0 || result == nullptr) {
    error = "Could not resolve AllStar host " + host_;
    return false;
  }
  std::memset(&serverAddress_, 0, sizeof(serverAddress_));
  std::memcpy(&serverAddress_, result->ai_addr, sizeof(sockaddr_in));
  freeaddrinfo(result);
  return true;
}

bool AllStarClient::start(std::string &error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
      return true;
    }
    if (!configured()) {
      error = "ALLSTAR/NODE, HOST, or USER is not configured";
      return false;
    }
  }

  const uint64_t callbackId = node_.addRxAudioCallback(
      [this](const std::vector<int16_t> &pcm) { pushUplink(pcm); });

  bool removeCallback = false;
  bool success = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
      removeCallback = true;
      success = true;
    } else {
      std::string socketError;
      if (!link::socketStartup(socketError)) {
        error = socketError;
      } else {
        if (socket_ == INVALID_SOCKET) {
          socket_ = link::createUdpSocket(0, socketError);
          if (socket_ == INVALID_SOCKET) {
            error = socketError;
          } else if (!link::setNonBlocking(socket_, true)) {
            error = "Could not configure AllStar socket";
            link::closeSocket(socket_);
            socket_ = INVALID_SOCKET;
          }
        }
        if (socket_ != INVALID_SOCKET && resolveServer(error)) {
          rxCallbackId_ = callbackId;
          state_ = State::Idle;
          connectionNotified_ = false;
          running_.store(true);
          thread_ = std::thread([this]() { workerLoop(); });
          success = true;
          logger_.info("AllStar client ready for node " + nodeNumber_ +
                       " at " + host_ + ":" + std::to_string(port_));
        } else {
          if (socket_ != INVALID_SOCKET) {
            link::closeSocket(socket_);
            socket_ = INVALID_SOCKET;
          }
        }
      }
      removeCallback = !success;
    }
  }

  if (removeCallback) {
    node_.removeRxAudioCallback(callbackId);
  }
  return success;
}

void AllStarClient::stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
  uint64_t callbackId = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callbackId = rxCallbackId_;
    rxCallbackId_ = 0;
  }
  if (callbackId != 0) {
    node_.removeRxAudioCallback(callbackId);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (socket_ != INVALID_SOCKET) {
      link::closeSocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    connectionCallback_ = nullptr;
    teardownLocked("");
    state_ = State::Idle;
  }
}

bool AllStarClient::connect(std::string &error) {
  if (!configured()) {
    error = "ALLSTAR/NODE, HOST, or USER is not configured";
    return false;
  }
  if (!running_.load()) {
    if (!start(error)) {
      return false;
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != State::Idle) {
    teardownLocked("Reconnecting");
  }
  srcCall_ = static_cast<uint16_t>(randomCallNumber());
  dstCall_ = 0;
  oseq_ = 0;
  iseq_ = 0;
  format_ = iax2::FormatUlaw;
  hangingUp_ = false;
  connectionNotified_ = false;
  rxVoiceStreamStarted_ = false;
  txVoiceStreamStarted_ = false;
  lastFullVoiceTs_ = 0;
  pending_.clear();
  uplink32k_.clear();
  uplink8k_.clear();
  downsampler_.reset();
  upsampler_.reset();
  const Clock::time_point now = Clock::now();
  callStart_ = now;
  nextRetransmit_ = now + std::chrono::milliseconds(kRetransmitMs);
  nextPing_ = now + std::chrono::milliseconds(kPingIntervalMs);
  nextTx_ = now;
  lastVoiceTx_ = now;
  connectDeadline_ = now + std::chrono::milliseconds(kConnectTimeoutMs);
  state_ = State::Connecting;

  iax2::IeSet ies;
  ies.addUint16(iax2::IeVersion, iax2::kProtocolVersion);
  ies.addString(iax2::IeCalledNumber, nodeNumber_);
  ies.addString(iax2::IeCallingNumber,
                callerNumber_.empty() ? username_ : callerNumber_);
  if (!callerId_.empty()) {
    ies.addString(iax2::IeCallingName, callerId_);
  }
  ies.addRaw(iax2::IeCallingPres, {0});
  ies.addRaw(iax2::IeCallingTon, {0});
  ies.addUint16(iax2::IeCallingTns, 0);
  ies.addString(iax2::IeUsername, username_);
  ies.addUint32(iax2::IeCapability, iax2::FormatUlaw);
  ies.addUint32(iax2::IeFormat, iax2::FormatUlaw);
  sendReliableLocked(iax2::FrameIax, iax2::IaxNew, ies.build());
  logger_.info("AllStar calling node " + nodeNumber_ + " at " + host_ + ":" +
               std::to_string(port_));
  return true;
}

void AllStarClient::disconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::Connecting || state_ == State::Up) {
    iax2::IeSet ies;
    ies.addString(iax2::IeCause, "Normal Clearing");
    ies.addByte(iax2::IeCauseCode, iax2::CauseNormalClearing);
    sendReliableLocked(iax2::FrameIax, iax2::IaxHangup, ies.build());
    hangingUp_ = true;
    state_ = State::HungUp;
    nextRetransmit_ = Clock::now() + std::chrono::milliseconds(kRetransmitMs);
    connectDeadline_ = Clock::time_point::max();
    nextPing_ = Clock::time_point::max();
    node_.enqueueNetworkAudio({}, true);
    notifyConnectionLocked(false);
    logger_.info("AllStar hangup sent");
    return;
  }
  teardownLocked("Disconnected");
}

void AllStarClient::pushUplink(const std::vector<int16_t> &pcm) {
  if (pcm.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_.load() || state_ == State::Idle) {
    return;
  }
  uplink32k_.insert(uplink32k_.end(), pcm.begin(), pcm.end());
  const size_t maximum = 32000 * 2;
  if (uplink32k_.size() > maximum) {
    uplink32k_.erase(uplink32k_.begin(),
                     uplink32k_.begin() +
                         static_cast<std::ptrdiff_t>(uplink32k_.size() -
                                                     maximum));
  }
}

void AllStarClient::workerLoop() {
  while (running_.load()) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket_, &readSet);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 10000;
    const int selected =
        select(0, &readSet, nullptr, nullptr, &timeout);
    if (!running_.load()) {
      break;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (selected > 0 && FD_ISSET(socket_, &readSet)) {
      uint8_t buffer[2048];
      for (;;) {
        sockaddr_storage from{};
        int fromLength = sizeof(from);
        const int received =
            recvfrom(socket_, reinterpret_cast<char *>(buffer),
                     sizeof(buffer), 0,
                     reinterpret_cast<sockaddr *>(&from), &fromLength);
        if (received <= 0) {
          break;
        }
        handleDatagramLocked(buffer, static_cast<size_t>(received));
      }
    }
    tickLocked(Clock::now());
  }
}

void AllStarClient::tickLocked(Clock::time_point now) {
  if (state_ == State::Connecting && now >= connectDeadline_) {
    logger_.warning("AllStar connect timed out");
    teardownLocked("Connect timed out");
    return;
  }

  if (!pending_.empty() && now >= nextRetransmit_) {
    bool dropped = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
      PendingFrame &pending = it->second;
      if (pending.retries >= kMaxRetransmits) {
        it = pending_.erase(it);
        dropped = true;
        continue;
      }
      pending.retries++;
      std::vector<uint8_t> bytes = pending.bytes;
      if (bytes.size() > 2) {
        bytes[2] = static_cast<uint8_t>(bytes[2] | 0x80);
      }
      sendRawLocked(bytes);
      ++it;
    }
    if (dropped) {
      if (!hangingUp_) {
        logger_.warning("AllStar no ACK after retransmits");
      }
      teardownLocked("No ACK from node");
      return;
    }
    nextRetransmit_ = now + std::chrono::milliseconds(kRetransmitMs);
  }

  if (state_ == State::HungUp && pending_.empty()) {
    teardownLocked("Hangup complete");
    return;
  }

  if (state_ == State::Up && now >= nextPing_) {
    sendReliableLocked(iax2::FrameIax, iax2::IaxPing, {});
    nextPing_ = now + std::chrono::milliseconds(kPingIntervalMs);
  }

  if (state_ != State::Up) {
    return;
  }

  if (!uplink32k_.empty()) {
    std::vector<int16_t> pcm32;
    pcm32.swap(uplink32k_);
    downsampler_.process(pcm32.data(), pcm32.size(), uplink8k_);
    const size_t maximum = kFrameSamples * 20;
    if (uplink8k_.size() > maximum) {
      uplink8k_.erase(uplink8k_.begin(),
                      uplink8k_.begin() +
                          static_cast<std::ptrdiff_t>(uplink8k_.size() -
                                                      maximum));
    }
  }

  int sent = 0;
  while (state_ == State::Up && sent < 4 && now >= nextTx_ &&
         uplink8k_.size() >= kFrameSamples) {
    sendVoiceLocked();
    uplink8k_.erase(uplink8k_.begin(),
                    uplink8k_.begin() +
                        static_cast<std::ptrdiff_t>(kFrameSamples));
    nextTx_ += std::chrono::milliseconds(kTxFrameMs);
    ++sent;
  }
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now - nextTx_)
          .count() > 200) {
    nextTx_ = now;
  }
  if (std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                            lastVoiceTx_)
          .count() > static_cast<long long>(kTxIdleResetMs)) {
    txVoiceStreamStarted_ = false;
  }
}

void AllStarClient::sendVoiceLocked() {
  if (state_ != State::Up || uplink8k_.size() < kFrameSamples) {
    return;
  }
  std::vector<uint8_t> payload(kFrameSamples);
  for (size_t i = 0; i < kFrameSamples; ++i) {
    payload[i] = vr76::linearToUlaw(uplink8k_[i]);
  }
  const uint32_t ts = timestampLocked();
  const bool needFull =
      !txVoiceStreamStarted_ ||
      (ts - lastFullVoiceTs_) >= kFullVoiceResyncMs ||
      ((ts & 0xFFFF) < (lastFullVoiceTs_ & 0xFFFF));
  if (needFull) {
    lastFullVoiceTs_ = ts;
    txVoiceStreamStarted_ = true;
    sendReliableLocked(iax2::FrameVoice,
                       static_cast<uint8_t>(iax2::FormatUlaw), payload);
  } else {
    sendRawLocked(
        iax2::encodeMini(srcCall_, static_cast<uint16_t>(ts & 0xFFFF),
                         payload));
  }
  lastVoiceTx_ = Clock::now();
}

void AllStarClient::handleDatagramLocked(const uint8_t *data, size_t size) {
  if (state_ == State::Idle || data == nullptr || size < 4) {
    return;
  }
  if (iax2::isFullFrame(data, size)) {
    iax2::Frame frame;
    if (iax2::decodeFull(data, size, frame)) {
      handleFullFrameLocked(frame);
    }
    return;
  }
  handleMiniFrameLocked(data, size);
}

void AllStarClient::handleFullFrameLocked(const iax2::Frame &frame) {
  if (frame.destination != 0 && frame.destination != srcCall_) {
    return;
  }
  if (dstCall_ == 0 && frame.source != 0) {
    dstCall_ = frame.source;
  }
  ackPendingUpToLocked(frame.iseq);
  if (hangingUp_) {
    if (pending_.empty()) {
      teardownLocked("Hangup acknowledged");
    }
    return;
  }
  if (frameCountsForSeq(frame.type, frame.subclass)) {
    if (frame.oseq != iseq_ && frame.type != iax2::FrameIax) {
      sendAckLocked(iax2::IaxAck, frame.timestamp);
      return;
    }
    iseq_ = static_cast<uint8_t>(frame.oseq + 1);
  }

  switch (frame.type) {
  case iax2::FrameIax:
    handleIaxLocked(frame);
    break;
  case iax2::FrameControl:
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    handleControlLocked(frame);
    break;
  case iax2::FrameVoice: {
    if (state_ != State::Up || frame.payload.empty()) {
      sendAckLocked(iax2::IaxAck, frame.timestamp);
      break;
    }
    rxVoiceStreamStarted_ = true;
    std::vector<int16_t> pcm8(frame.payload.size());
    for (size_t i = 0; i < frame.payload.size(); ++i) {
      pcm8[i] = vr76::ulawToLinear(frame.payload[i]);
    }
    std::vector<int16_t> pcm32;
    if (isAudible(pcm8)) {
      upsampler_.process(pcm8.data(), pcm8.size(), pcm32);
      node_.enqueueNetworkAudio(pcm32, false);
    }
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    break;
  }
  case iax2::FrameText: {
    if (!frame.payload.empty()) {
      std::string text(frame.payload.begin(), frame.payload.end());
      while (!text.empty() &&
             (text.back() == '\0' || text.back() == '\r' ||
              text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
      }
      if (!text.empty()) {
        logger_.info("AllStar: " + text);
      }
    }
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    break;
  }
  default:
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    break;
  }
}

void AllStarClient::handleMiniFrameLocked(const uint8_t *data, size_t size) {
  uint16_t source = 0;
  uint16_t timestamp = 0;
  std::vector<uint8_t> payload;
  if (!iax2::decodeMini(data, size, source, timestamp, payload)) {
    return;
  }
  if (state_ != State::Up || !rxVoiceStreamStarted_ || payload.empty()) {
    return;
  }
  if (dstCall_ != 0 && source != dstCall_) {
    return;
  }
  std::vector<int16_t> pcm8(payload.size());
  for (size_t i = 0; i < payload.size(); ++i) {
    pcm8[i] = vr76::ulawToLinear(payload[i]);
  }
  std::vector<int16_t> pcm32;
  if (isAudible(pcm8)) {
    upsampler_.process(pcm8.data(), pcm8.size(), pcm32);
    node_.enqueueNetworkAudio(pcm32, false);
  }
}

void AllStarClient::handleIaxLocked(const iax2::Frame &frame) {
  switch (frame.subclass) {
  case iax2::IaxAuthReq: {
    const iax2::IeSet ies =
        iax2::IeSet::parse(frame.payload.data(), frame.payload.size());
    const uint32_t methods = ies.getUint(iax2::IeAuthMethods);
    const std::string challenge = ies.getString(iax2::IeChallenge);
    if ((methods & iax2::AuthMd5) == 0 || challenge.empty()) {
      sendRejectLocked("Unsupported auth method");
      break;
    }
    iax2::IeSet reply;
    reply.addString(iax2::IeMd5Result, md5Hex(challenge + secret_));
    sendReliableLocked(iax2::FrameIax, iax2::IaxAuthRep, reply.build());
    logger_.info("AllStar authentication response sent");
    break;
  }
  case iax2::IaxAccept: {
    const iax2::IeSet ies =
        iax2::IeSet::parse(frame.payload.data(), frame.payload.size());
    const uint32_t format = ies.getUint(iax2::IeFormat);
    if (format != 0 && format != iax2::FormatUlaw) {
      logger_.warning("AllStar node selected unsupported format " +
                      std::to_string(format));
      sendRejectLocked("Unsupported audio format");
      break;
    }
    format_ = iax2::FormatUlaw;
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    nextPing_ = Clock::now() + std::chrono::milliseconds(kPingIntervalMs);
    logger_.info("AllStar call accepted (ulaw)");
    break;
  }
  case iax2::IaxAck:
    break;
  case iax2::IaxPing:
  case iax2::IaxPoke:
    sendAckLocked(iax2::IaxPong, frame.timestamp);
    break;
  case iax2::IaxPong:
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    break;
  case iax2::IaxLagRq:
    sendAckLocked(iax2::IaxLagRp, frame.timestamp);
    break;
  case iax2::IaxLagRp:
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    break;
  case iax2::IaxHangup: {
    const iax2::IeSet ies =
        iax2::IeSet::parse(frame.payload.data(), frame.payload.size());
    const std::string cause = ies.getString(iax2::IeCause);
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    teardownLocked(cause.empty() ? "Node hung up" : cause);
    break;
  }
  case iax2::IaxReject: {
    const iax2::IeSet ies =
        iax2::IeSet::parse(frame.payload.data(), frame.payload.size());
    const std::string cause = ies.getString(iax2::IeCause);
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    logger_.warning("AllStar call rejected" +
                    (cause.empty() ? std::string() : ": " + cause));
    teardownLocked(cause.empty() ? "Call rejected" : cause);
    break;
  }
  case iax2::IaxInval:
    logger_.warning("AllStar call is no longer valid");
    teardownLocked("Call invalid");
    break;
  default:
    sendAckLocked(iax2::IaxAck, frame.timestamp);
    break;
  }
}

void AllStarClient::handleControlLocked(const iax2::Frame &frame) {
  switch (frame.subclass) {
  case iax2::ControlAnswer:
    logger_.info("AllStar node answered");
    connectDeadline_ = Clock::time_point::max();
    if (state_ != State::Up) {
      state_ = State::Up;
      notifyConnectionLocked(true);
    }
    nextPing_ = Clock::now() + std::chrono::milliseconds(kPingIntervalMs);
    nextTx_ = Clock::now();
    break;
  case iax2::ControlHangup: {
    const iax2::IeSet ies =
        iax2::IeSet::parse(frame.payload.data(), frame.payload.size());
    const std::string cause = ies.getString(iax2::IeCause);
    teardownLocked(cause.empty() ? "Node hung up" : cause);
    break;
  }
  case iax2::ControlBusy:
    teardownLocked("Node busy");
    break;
  case iax2::ControlCongestion:
    teardownLocked("Network congestion");
    break;
  default:
    break;
  }
}

void AllStarClient::sendReliableLocked(uint8_t frameType, uint8_t subclass,
                                       const std::vector<uint8_t> &payload) {
  iax2::Frame frame;
  frame.source = srcCall_;
  frame.destination = dstCall_;
  frame.timestamp = timestampLocked();
  frame.oseq = oseq_;
  frame.iseq = iseq_;
  frame.type = frameType;
  frame.subclass = subclass;
  frame.payload = payload;
  const std::vector<uint8_t> bytes = iax2::encodeFull(frame);
  pending_[oseq_].bytes = bytes;
  pending_[oseq_].retries = 0;
  oseq_ = static_cast<uint8_t>(oseq_ + 1);
  sendRawLocked(bytes);
}

void AllStarClient::sendAckLocked(uint8_t subclass, uint32_t timestamp) {
  iax2::Frame frame;
  frame.source = srcCall_;
  frame.destination = dstCall_;
  frame.timestamp = timestamp;
  frame.oseq = oseq_;
  frame.iseq = iseq_;
  frame.type = iax2::FrameIax;
  frame.subclass = subclass;
  sendRawLocked(iax2::encodeFull(frame));
}

void AllStarClient::sendRejectLocked(const std::string &cause) {
  iax2::IeSet ies;
  ies.addString(iax2::IeCause, cause);
  sendReliableLocked(iax2::FrameIax, iax2::IaxReject, ies.build());
  teardownLocked(cause);
}

void AllStarClient::sendRawLocked(const std::vector<uint8_t> &bytes) {
  if (socket_ == INVALID_SOCKET) {
    return;
  }
  if (!link::sendTo(socket_,
                    reinterpret_cast<const sockaddr *>(&serverAddress_),
                    sizeof(serverAddress_), bytes.data(), bytes.size())) {
    logger_.debug("AllStar UDP send failed");
  }
}

void AllStarClient::ackPendingUpToLocked(uint8_t peerISeqno) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (seqBefore(it->first, peerISeqno)) {
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

void AllStarClient::teardownLocked(const std::string &reason) {
  const bool wasActive = state_ == State::Up || state_ == State::Connecting;
  state_ = State::Idle;
  hangingUp_ = false;
  pending_.clear();
  rxVoiceStreamStarted_ = false;
  txVoiceStreamStarted_ = false;
  lastFullVoiceTs_ = 0;
  uplink32k_.clear();
  uplink8k_.clear();
  downsampler_.reset();
  upsampler_.reset();
  if (wasActive) {
    node_.enqueueNetworkAudio({}, true);
  }
  notifyConnectionLocked(false);
  if (!reason.empty()) {
    logger_.info("AllStar disconnected: " + reason);
  }
  srcCall_ = 0;
  dstCall_ = 0;
  oseq_ = 0;
  iseq_ = 0;
}

void AllStarClient::notifyConnectionLocked(bool connected) {
  if (connectionNotified_ == connected) {
    return;
  }
  connectionNotified_ = connected;
  if (connectionCallback_) {
    connectionCallback_(connected);
  }
}

uint32_t AllStarClient::timestampLocked() const {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            callStart_)
          .count() &
      0xFFFFFFFF);
}

bool AllStarClient::seqBefore(uint8_t a, uint8_t b) {
  const uint8_t diff = static_cast<uint8_t>(b - a);
  return diff != 0 && diff < 128;
}

bool AllStarClient::frameCountsForSeq(uint8_t frameType, uint8_t subclass) {
  if (frameType != iax2::FrameIax) {
    return true;
  }
  switch (subclass) {
  case iax2::IaxAck:
  case iax2::IaxInval:
  case 0x1B:
  case 0x1C:
  case iax2::IaxVnak:
    return false;
  default:
    return true;
  }
}

std::string AllStarClient::md5Hex(const std::string &input) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) <
      0) {
    return {};
  }
  unsigned char digest[16]{};
  const NTSTATUS status =
      BCryptHash(algorithm, nullptr, 0,
                 reinterpret_cast<PUCHAR>(const_cast<char *>(input.data())),
                 static_cast<ULONG>(input.size()), digest, sizeof(digest));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) {
    return {};
  }
  static const char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(32);
  for (unsigned char value : digest) {
    result.push_back(kHex[(value >> 4) & 0x0F]);
    result.push_back(kHex[value & 0x0F]);
  }
  return result;
}

uint32_t AllStarClient::randomCallNumber() {
  static std::mt19937 generator(std::random_device{}());
  std::uniform_int_distribution<uint32_t> distribution(1, 0x7FFE);
  return distribution(generator);
}

} // namespace vr76
