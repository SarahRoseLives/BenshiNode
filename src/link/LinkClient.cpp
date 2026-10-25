#include "LinkClient.h"

#include "link/G711.h"
#include "link/LinkCrypto.h"
#include "link/LinkSocket.h"
#include "node/NodeEngine.h"

#include <algorithm>
#include <cstring>

namespace vr76::link {

namespace {

constexpr int kReconnectMs = 5000;
constexpr int kHeartbeatMs = 5000;
constexpr int kUplinkEndMs = 300;
constexpr size_t kFrameSamples = 160; // 20 ms at 8 kHz

} // namespace

LinkClient::LinkClient(Logger &logger, NodeEngine &node)
    : logger_(logger), node_(node) {}

LinkClient::~LinkClient() { stop(); }

bool LinkClient::start(const std::string &host, uint16_t port,
                       const std::string &key, const std::string &callsign,
                       std::string &error) {
  if (running_.load()) {
    return true;
  }
  std::string socketError;
  if (!socketStartup(socketError)) {
    error = socketError;
    return false;
  }
  if (rxCallbackId_ == 0) {
    rxCallbackId_ = node_.addRxAudioCallback(
        [this](const std::vector<int16_t> &pcm) { pushUplinkPcm(pcm); });
  }
  diagSamplesIn_.store(0);
  diagFramesOut_.store(0);
  lastDiag_ = std::chrono::steady_clock::now();
  host_ = host;
  port_ = port;
  key_ = key;
  callsign_ = callsign.empty() ? "BenshiNode" : callsign;
  running_.store(true);
  thread_ = std::thread([this]() { run(); });
  logger_.info("Linking to server " + host_ + ":" + std::to_string(port_));
  return true;
}

void LinkClient::stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
  connected_.store(false);
  if (rxCallbackId_ != 0) {
    node_.removeRxAudioCallback(rxCallbackId_);
    rxCallbackId_ = 0;
  }
}

bool LinkClient::authenticate(SOCKET &tcp, uint32_t &session,
                              uint16_t &udpPort, sockaddr_in &serverAddress,
                              std::string &error) {
  tcp = connectTcp(host_, port_, 5000, error);
  if (tcp == INVALID_SOCKET) {
    return false;
  }

  uint8_t challengeData[40] = {};
  if (!recvAll(tcp, challengeData, sizeof(challengeData), 4000)) {
    error = "Timed out waiting for authentication challenge";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }
  AuthChallenge challenge;
  if (!decodeChallenge(challengeData, sizeof(challengeData), challenge)) {
    error = "Invalid authentication challenge";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }

  AuthResponse response;
  response.callsign = callsign_;
  if (!key_.empty() &&
      !hmacSha256(key_, challenge.nonce, sizeof(challenge.nonce),
                  response.digest)) {
    error = "Could not calculate authentication digest";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }

  const auto responseData = encodeAuthResponse(response);
  if (!sendAll(tcp, responseData.data(), responseData.size())) {
    error = "Could not send authentication response";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }

  uint8_t okData[12] = {};
  if (!recvAll(tcp, okData, sizeof(okData), 4000)) {
    error = "Timed out waiting for authentication result";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }
  AuthOk ok;
  if (!decodeAuthOk(okData, sizeof(okData), ok)) {
    error = "Authentication rejected";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }

  sockaddr_storage peer{};
  int peerLength = sizeof(peer);
  if (getpeername(tcp, reinterpret_cast<sockaddr *>(&peer), &peerLength) != 0) {
    error = "Could not determine server address";
    closeSocket(tcp);
    tcp = INVALID_SOCKET;
    return false;
  }
  std::memset(&serverAddress, 0, sizeof(serverAddress));
  serverAddress = *reinterpret_cast<sockaddr_in *>(&peer);
  serverAddress.sin_port = htons(ok.udpPort);
  session = ok.session;
  udpPort = ok.udpPort;
  closeSocket(tcp);
  tcp = INVALID_SOCKET;
  return true;
}

void LinkClient::run() {
  while (running_.load()) {
    bool connectedNotified = false;
    SOCKET tcp = INVALID_SOCKET;
    uint32_t session = 0;
    uint16_t udpPort = 0;
    sockaddr_in serverAddress{};
    std::string error;
    if (!authenticate(tcp, session, udpPort, serverAddress, error)) {
      logger_.warning("Link authentication failed: " + error);
      for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }

    SOCKET udp = createUdpSocket(0, error);
    if (udp == INVALID_SOCKET) {
      logger_.error("Could not create link UDP socket: " + error);
      std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
      continue;
    }

    session_ = session;
    sequence_ = 0;
    talking_ = false;
    {
      std::lock_guard<std::mutex> lock(uplinkMutex_);
      uplink32k_.clear();
      uplink8k_.clear();
      lastUplink_ = std::chrono::steady_clock::now();
    }
    downsampler_.reset();
    upsampler_.reset();

    AudioPacket registration;
    registration.type = PacketType::Register;
    registration.session = session_;
    const auto registrationData = encodeAudioPacket(registration);
    sendTo(udp, reinterpret_cast<const sockaddr *>(&serverAddress),
           sizeof(serverAddress), registrationData.data(),
           registrationData.size());

    connected_.store(true);
    sockaddr_storage serverEndpoint{};
    std::memcpy(&serverEndpoint, &serverAddress, sizeof(serverAddress));
    logger_.info("Linked to server " + endpointToString(serverEndpoint));
    if (!connectedNotified && connectionCallback_) {
      connectedNotified = true;
      connectionCallback_(true);
    }

    auto lastHeartbeat = std::chrono::steady_clock::now();
    auto nextSend = std::chrono::steady_clock::now();
    auto lastServerActivity = std::chrono::steady_clock::now();

    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(
              now - lastServerActivity)
              .count() > 20) {
        logger_.warning("Link server stopped responding; reconnecting");
        break;
      }
      if (now >= nextSend) {
        // Send every due 20 ms frame. Windows select timeouts are commonly
        // quantized to ~15.6 ms, so sending only once per loop iteration
        // settles near 32 frames/s. Catch up when more than one frame is due.
        constexpr int kMaxCatchUpFrames = 4;
        auto sendNow = now;
        for (int sent = 0;
             sent < kMaxCatchUpFrames && sendNow >= nextSend; ++sent) {
          sendUplinkFrames(udp, serverAddress);
          nextSend += std::chrono::milliseconds(20);
          sendNow = std::chrono::steady_clock::now();
        }
        // Do not try to replay a long backlog after a stall; the uplink
        // buffer already keeps only the most recent audio.
        if (sendNow - nextSend > std::chrono::milliseconds(200)) {
          nextSend = sendNow;
        }
      }

      if (std::chrono::duration_cast<std::chrono::milliseconds>(
              now - lastHeartbeat)
              .count() >= kHeartbeatMs) {
        AudioPacket heartbeat;
        heartbeat.type = PacketType::Heartbeat;
        heartbeat.session = session_;
        const auto data = encodeAudioPacket(heartbeat);
        sendTo(udp, reinterpret_cast<const sockaddr *>(&serverAddress),
               sizeof(serverAddress), data.data(), data.size());
        lastHeartbeat = now;
      }

      if (std::chrono::duration_cast<std::chrono::seconds>(now - lastDiag_)
              .count() >= 5) {
        logger_.debug(
            "Link uplink: " +
            std::to_string(diagSamplesIn_.exchange(0)) + " samples in, " +
            std::to_string(diagFramesOut_.exchange(0)) + " frames out");
        lastDiag_ = now;
      }

      fd_set readSet;
      FD_ZERO(&readSet);
      FD_SET(udp, &readSet);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 10000;
      const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
      if (selected > 0) {
        uint8_t buffer[2048];
        sockaddr_storage from{};
        int fromLength = sizeof(from);
        const int received =
            recvfrom(udp, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                     reinterpret_cast<sockaddr *>(&from), &fromLength);
        if (received > 0) {
          AudioPacket packet;
          if (decodeAudioPacket(buffer, static_cast<size_t>(received), packet)) {
            // The server relays packets from other sessions using the
            // sender's session id, so do not require our own id here.
            lastServerActivity = std::chrono::steady_clock::now();
            processDownlink(packet);
          }
        }
      }
    }

    connected_.store(false);
    if (connectedNotified) {
      connectedNotified = false;
      if (connectionCallback_) {
        connectionCallback_(false);
      }
    }
    closeSocket(udp);
    {
      std::lock_guard<std::mutex> lock(uplinkMutex_);
      uplink32k_.clear();
      uplink8k_.clear();
    }
    if (running_.load()) {
      logger_.warning("Link disconnected; reconnecting");
      for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }
  connected_.store(false);
}

void LinkClient::pushUplinkPcm(const std::vector<int16_t> &pcm) {
  if (pcm.empty()) {
    return;
  }
  diagSamplesIn_.fetch_add(pcm.size(), std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(uplinkMutex_);
  const size_t maximum = 32000 * 2;
  if (uplink32k_.size() > maximum) {
    uplink32k_.erase(
        uplink32k_.begin(),
        uplink32k_.begin() + static_cast<std::ptrdiff_t>(
                                 uplink32k_.size() - maximum));
  }
  uplink32k_.insert(uplink32k_.end(), pcm.begin(), pcm.end());
  lastUplink_ = std::chrono::steady_clock::now();
}

void LinkClient::sendUplinkFrames(SOCKET udp,
                                  const sockaddr_in &serverAddress) {
  std::vector<int16_t> pcm32;
  {
    std::lock_guard<std::mutex> lock(uplinkMutex_);
    if (!uplink32k_.empty()) {
      pcm32.swap(uplink32k_);
    }
  }
  if (!pcm32.empty()) {
    downsampler_.process(pcm32.data(), pcm32.size(), uplink8k_);
    lastUplink_ = std::chrono::steady_clock::now();
  }

  while (uplink8k_.size() >= kFrameSamples) {
    AudioPacket packet;
    packet.type = PacketType::Audio;
    packet.session = session_;
    packet.sequence = sequence_++;
    if (!talking_) {
      packet.flags |= FlagStart;
      talking_ = true;
    }
    packet.payload.resize(kFrameSamples);
    for (size_t i = 0; i < kFrameSamples; ++i) {
      packet.payload[i] = linearToUlaw(uplink8k_[i]);
    }
    uplink8k_.erase(uplink8k_.begin(),
                    uplink8k_.begin() +
                        static_cast<std::ptrdiff_t>(kFrameSamples));

    const auto data = encodeAudioPacket(packet);
    if (sendTo(udp, reinterpret_cast<const sockaddr *>(&serverAddress),
               sizeof(serverAddress), data.data(), data.size())) {
      diagFramesOut_.fetch_add(1, std::memory_order_relaxed);
    }
    break; // one 20 ms frame per iteration
  }

  const auto now = std::chrono::steady_clock::now();
  if (talking_ &&
      std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUplink_)
              .count() > kUplinkEndMs) {
    sendEnd(udp, serverAddress);
  }
}

void LinkClient::sendEnd(SOCKET udp, const sockaddr_in &serverAddress) {
  AudioPacket packet;
  packet.type = PacketType::End;
  packet.session = session_;
  packet.sequence = sequence_++;
  const auto data = encodeAudioPacket(packet);
  sendTo(udp, reinterpret_cast<const sockaddr *>(&serverAddress),
         sizeof(serverAddress), data.data(), data.size());
  talking_ = false;
}

void LinkClient::processDownlink(const AudioPacket &packet) {
  if (packet.type == PacketType::Heartbeat ||
      packet.type == PacketType::Register) {
    return;
  }
  if (packet.type == PacketType::End) {
    node_.enqueueNetworkAudio({}, true);
    upsampler_.reset();
    return;
  }
  if (packet.type != PacketType::Audio || packet.payload.empty()) {
    return;
  }
  if ((packet.flags & FlagStart) != 0) {
    upsampler_.reset();
  }

  std::vector<int16_t> pcm8(packet.payload.size());
  for (size_t i = 0; i < packet.payload.size(); ++i) {
    pcm8[i] = ulawToLinear(packet.payload[i]);
  }
  std::vector<int16_t> pcm32;
  upsampler_.process(pcm8.data(), pcm8.size(), pcm32);
  node_.enqueueNetworkAudio(pcm32, false);
}

} // namespace vr76::link
