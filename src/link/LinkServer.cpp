#include "LinkServer.h"

#include "link/LinkCrypto.h"
#include "link/LinkSocket.h"

#include <algorithm>
#include <cstring>

namespace vr76::link {

namespace {

constexpr int kMaxAuthSeconds = 4;
constexpr int kSessionTimeoutSeconds = 30;
constexpr int kTalkerTimeoutMs = 600;

} // namespace

LinkServer::LinkServer(Logger &logger, const Config &config) : logger_(logger) {
  controlPort_ = static_cast<uint16_t>(
      std::clamp(config.getInt("SERVER", "PORT", kDefaultControlPort), 1, 65535));
  audioPort_ = static_cast<uint16_t>(
      std::clamp(config.getInt("SERVER", "AUDIO_PORT", kDefaultAudioPort), 1,
                 65535));
  key_ = config.getString("SERVER", "KEY", "");
  maxClients_ = static_cast<size_t>(
      std::max(1, config.getInt("SERVER", "MAX_CLIENTS", 16)));
}

LinkServer::~LinkServer() { stop(); }

bool LinkServer::start(std::string &error) {
  if (running_.load()) {
    return true;
  }

  listener_ = createTcpListener(controlPort_, error);
  if (listener_ == INVALID_SOCKET) {
    return false;
  }
  udp_ = createUdpSocket(audioPort_, error);
  if (udp_ == INVALID_SOCKET) {
    closeSocket(listener_);
    listener_ = INVALID_SOCKET;
    return false;
  }

  running_.store(true);
  acceptThread_ = std::thread([this]() { acceptLoop(); });
  udpThread_ = std::thread([this]() { udpLoop(); });

  logger_.info("BenshiNode server listening: TCP " +
               std::to_string(controlPort_) + ", UDP " +
               std::to_string(audioPort_));
  if (key_.empty()) {
    logger_.warning("Server key is empty; anyone can connect");
  }
  return true;
}

void LinkServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }

  closeSocket(listener_);
  closeSocket(udp_);
  listener_ = INVALID_SOCKET;
  udp_ = INVALID_SOCKET;

  if (acceptThread_.joinable()) {
    acceptThread_.join();
  }
  if (udpThread_.joinable()) {
    udpThread_.join();
  }

  std::lock_guard<std::mutex> lock(clientThreadsMutex_);
  for (auto &thread : clientThreads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  clientThreads_.clear();
}

void LinkServer::acceptLoop() {
  while (running_.load()) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listener_, &readSet);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
    if (selected <= 0) {
      continue;
    }

    sockaddr_storage address{};
    int addressLength = sizeof(address);
    SOCKET client =
        accept(listener_, reinterpret_cast<sockaddr *>(&address), &addressLength);
    if (client == INVALID_SOCKET) {
      continue;
    }
    setNonBlocking(client, false);

    {
      std::lock_guard<std::mutex> lock(sessionsMutex_);
      if (sessions_.size() >= maxClients_) {
        logger_.warning("Rejecting client: maximum clients reached");
        closeSocket(client);
        continue;
      }
    }

    std::lock_guard<std::mutex> lock(clientThreadsMutex_);
    clientThreads_.emplace_back([this, client]() { clientThread(client); });
  }
}

void LinkServer::clientThread(SOCKET client) {
  setReceiveTimeout(client, kMaxAuthSeconds * 1000);

  AuthChallenge challenge;
  if (!randomBytes(challenge.nonce, sizeof(challenge.nonce))) {
    logger_.error("Could not generate authentication challenge");
    closeSocket(client);
    return;
  }
  const auto challengeData = encodeChallenge(challenge);
  if (!sendAll(client, challengeData.data(), challengeData.size())) {
    closeSocket(client);
    return;
  }
  logger_.debug("Sent link authentication challenge");

  uint8_t responseData[52] = {};
  if (!recvAll(client, responseData, sizeof(responseData),
               kMaxAuthSeconds * 1000)) {
    logger_.warning("Link client did not send an authentication response");
    closeSocket(client);
    return;
  }

  AuthResponse response;
  if (!decodeAuthResponse(responseData, sizeof(responseData), response)) {
    logger_.warning("Link client sent an invalid authentication response");
    const auto fail = encodeAuthFail();
    sendAll(client, fail.data(), fail.size());
    closeSocket(client);
    return;
  }
  logger_.debug("Received link authentication response from " +
                response.callsign);

  if (!key_.empty()) {
    uint8_t expected[32] = {};
    if (!hmacSha256(key_, challenge.nonce, sizeof(challenge.nonce), expected) ||
        !constantTimeEqual(expected, response.digest, sizeof(expected))) {
      logger_.warning("Rejected client with invalid key: " +
                      response.callsign);
      const auto fail = encodeAuthFail();
      sendAll(client, fail.data(), fail.size());
      closeSocket(client);
      return;
    }
  }

  const auto now = std::chrono::steady_clock::now();
  Session session;
  session.callsign = response.callsign;
  session.lastSeen = now;
  {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    do {
      session.id = nextSession_++;
    } while (session.id == 0 || sessions_.count(session.id) != 0);
    sessions_[session.id] = session;
  }

  AuthOk ok;
  ok.session = session.id;
  ok.udpPort = audioPort_;
  const auto okData = encodeAuthOk(ok);
  sendAll(client, okData.data(), okData.size());
  closeSocket(client);

  logger_.info("Link client authenticated: " + response.callsign + " (session " +
               std::to_string(session.id) + ")");
}

void LinkServer::udpLoop() {
  uint8_t buffer[2048];
  auto lastCleanup = std::chrono::steady_clock::now();

  while (running_.load()) {
    sockaddr_storage address{};
    int addressLength = sizeof(address);
    const int received =
        recvfrom(udp_, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                 reinterpret_cast<sockaddr *>(&address), &addressLength);
    if (received <= 0) {
      const int code = WSAGetLastError();
      if (code == WSAEWOULDBLOCK || code == WSAETIMEDOUT) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      continue;
    }

    AudioPacket packet;
    if (!decodeAudioPacket(buffer, static_cast<size_t>(received), packet)) {
      continue;
    }

    const auto now = std::chrono::steady_clock::now();
    bool knownSession = false;
    {
      std::lock_guard<std::mutex> lock(sessionsMutex_);
      auto it = sessions_.find(packet.session);
      if (it != sessions_.end()) {
        Session &session = it->second;
        session.address = address;
        session.addressLength = addressLength;
        session.hasAddress = true;
        session.lastSeen = now;
        knownSession = true;
      }
    }
    if (!knownSession) {
      continue;
    }

    if (packet.type == PacketType::Heartbeat) {
      AudioPacket reply;
      reply.type = PacketType::Heartbeat;
      const auto data = encodeAudioPacket(reply);
      sendTo(udp_, reinterpret_cast<const sockaddr *>(&address), addressLength,
             data.data(), data.size());
    } else if (packet.type == PacketType::Audio) {
      bool accepted = false;
      {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (activeTalker_ == 0 || activeTalker_ == packet.session ||
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastTalkerAudio_)
                    .count() > kTalkerTimeoutMs) {
          if (activeTalker_ != packet.session) {
            logger_.info("Talker: " + sessions_[packet.session].callsign);
          }
          activeTalker_ = packet.session;
          lastTalkerAudio_ = now;
          accepted = true;
        }
      }
      if (accepted) {
        relayAudio(packet, packet.session);
      }
    } else if (packet.type == PacketType::End) {
      bool relay = false;
      {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        if (activeTalker_ == packet.session) {
          activeTalker_ = 0;
          relay = true;
        }
      }
      if (relay) {
        relayAudio(packet, packet.session);
      }
    }

    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCleanup)
            .count() >= 5) {
      lastCleanup = now;
      std::lock_guard<std::mutex> lock(sessionsMutex_);
      cleanupSessions(now);
    }
  }
}

void LinkServer::cleanupSessions(std::chrono::steady_clock::time_point now) {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastSeen)
            .count() > kSessionTimeoutSeconds) {
      logger_.info("Link client expired: " + it->second.callsign);
      if (activeTalker_ == it->first) {
        activeTalker_ = 0;
      }
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

void LinkServer::relayAudio(const AudioPacket &packet, uint32_t fromSession) {
  std::vector<std::pair<sockaddr_storage, int>> recipients;
  {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (const auto &[id, session] : sessions_) {
      if (id != fromSession && session.hasAddress) {
        recipients.emplace_back(session.address, session.addressLength);
      }
    }
  }
  if (recipients.empty()) {
    return;
  }

  const auto data = encodeAudioPacket(packet);
  for (const auto &[address, length] : recipients) {
    sendTo(udp_, reinterpret_cast<const sockaddr *>(&address), length,
           data.data(), data.size());
  }
}

} // namespace vr76::link
