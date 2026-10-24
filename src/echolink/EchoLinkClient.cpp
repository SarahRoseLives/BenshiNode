#include "EchoLinkClient.h"

#include "link/LinkSocket.h"
#include "node/NodeEngine.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace vr76::echolink {

namespace {

constexpr size_t kEchoLinkFrameSamples = 160;
constexpr size_t kEchoLinkFramesPerPacket = 4;
constexpr size_t kEchoLinkBlockSamples = kEchoLinkFrameSamples *
                                        kEchoLinkFramesPerPacket;
constexpr int kReconnectMs = 5000;

std::string upper(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return value;
}

bool parseHostPort(const std::string &text, std::string &host, uint16_t &port) {
  host = text;
  port = 5200;
  const auto colon = text.rfind(':');
  if (colon != std::string::npos && colon + 1 < text.size()) {
    host = text.substr(0, colon);
    try {
      const int value = std::stoi(text.substr(colon + 1));
      if (value <= 0 || value > 65535) {
        return false;
      }
      port = static_cast<uint16_t>(value);
    } catch (...) {
      return false;
    }
  }
  return !host.empty();
}

} // namespace

EchoLinkClient::EchoLinkClient(Logger &logger, const Config &config,
                               NodeEngine &node)
    : logger_(logger), config_(config), node_(node) {
  callsign_ = config.getString("ECHOLINK", "CALLSIGN",
                               config.getString("NODE", "CALLSIGN", ""));
  password_ = config.getString("ECHOLINK", "PASSWORD", "");
  name_ = config.getString("ECHOLINK", "NAME", callsign_);
  info_ = config.getString("ECHOLINK", "INFO", "BenshiNode");
  portBase_ = static_cast<uint16_t>(
      std::clamp(config.getInt("ECHOLINK", "PORT_BASE", 5198), 1, 65000));
  directIp_ = config.getString("ECHOLINK", "IP", "");
  const std::string directory =
      config.getString("ECHOLINK", "DIRECTORY", "servers.echolink.org:5200");
  parseHostPort(directory, directoryHost_, directoryPort_);

  encoder_ = gsm_create();
  decoder_ = gsm_create();
  node_.addRxAudioCallback(
      [this](const std::vector<int16_t> &pcm) { pushUplinkPcm(pcm); });
}

EchoLinkClient::~EchoLinkClient() {
  stop();
  if (encoder_ != nullptr) {
    gsm_destroy(encoder_);
    encoder_ = nullptr;
  }
  if (decoder_ != nullptr) {
    gsm_destroy(decoder_);
    decoder_ = nullptr;
  }
}

bool EchoLinkClient::start(const std::string &target, std::string &error) {
  if (target.empty()) {
    error = "No EchoLink target node specified";
    return false;
  }
  if (running_.load()) {
    stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (encoder_ == nullptr || decoder_ == nullptr) {
    error = "GSM codec initialization failed";
    return false;
  }
  if (callsign_.empty()) {
    error = "ECHOLINK/CALLSIGN is not configured";
    return false;
  }
  if (directIp_.empty() && password_.empty()) {
    error = "ECHOLINK/PASSWORD is required for directory lookup";
    return false;
  }
  target_ = target;

  std::string socketError;
  if (!vr76::link::socketStartup(socketError)) {
    error = socketError;
    return false;
  }

  running_.store(true);
  thread_ = std::thread([this]() { run(); });
  logger_.info("EchoLink target: " + target_);
  return true;
}

void EchoLinkClient::stop() {
  running_.store(false);
  if (thread_.joinable()) {
    thread_.join();
  }
  connected_.store(false);
}

bool EchoLinkClient::directoryLookup(const std::string &target,
                                     std::string &ip, std::string &error) {
  if (!directIp_.empty()) {
    ip = directIp_;
    return true;
  }
  if (password_.empty()) {
    error = "EchoLink password is not configured";
    return false;
  }

  SOCKET tcp = vr76::link::connectTcp(directoryHost_, directoryPort_, 8000,
                                      error);
  if (tcp == INVALID_SOCKET) {
    return false;
  }
  const std::string login =
      makeDirectoryLogin(callsign_, password_, info_, true);
  if (!vr76::link::sendAll(tcp, login.data(), login.size())) {
    error = "Could not send EchoLink directory login";
    vr76::link::closeSocket(tcp);
    return false;
  }
  char response[2] = {};
  const bool loginOk =
      vr76::link::recvAll(tcp, response, sizeof(response), 8000) &&
      std::memcmp(response, "OK", 2) == 0;
  vr76::link::closeSocket(tcp);
  if (!loginOk) {
    error = "EchoLink directory rejected the callsign/password";
    return false;
  }

  tcp = vr76::link::connectTcp(directoryHost_, directoryPort_, 8000, error);
  if (tcp == INVALID_SOCKET) {
    return false;
  }
  const std::string request = makeDirectoryListRequest();
  if (!vr76::link::sendAll(tcp, request.data(), request.size())) {
    error = "Could not request EchoLink station list";
    vr76::link::closeSocket(tcp);
    return false;
  }

  std::string data;
  char buffer[8192];
  vr76::link::setReceiveTimeout(tcp, 15000);
  while (data.find("+++") == std::string::npos) {
    const int received = recv(tcp, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      break;
    }
    data.append(buffer, static_cast<size_t>(received));
    if (data.size() > 4 * 1024 * 1024) {
      break;
    }
  }
  vr76::link::closeSocket(tcp);

  std::vector<Station> stations;
  if (!parseDirectoryList(data, stations)) {
    error = "Could not parse the EchoLink directory response";
    return false;
  }
  Station station;
  if (!findStation(stations, target, station)) {
    error = "EchoLink node not found: " + target;
    return false;
  }
  ip = station.ip;
  remoteCallsign_ = station.callsign;
  target_ = std::to_string(station.id);
  logger_.info("EchoLink resolved " + target + " to " + station.callsign +
               " at " + ip);
  return true;
}

void EchoLinkClient::run() {
  while (running_.load()) {
    std::string ip;
    std::string error;
    if (!directoryLookup(target_, ip, error)) {
      logger_.warning("EchoLink lookup failed: " + error);
      if (error.find("rejected") != std::string::npos) {
        // Do not hammer the directory server with a bad password.
        running_.store(false);
        break;
      }
      for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(portBase_);
    if (inet_pton(AF_INET, ip.c_str(), &remote.sin_addr) != 1) {
      logger_.error("Invalid EchoLink remote IP: " + ip);
      std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
      continue;
    }
    sockaddr_in remoteControl = remote;
    remoteControl.sin_port = htons(portBase_ + 1);

    SOCKET audio = vr76::link::createUdpSocket(portBase_, error);
    if (audio == INVALID_SOCKET) {
      logger_.error("Could not open EchoLink audio UDP port: " + error);
      std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
      continue;
    }
    SOCKET control = vr76::link::createUdpSocket(portBase_ + 1, error);
    if (control == INVALID_SOCKET) {
      logger_.error("Could not open EchoLink control UDP port: " + error);
      vr76::link::closeSocket(audio);
      std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectMs));
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(uplinkMutex_);
      uplink32k_.clear();
      uplink8k_.clear();
      lastUplink_ = std::chrono::steady_clock::now();
    }
    downsampler_.reset();
    upsampler_.reset();
    connected_.store(false);
    uint16_t sequence = 0;
    bool infoSent = false;
    auto lastSdes = std::chrono::steady_clock::now();
    auto lastControl = std::chrono::steady_clock::now();
    auto nextUplink = std::chrono::steady_clock::now();
    const auto started = std::chrono::steady_clock::now();

    sendSdes(control, remoteControl);
    logger_.info("EchoLink connecting to " + ip + " (node " + target_ + ")");

    while (running_.load()) {
      const auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - lastSdes)
              .count() >= 10) {
        sendSdes(control, remoteControl);
        lastSdes = now;
      }
      if (!connected_.load() &&
          std::chrono::duration_cast<std::chrono::seconds>(now - started)
                  .count() > 60) {
        logger_.warning("EchoLink connection timed out");
        break;
      }
      if (connected_.load() &&
          std::chrono::duration_cast<std::chrono::seconds>(now - lastControl)
                  .count() > 50) {
        logger_.warning("EchoLink station stopped responding");
        break;
      }
      if (!infoSent && connected_.load()) {
        sendInfo(audio, remote);
        infoSent = true;
      }
      if (now >= nextUplink) {
        sendUplink(audio, remote, sequence);
        nextUplink = now + std::chrono::milliseconds(80);
      }

      fd_set readSet;
      FD_ZERO(&readSet);
      FD_SET(audio, &readSet);
      FD_SET(control, &readSet);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = 20000;
      const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
      if (selected <= 0) {
        continue;
      }

      if (FD_ISSET(control, &readSet)) {
        uint8_t buffer[2048];
        sockaddr_storage from{};
        int fromLength = sizeof(from);
        const int received =
            recvfrom(control, reinterpret_cast<char *>(buffer), sizeof(buffer),
                     0, reinterpret_cast<sockaddr *>(&from), &fromLength);
        if (received > 0) {
          lastControl = std::chrono::steady_clock::now();
          processControl(buffer, static_cast<size_t>(received));
        }
      }
      if (FD_ISSET(audio, &readSet)) {
        uint8_t buffer[2048];
        sockaddr_storage from{};
        int fromLength = sizeof(from);
        const int received =
            recvfrom(audio, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                     reinterpret_cast<sockaddr *>(&from), &fromLength);
        if (received > 0) {
          const auto *data = reinterpret_cast<const uint8_t *>(buffer);
          if (data[0] == 0x6f) {
            logger_.debug("EchoLink info/chat: " +
                          std::string(reinterpret_cast<const char *>(data + 1),
                                      static_cast<size_t>(received - 1)));
          } else {
            processAudio(data, static_cast<size_t>(received));
          }
        }
      }
    }

    const auto bye = makeByePacket();
    vr76::link::sendTo(control,
                       reinterpret_cast<const sockaddr *>(&remoteControl),
                       sizeof(remoteControl), bye.data(), bye.size());
    vr76::link::closeSocket(control);
    vr76::link::closeSocket(audio);
    connected_.store(false);
    if (running_.load()) {
      logger_.warning("EchoLink disconnected; retrying");
      for (int i = 0; i < kReconnectMs / 100 && running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }
}

void EchoLinkClient::sendSdes(SOCKET control, const sockaddr_in &remote) {
  const auto packet = makeSdesPacket(callsign_, name_, "");
  vr76::link::sendTo(control, reinterpret_cast<const sockaddr *>(&remote),
                     sizeof(remote), packet.data(), packet.size());
}

void EchoLinkClient::sendInfo(SOCKET audio, const sockaddr_in &remote) {
  const std::string packet = makeInfoPacket(info_);
  vr76::link::sendTo(audio, reinterpret_cast<const sockaddr *>(&remote),
                     sizeof(remote), packet.data(), packet.size());
}

void EchoLinkClient::pushUplinkPcm(const std::vector<int16_t> &pcm) {
  if (pcm.empty()) {
    return;
  }
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

void EchoLinkClient::sendUplink(SOCKET audio, const sockaddr_in &remote,
                                uint16_t &sequence) {
  std::vector<int16_t> pcm32;
  {
    std::lock_guard<std::mutex> lock(uplinkMutex_);
    if (!uplink32k_.empty()) {
      pcm32.swap(uplink32k_);
    }
  }
  if (!pcm32.empty()) {
    downsampler_.process(pcm32.data(), pcm32.size(), uplink8k_);
  }
  if (uplink8k_.size() < kEchoLinkBlockSamples) {
    return;
  }

  std::vector<int16_t> block(uplink8k_.begin(),
                            uplink8k_.begin() + kEchoLinkBlockSamples);
  uplink8k_.erase(uplink8k_.begin(),
                  uplink8k_.begin() +
                      static_cast<std::ptrdiff_t>(kEchoLinkBlockSamples));

  uint8_t frames[kEchoLinkFramesPerPacket * 33] = {};
  for (size_t i = 0; i < kEchoLinkFramesPerPacket; ++i) {
    gsm_encode(encoder_, block.data() + i * kEchoLinkFrameSamples,
               frames + i * 33);
  }
  const auto packet =
      makeVoicePacket(sequence++, frames, sizeof(frames));
  vr76::link::sendTo(audio, reinterpret_cast<const sockaddr *>(&remote),
                     sizeof(remote), packet.data(), packet.size());
}

void EchoLinkClient::processControl(const uint8_t *data, size_t size) {
  if (isRtcpBye(data, size)) {
    logger_.info("EchoLink station disconnected");
    connected_.store(false);
    running_.store(false);
    return;
  }
  if (!isRtcpSdes(data, size)) {
    return;
  }
  std::string remoteName;
  if (parseSdesItem(data, size, 2, remoteName)) {
    logger_.info("EchoLink connected: " + remoteName);
  }
  connected_.store(true);
}

void EchoLinkClient::processAudio(const uint8_t *data, size_t size) {
  const uint8_t *frames = nullptr;
  size_t frameBytes = 0;
  if (!parseVoicePacket(data, size, frames, frameBytes)) {
    return;
  }
  if (frameBytes < kEchoLinkFramesPerPacket * 33) {
    return;
  }

  std::vector<int16_t> pcm8;
  pcm8.reserve(kEchoLinkBlockSamples);
  for (size_t i = 0; i < kEchoLinkFramesPerPacket; ++i) {
    int16_t decoded[kEchoLinkFrameSamples] = {};
    if (gsm_decode(decoder_, const_cast<gsm_byte *>(frames + i * 33),
                   decoded) != 0) {
      return;
    }
    pcm8.insert(pcm8.end(), decoded,
                decoded + kEchoLinkFrameSamples);
  }

  std::vector<int16_t> pcm32;
  upsampler_.process(pcm8.data(), pcm8.size(), pcm32);
  node_.enqueueNetworkAudio(pcm32, false);
}

} // namespace vr76::echolink
