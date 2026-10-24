#include "RadioSession.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <thread>

namespace vr76 {

namespace {

const winrt::guid kSppUuid{
    0x00001101, 0x0000, 0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

constexpr int kAudioSampleRate = 32000;
constexpr int kChunkSamples = kAudioSampleRate / 50; // 20 ms

bool parseFrequencyHz(const std::string &text, uint32_t &frequencyHz) {
  if (text.empty()) {
    return false;
  }
  try {
    std::size_t used = 0;
    const double value = std::stod(text, &used);
    if (used == 0 || value <= 0.0) {
      return false;
    }
    // Values below 10000 are interpreted as MHz, larger values as Hz.
    frequencyHz = static_cast<uint32_t>(
        std::llround(value < 10000.0 ? value * 1000000.0 : value));
    return frequencyHz > 0;
  } catch (...) {
    return false;
  }
}

std::string lowerAscii(const std::string &text) {
  std::string value;
  for (char c : text) {
    value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return value;
}

int parseVfo(const std::string &text) {
  std::string value;
  for (char c : text) {
    value.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (value == "A" || value == "1" || value == "VFOA") {
    return 1;
  }
  if (value == "B" || value == "2" || value == "VFOB") {
    return 2;
  }
  return 0;
}

void put32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
  data[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
  data[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

void put16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

} // namespace

RadioSession::RadioSession(Logger &logger, const Config &config)
    : logger_(logger), config_(config), control_(logger), audio_(logger) {
  macAddress_ = config_.getString("RADIO", "MAC");
  statusPollMs_ = std::max(100, config_.getInt("RADIO", "STATUS_POLL_MS", 500));
  vfoKeepAliveMs_ =
      std::max(0, config_.getInt("RADIO", "VFO_KEEPALIVE_MS", 1000));
  txLeadMs_ = std::max(0, config_.getInt("RADIO", "TX_LEAD_MS", 100));
  audio_.setBitpool(std::clamp(config_.getInt("RADIO", "SBC_BITPOOL", 18),
                               16, 53));
  configuredVfo_ = parseVfo(config_.getString("RADIO", "VFO", ""));
  if (config_.has("RADIO", "CHANNEL")) {
    configuredChannel_ =
        std::clamp(config_.getInt("RADIO", "CHANNEL", -1), -1, 255);
  }
  rxToneHz_ = std::max(0.0, config_.getDouble("RADIO", "RX_CTCSS", 0.0));
  txToneHz_ = std::max(0.0, config_.getDouble("RADIO", "TX_CTCSS", 0.0));

  const std::string dualWatchText =
      config_.getString("RADIO", "DUAL_WATCH", "");
  if (!dualWatchText.empty()) {
    int mode = -1;
    if (dualWatchText == "0" || lowerAscii(dualWatchText) == "off" ||
        lowerAscii(dualWatchText) == "false") {
      mode = 0;
    } else {
      mode = parseVfo(dualWatchText);
    }
    if (mode >= 0) {
      dualWatchMode_ = mode;
    } else {
      logger_.warning("Ignoring invalid RADIO/DUAL_WATCH value: " +
                      dualWatchText);
    }
  } else if (configuredVfo_ != 0) {
    // Backwards-compatible: VFO=A/B implicitly requests dual-channel mode.
    dualWatchMode_ = configuredVfo_;
  }

  if (config_.has("RADIO", "APRS")) {
    aprsSharingConfigured_ = true;
    aprsSharingEnabled_ = config_.getBool("RADIO", "APRS", true);
  }

  const std::string frequencyText =
      config_.getString("RADIO", "FREQUENCY", "");
  if (!frequencyText.empty() &&
      !parseFrequencyHz(frequencyText, simplexFrequencyHz_)) {
    logger_.warning("Ignoring invalid RADIO/FREQUENCY value: " + frequencyText);
    simplexFrequencyHz_ = 0;
  }

  control_.setDataCallback([this](std::vector<uint8_t> data) {
    onControlData(std::move(data));
  });
  control_.setDisconnectCallback([this]() { onControlDisconnected(); });

  audio_.setPcmCallback([this](const std::vector<int16_t> &pcm, bool transmit) {
    if (audioPcmCallback_) {
      audioPcmCallback_(pcm, transmit);
    }
  });
  audio_.setEndCallback([this](bool transmit) {
    if (audioEndCallback_) {
      audioEndCallback_(transmit);
    }
  });
}

RadioSession::~RadioSession() { disconnect(); }

bool RadioSession::connect(std::string &error) {
  disconnect();
  stopping_.store(false);
  controlLost_.store(false);

  if (macAddress_.empty()) {
    error = "RADIO/MAC is not configured";
    return false;
  }

  if (!control_.connect(macAddress_, {kSppUuid}, false, error)) {
    return false;
  }

  if (!getDeviceInfo(error)) {
    disconnect();
    return false;
  }
  if (!registerNotifications(error)) {
    disconnect();
    return false;
  }

  {
    std::string statusError;
    RadioStatus status;
    if (!getHtStatus(status, statusError)) {
      logger_.warning("Could not read initial HT status: " + statusError);
    }
  }

  if (!audio_.connect(macAddress_, error)) {
    logger_.error("Audio channel connection failed: " + error);
    disconnect();
    return false;
  }

  if (simplexFrequencyHz_ != 0 || configuredVfo_ != 0 ||
      configuredChannel_ >= 0) {
    std::string tuneError;
    if (!configureRadio(tuneError)) {
      error = "Remote radio configuration failed: " + tuneError;
      logger_.error(error);
      disconnect();
      return false;
    }
  }

  if (configuredChannel_ < 0 && simplexFrequencyHz_ != 0 &&
      vfoKeepAliveMs_ > 0) {
    vfoKeepAlive_.store(true);
    vfoKeepAliveThread_ = std::thread([this]() { vfoKeepAliveLoop(); });
  }

  connected_.store(true);
  pollThread_ = std::thread([this]() { pollLoop(); });
  logger_.info("VR-N76 connected on " + macAddress_);
  return true;
}

void RadioSession::disconnect() {
  stopping_.store(true);
  vfoKeepAlive_.store(false);
  pendingCv_.notify_all();

  if (vfoKeepAliveThread_.joinable()) {
    vfoKeepAliveThread_.join();
  }
  if (pollThread_.joinable()) {
    pendingCv_.notify_all();
    pollThread_.join();
  }

  audio_.disconnect();
  control_.close();
  connected_.store(false);

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pending_) {
      pending_->done = true;
      pending_.reset();
    }
  }
  pendingCv_.notify_all();
}

bool RadioSession::sendCommand(uint16_t group, uint16_t command,
                               const std::vector<uint8_t> &data,
                               bool expectReply, gaia::Frame *reply,
                               std::string &error, int timeoutMs) {
  std::lock_guard<std::mutex> requestLock(requestMutex_);
  if (!control_.isConnected()) {
    error = "Control channel is not connected";
    return false;
  }

  std::shared_ptr<Pending> pending;
  if (expectReply) {
    pending = std::make_shared<Pending>();
    pending->group = group;
    pending->command = command;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      pending_ = pending;
    }
  }

  const auto raw = gaia::buildCommand(group, command, data);
  const auto framed = gaia::encode(raw);
  if (framed.empty() || !control_.write(framed.data(), framed.size())) {
    error = "Failed to write control command";
    if (expectReply) {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      if (pending_ == pending) {
        pending_.reset();
      }
    }
    return false;
  }

  if (!expectReply) {
    return true;
  }

  std::unique_lock<std::mutex> lock(pendingMutex_);
  const bool completed = pendingCv_.wait_for(
      lock, std::chrono::milliseconds(timeoutMs),
      [&]() { return pending->done || stopping_.load(); });

  if (!completed || !pending->done) {
    if (pending_ == pending) {
      pending_.reset();
    }
    error = "Timed out waiting for reply to " +
            std::string(gaia::commandName(command));
    return false;
  }

  if (reply != nullptr) {
    *reply = pending->frame;
  }
  if (pending_ == pending) {
    pending_.reset();
  }
  return true;
}

bool RadioSession::getDeviceInfo(std::string &error) {
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::GetDevInfo), {3},
                   true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty()) {
    error = "Empty GET_DEV_INFO reply";
    return false;
  }
  if (frame.payload[0] != 0) {
    error = "GET_DEV_INFO returned status " +
            std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }

  const auto &p = frame.payload;
  DeviceInfo info;
  info.vendorId = gaia::getByte(p, 1);
  info.productId = gaia::getShort(p, 2);
  info.hardwareVersion = gaia::getByte(p, 4);
  info.softwareVersion = gaia::getShort(p, 5);
  info.channelCount = gaia::getByte(p, 9);
  info.regionCount = (gaia::getByte(p, 10) & 0xF0) >> 4;
  info.supportsVfo = (gaia::getByte(p, 8) & 0x02) != 0;
  info.supportsDmr = (gaia::getByte(p, 8) & 0x01) != 0;

  {
    std::lock_guard<std::mutex> lock(statusMutex_);
    deviceInfo_ = info;
  }

  logger_.info("Radio: product=" + std::to_string(info.productId) +
               " hw=" + std::to_string(info.hardwareVersion) +
               " sw=" + std::to_string(info.softwareVersion) +
               " channels=" + std::to_string(info.channelCount) +
               " regions=" + std::to_string(info.regionCount));
  return true;
}

bool RadioSession::registerNotifications(std::string &error) {
  // HT status, channel changes, settings changes and VFO frequency changes.
  return sendCommand(gaia::kGroupBasic,
                     static_cast<uint16_t>(
                         gaia::Command::RegisterNotification),
                     {1, 5, 6, 14}, false, nullptr, error);
}

bool RadioSession::getHtStatus(RadioStatus &status, std::string &error) {
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::GetHtStatus), {},
                   true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty()) {
    error = "Empty GET_HT_STATUS reply";
    return false;
  }
  if (frame.payload[0] != 0) {
    error = "GET_HT_STATUS returned status " +
            std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }
  status = parseHtStatus(frame.payload, 1);
  {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = status;
  }
  if (statusCallback_) {
    statusCallback_(status);
  }
  return true;
}

bool RadioSession::setPower(bool on, std::string &error) {
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::SetHtOnOff),
                   {static_cast<uint8_t>(on ? 1 : 0)}, true, &frame, error)) {
    return false;
  }
  return frame.payload.empty() || frame.payload[0] == 0;
}

bool RadioSession::setVolume(int level, std::string &error) {
  level = std::clamp(level, 0, 15);
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::SetVolume),
                   {static_cast<uint8_t>(level)}, true, &frame, error)) {
    return false;
  }
  return frame.payload.empty() || frame.payload[0] == 0;
}

bool RadioSession::readSettings(std::vector<uint8_t> &raw, std::string &error) {
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::ReadSettings), {},
                   true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty()) {
    error = "Empty READ_SETTINGS reply";
    return false;
  }
  if (frame.payload[0] != 0) {
    error = "READ_SETTINGS returned status " +
            std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }
  raw.assign(frame.payload.begin() + 1, frame.payload.end());
  return !raw.empty();
}

bool RadioSession::writeSettings(const std::vector<uint8_t> &raw,
                                 std::string &error) {
  if (raw.empty()) {
    error = "Refusing to write empty settings";
    return false;
  }
  // Some firmware does not acknowledge WRITE_SETTINGS. Send it and let the
  // caller re-read the settings to confirm.
  return sendCommand(gaia::kGroupBasic,
                     static_cast<uint16_t>(gaia::Command::WriteSettings), raw,
                     false, nullptr, error);
}

bool RadioSession::readBssSettings(std::vector<uint8_t> &raw,
                                   std::string &error) {
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::ReadBssSettings), {},
                   true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty()) {
    error = "Empty READ_BSS_SETTINGS reply";
    return false;
  }
  if (frame.payload[0] != 0) {
    error = "READ_BSS_SETTINGS returned status " +
            std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }
  raw.assign(frame.payload.begin() + 1, frame.payload.end());
  return !raw.empty();
}

bool RadioSession::setDualWatch(int mode, std::string &error) {
  if (mode < 0 || mode > 3) {
    error = "Invalid dual-watch mode";
    return false;
  }

  std::vector<uint8_t> raw;
  if (!readSettings(raw, error)) {
    return false;
  }
  if (raw.size() < 2) {
    error = "READ_SETTINGS reply is too short";
    return false;
  }

  const int current = (raw[1] & 0x30) >> 4;
  if (current == mode) {
    logger_.info(mode == 0 ? "Radio is already in single-channel mode"
                           : "Radio is already on VFO " +
                                 std::string(mode == 2 ? "B" : "A"));
    return true;
  }

  raw[1] = static_cast<uint8_t>((raw[1] & ~0x30) | ((mode & 0x03) << 4));
  if (!writeSettings(raw, error)) {
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  std::vector<uint8_t> verify;
  if (!readSettings(verify, error)) {
    return false;
  }
  if (verify.size() < 2) {
    error = "READ_SETTINGS verification reply is too short";
    return false;
  }
  if (((verify[1] & 0x30) >> 4) != mode) {
    error = "Radio did not apply the requested channel mode";
    return false;
  }

  if (mode == 0) {
    logger_.info("Radio switched to single-channel mode");
  } else {
    logger_.info("Radio switched to dual-watch, VFO " +
                 std::string(mode == 2 ? "B" : "A") + " selected");
  }
  return true;
}

bool RadioSession::setVfo(int vfo, std::string &error) {
  if (vfo < 1 || vfo > 2) {
    return true;
  }
  return setDualWatch(vfo, error);
}

bool RadioSession::readChannelRaw(int channelId, std::vector<uint8_t> &raw,
                                  std::string &error) {
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::ReadRfCh),
                   {static_cast<uint8_t>(channelId)}, true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty() || frame.payload[0] != 0) {
    error = frame.payload.empty()
                ? "Empty READ_RF_CH reply"
                : "READ_RF_CH returned status " +
                      std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }
  if (frame.payload.size() < 2) {
    error = "READ_RF_CH reply is too short";
    return false;
  }
  raw.assign(frame.payload.begin() + 1, frame.payload.end());
  if (raw.size() < 25) {
    error = "READ_RF_CH channel record is too short";
    return false;
  }
  return true;
}

bool RadioSession::writeChannelRaw(const std::vector<uint8_t> &raw,
                                   std::string &error) {
  if (raw.size() < 25) {
    error = "Channel record must be 25 bytes";
    return false;
  }
  return sendCommand(gaia::kGroupBasic,
                     static_cast<uint16_t>(gaia::Command::WriteRfCh), raw,
                     false, nullptr, error);
}

bool RadioSession::readChannelRecord(int channelId, std::vector<uint8_t> &raw,
                                     std::string &error) {
  return readChannelRaw(channelId, raw, error);
}

bool RadioSession::writeChannelRecord(const std::vector<uint8_t> &raw,
                                      std::string &error) {
  return writeChannelRaw(raw, error);
}

bool RadioSession::setChannel(int channelId, std::string &error) {
  if (channelId < 0) {
    return true;
  }

  std::vector<uint8_t> raw;
  if (!readSettings(raw, error)) {
    return false;
  }
  if (raw.size() < 10) {
    error = "READ_SETTINGS reply is too short";
    return false;
  }

  const int mode = (raw[1] & 0x30) >> 4;
  const int channelA = ((raw[0] & 0xF0) >> 4) + (raw[9] & 0xF0);
  const int channelB = (raw[0] & 0x0F) + ((raw[9] & 0x0F) << 4);
  const bool useB = (mode == 2);
  const int current = useB ? channelB : channelA;
  if (current == channelId) {
    logger_.info("Radio is already on memory channel " +
                 std::to_string(channelId));
    return true;
  }

  if (useB) {
    raw[0] = static_cast<uint8_t>((raw[0] & 0xF0) | (channelId & 0x0F));
    raw[9] = static_cast<uint8_t>((raw[9] & 0xF0) |
                                  ((channelId >> 4) & 0x0F));
  } else {
    raw[0] = static_cast<uint8_t>((raw[0] & 0x0F) |
                                  ((channelId & 0x0F) << 4));
    raw[9] = static_cast<uint8_t>((raw[9] & 0x0F) | (channelId & 0xF0));
  }

  if (!writeSettings(raw, error)) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::vector<uint8_t> verify;
  if (!readSettings(verify, error)) {
    return false;
  }
  if (verify.size() < 10) {
    error = "READ_SETTINGS verification reply is too short";
    return false;
  }
  const int verifyA = ((verify[0] & 0xF0) >> 4) + (verify[9] & 0xF0);
  const int verifyB = (verify[0] & 0x0F) + ((verify[9] & 0x0F) << 4);
  if ((useB ? verifyB : verifyA) != channelId) {
    error = "Radio did not select the requested memory channel";
    return false;
  }

  logger_.info("Selected memory channel " + std::to_string(channelId) +
               (useB ? " on VFO B" : " on VFO A"));
  return true;
}

bool RadioSession::setAprsSharing(bool enabled, std::string &error) {
  std::vector<uint8_t> raw;
  if (!readBssSettings(raw, error)) {
    return false;
  }
  if (raw.size() < 2) {
    error = "READ_BSS_SETTINGS reply is too short";
    return false;
  }

  const bool current = (raw[1] & 0x10) != 0;
  if (current == enabled) {
    logger_.info(std::string("APRS location sharing is already ") +
                 (enabled ? "enabled" : "disabled"));
    return true;
  }

  if (enabled) {
    raw[1] = static_cast<uint8_t>(raw[1] | 0x10);
  } else {
    raw[1] = static_cast<uint8_t>(raw[1] & ~0x10);
  }

  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::WriteBssSettings), raw,
                   false, nullptr, error)) {
    return false;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  std::vector<uint8_t> verify;
  if (!readBssSettings(verify, error)) {
    return false;
  }
  if (verify.size() < 2) {
    error = "READ_BSS_SETTINGS verification reply is too short";
    return false;
  }
  if (((verify[1] & 0x10) != 0) != enabled) {
    error = "Radio did not apply the requested APRS sharing state";
    return false;
  }

  logger_.info(std::string("APRS location sharing ") +
               (enabled ? "enabled" : "disabled"));
  return true;
}

bool RadioSession::dumpConfiguration(std::string &text, std::string &error) {
  std::vector<uint8_t> settings;
  if (!readSettings(settings, error)) {
    return false;
  }
  std::vector<uint8_t> bss;
  if (!readBssSettings(bss, error)) {
    return false;
  }
  if (settings.size() < 10) {
    error = "Settings block is too short";
    return false;
  }

  const int doubleChannel = (settings[1] & 0x30) >> 4;
  const int channelA =
      ((settings[0] & 0xF0) >> 4) + (settings[9] & 0xF0);
  const int channelB =
      (settings[0] & 0x0F) + ((settings[9] & 0x0F) << 4);
  const bool scan = (settings[1] & 0x80) != 0;
  const int squelch = settings[1] & 0x0F;
  const bool shareLocation = bss.size() >= 2 && (bss[1] & 0x10) != 0;
  const int packetFormat = bss.size() >= 2 ? ((bss[1] & 0x04) >> 2) : 0;

  ChannelInfo channelAInfo;
  ChannelInfo channelBInfo;
  std::string channelError;
  const bool haveA = readChannel(channelA, channelAInfo, channelError);
  channelError.clear();
  const bool haveB = readChannel(channelB, channelBInfo, channelError);

  uint32_t vfoRx = 0;
  uint32_t vfoTx = 0;
  uint16_t vfoFlags = 0;
  channelError.clear();
  const bool haveVfo = getVfoFrequency(vfoRx, vfoTx, vfoFlags, channelError);

  std::ostringstream out;
  out << "Settings:\n"
      << "  selected VFO: "
      << (doubleChannel == 0 ? "single channel"
                             : (doubleChannel == 2 ? "dual watch, VFO B"
                                                   : "dual watch, VFO A"))
      << "\n"
      << "  channel A: " << channelA;
  if (haveA) {
    out << " (" << channelAInfo.name << ", "
        << channelAInfo.rxFrequencyHz / 1000000.0 << " MHz, mod "
        << channelAInfo.rxModulation << ")";
  }
  out << "\n"
      << "  channel B: " << channelB;
  if (haveB) {
    out << " (" << channelBInfo.name << ", "
        << channelBInfo.rxFrequencyHz / 1000000.0 << " MHz)";
  }
  out << "\n"
      << "  scan: " << (scan ? "on" : "off") << "\n"
      << "  squelch level: " << squelch << "\n";
  if (haveVfo && vfoRx != 0) {
    out << "  VFO RX: " << vfoRx / 1000000.0 << " MHz";
    if (vfoTx != 0) {
      out << ", TX: " << vfoTx / 1000000.0 << " MHz";
    }
    out << " (flags 0x" << std::hex << vfoFlags << std::dec
        << (((vfoFlags & 0x00FF) != 0) ? ", VFO mode active"
                                      : ", VFO mode inactive")
        << ")\n";
  }
  out << "APRS/BSS:\n"
      << "  location sharing: " << (shareLocation ? "on" : "off") << "\n"
      << "  packet format: " << packetFormat << "\n";

  out << "Channels:\n";
  int channelCount = deviceInfo_.channelCount;
  if (channelCount <= 0) {
    channelCount = 32;
  }
  for (int id = 0; id < channelCount; ++id) {
    ChannelInfo info;
    std::string channelError2;
    if (readChannel(id, info, channelError2)) {
      out << "  " << id << ": " << info.name << " RX "
          << info.rxFrequencyHz / 1000000.0 << " MHz TX "
          << info.txFrequencyHz / 1000000.0 << " MHz mod "
          << info.rxModulation << "\n";
    }
  }

  text = out.str();
  return true;
}

bool RadioSession::getVfoFrequency(uint32_t &rxHz, uint32_t &txHz,
                                   uint16_t &flags, std::string &error) {
  rxHz = 0;
  txHz = 0;
  flags = 0;
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::FreqModeGetStatus), {},
                   true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty()) {
    error = "Empty FREQ_MODE_GET_STATUS reply";
    return false;
  }
  if (frame.payload[0] != 0) {
    error = "FREQ_MODE_GET_STATUS returned status " +
            std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }
  if (frame.payload.size() < 5) {
    error = "FREQ_MODE_GET_STATUS reply is too short";
    return false;
  }
  {
    std::ostringstream hex;
    for (uint8_t b : frame.payload) {
      hex << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<unsigned>(b) << ' ';
    }
    logger_.debug("FREQ_MODE_GET_STATUS payload: " + hex.str());
  }
  rxHz = gaia::getInt(frame.payload, 1) & 0x3FFFFFFFU;
  if (frame.payload.size() >= 9) {
    txHz = gaia::getInt(frame.payload, 5) & 0x3FFFFFFFU;
  }
  if (frame.payload.size() >= 15) {
    flags = gaia::getShort(frame.payload, 13);
  }
  return true;
}

bool RadioSession::readChannel(int channelId, ChannelInfo &channel,
                               std::string &error) {
  if (channelId < 0) {
    return false;
  }
  gaia::Frame frame;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::ReadRfCh),
                   {static_cast<uint8_t>(channelId)}, true, &frame, error)) {
    return false;
  }
  if (frame.payload.empty()) {
    error = "Empty READ_RF_CH reply";
    return false;
  }
  if (frame.payload[0] != 0) {
    error = "READ_RF_CH returned status " +
            std::to_string(static_cast<unsigned>(frame.payload[0]));
    return false;
  }
  const auto &p = frame.payload;
  if (p.size() < 17) {
    error = "READ_RF_CH reply is too short";
    return false;
  }

  channel.channelId = gaia::getByte(p, 1);
  const uint32_t txRaw = gaia::getInt(p, 2);
  const uint32_t rxRaw = gaia::getInt(p, 6);
  channel.txModulation = static_cast<int>((p[2] >> 6) & 0x03);
  channel.rxModulation = static_cast<int>((p[6] >> 6) & 0x03);
  channel.txFrequencyHz = txRaw & 0x3FFFFFFFU;
  channel.rxFrequencyHz = rxRaw & 0x3FFFFFFFU;

  const size_t nameStart = 16;
  const size_t nameLength = std::min<size_t>(10, p.size() - nameStart);
  std::string name;
  for (size_t i = 0; i < nameLength; ++i) {
    const uint8_t c = p[nameStart + i];
    if (c == 0) {
      break;
    }
    name.push_back(static_cast<char>(c));
  }
  while (!name.empty() &&
         std::isspace(static_cast<unsigned char>(name.back()))) {
    name.pop_back();
  }
  channel.name = name;
  return true;
}

bool RadioSession::setSimplexFrequency(uint32_t frequencyHz, double rxToneHz,
                                       double txToneHz, std::string &error) {
  return setSimplexFrequencyWithFlags(frequencyHz, rxToneHz, txToneHz, 0x0003,
                                      error);
}

bool RadioSession::setSimplexFrequencyWithFlags(uint32_t frequencyHz,
                                                double rxToneHz,
                                                double txToneHz,
                                                uint16_t flags,
                                                std::string &error) {
  if (frequencyHz == 0) {
    return true;
  }

  const uint16_t rxTone = static_cast<uint16_t>(
      std::lround(std::max(0.0, rxToneHz) * 100.0));
  const uint16_t txTone = static_cast<uint16_t>(
      std::lround(std::max(0.0, txToneHz) * 100.0));

  std::vector<uint8_t> data(16, 0);
  put32(data, 0, frequencyHz); // RX (FM, modulation bits zero)
  put32(data, 4, frequencyHz); // TX
  put16(data, 8, rxTone);
  put16(data, 10, txTone);
  put16(data, 12, flags);
  put16(data, 14, 0x61A8);

  gaia::Frame reply;
  if (!sendCommand(gaia::kGroupBasic,
                   static_cast<uint16_t>(gaia::Command::FreqModeSetPar), data,
                   true, &reply, error)) {
    return false;
  }
  if (!reply.payload.empty() && reply.payload[0] != 0) {
    error = "FREQ_MODE_SET_PAR returned status " +
            std::to_string(static_cast<unsigned>(reply.payload[0]));
    return false;
  }

  // Keep the low flags byte non-zero. Sending 0x0000 here makes the radio
  // leave frequency (VFO) mode and return to its stored APRS memory channel on
  // some firmware.
  logger_.info("Tuned " + (configuredVfo_ == 2 ? std::string("VFO B") :
                                                 configuredVfo_ == 1
                                                     ? std::string("VFO A")
                                                     : std::string("VFO")) +
               " to " + std::to_string(frequencyHz / 1000000.0) + " MHz");
  return true;
}

bool RadioSession::configureRadio(std::string &error) {
  if (dualWatchMode_ >= 0) {
    std::string modeError;
    if (!setDualWatch(dualWatchMode_, modeError)) {
      logger_.warning("Channel-mode selection failed: " + modeError +
                      " (using the radio's current mode)");
    }
  }

  if (aprsSharingConfigured_) {
    std::string aprsError;
    if (!setAprsSharing(aprsSharingEnabled_, aprsError)) {
      logger_.warning("APRS sharing configuration failed: " + aprsError);
    }
  }

  // Never rewrite a memory channel here. The node only selects an existing
  // channel; channel records are user data.
  if (configuredChannel_ >= 0) {
    std::string channelError;
    if (!setChannel(configuredChannel_, channelError)) {
      logger_.warning("Memory-channel selection failed: " + channelError);
    }
    return true;
  }

  if (simplexFrequencyHz_ != 0 &&
      !setSimplexFrequency(simplexFrequencyHz_, rxToneHz_, txToneHz_, error)) {
    return false;
  }
  return true;
}

bool RadioSession::transmitPcm(const std::vector<int16_t> &pcm,
                               std::string &error) {
  if (!connected_.load()) {
    error = "Radio is not connected";
    return false;
  }
  if (pcm.empty()) {
    error = "No transmit audio";
    return false;
  }
  if (!audio_.beginTx()) {
    error = "Could not start transmit audio";
    return false;
  }

  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  size_t offset = 0;
  while (offset < pcm.size()) {
    const size_t count = std::min<size_t>(kChunkSamples, pcm.size() - offset);
    if (!audio_.sendPcm(pcm.data() + offset, count)) {
      audio_.endTx();
      error = "Transmit audio write failed";
      return false;
    }
    offset += count;

    const auto audioTime =
        std::chrono::milliseconds(offset * 1000 / kAudioSampleRate);
    const auto target =
        start + audioTime - std::chrono::milliseconds(txLeadMs_);
    const auto now = clock::now();
    if (target > now) {
      std::this_thread::sleep_until(target);
    }
  }

  return audio_.endTx();
}

bool RadioSession::forceUnkey(std::string &error) {
  if (!connected_.load()) {
    error = "Radio is not connected";
    return false;
  }

  audio_.sendEndFrame();

  RadioStatus status;
  for (int attempt = 0; attempt < 6; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string statusError;
    if (getHtStatus(status, statusError) && !status.inTx) {
      logger_.info("Radio is unkeyed");
      return true;
    }
  }

  // Some firmware leaves the PA keyed if the audio run is interrupted. Toggle
  // the radio's transmit state through the programmable-function command.
  gaia::Frame reply;
  std::string commandError;
  if (sendCommand(gaia::kGroupBasic,
                  static_cast<uint16_t>(gaia::Command::DoProgFunc), {4}, true,
                  &reply, commandError)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::string statusError;
    if (getHtStatus(status, statusError) && !status.inTx) {
      logger_.info("Radio unkeyed with DO_PROG_FUNC toggleRadioTx");
      return true;
    }
  }

  if (sendCommand(gaia::kGroupBasic,
                  static_cast<uint16_t>(gaia::Command::DoProgFunc), {13}, true,
                  &reply, commandError)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    std::string statusError;
    if (getHtStatus(status, statusError) && !status.inTx) {
      logger_.info("Radio unkeyed with DO_PROG_FUNC mainPtt");
      return true;
    }
  }

  error = "Radio is still reporting TX after unkey attempts";
  return false;
}

bool RadioSession::transmitTestTone(double frequencyHz, int milliseconds,
                                    std::string &error) {
  const double twoPi = 6.28318530717958647692;
  const int totalSamples = kAudioSampleRate * milliseconds / 1000;
  std::vector<int16_t> pcm(static_cast<size_t>(totalSamples));
  for (int i = 0; i < totalSamples; ++i) {
    const double envelope =
        std::min(1.0, std::min(i, totalSamples - i) /
                          (kAudioSampleRate * 0.010));
    pcm[static_cast<size_t>(i)] = static_cast<int16_t>(
        std::lround(8000.0 * envelope *
                    std::sin(twoPi * frequencyHz * static_cast<double>(i) /
                             kAudioSampleRate)));
  }
  return transmitPcm(pcm, error);
}

void RadioSession::onControlData(std::vector<uint8_t> data) {
  controlDecoder_.push(data.data(), data.size());
  gaia::Frame frame;
  while (controlDecoder_.next(frame)) {
    handleFrame(frame);
  }
}

void RadioSession::handleFrame(const gaia::Frame &frame) {
  if (!frame.isResponse &&
      frame.command == static_cast<uint16_t>(gaia::Command::EventNotification)) {
    handleNotification(frame);
    return;
  }

  if (frame.isResponse) {
    std::shared_ptr<Pending> pending;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      if (pending_ && pending_->group == frame.group &&
          pending_->command == frame.command) {
        pending = pending_;
        pending_->frame = frame;
        pending_->done = true;
      }
    }
    if (pending) {
      pendingCv_.notify_all();
    } else {
      logger_.debug("Unsolicited response: " +
                    std::string(gaia::commandName(frame.command)));
    }
  }
}

void RadioSession::handleNotification(const gaia::Frame &frame) {
  if (frame.payload.empty()) {
    return;
  }
  const uint8_t type = frame.payload[0];
  switch (static_cast<gaia::NotificationType>(type)) {
  case gaia::NotificationType::HtStatusChanged:
    handleHtStatus(frame.payload, 1);
    break;
  case gaia::NotificationType::HtChChanged:
  case gaia::NotificationType::HtSettingsChanged:
    logger_.debug(std::string("Radio notification: ") +
                  gaia::notificationName(type));
    break;
  case gaia::NotificationType::FreqModeStatusChanged: {
    if (frame.payload.size() >= 5) {
      const uint32_t rx = gaia::getInt(frame.payload, 1) & 0x3FFFFFFFU;
      const uint32_t tx = gaia::getInt(frame.payload, 5) & 0x3FFFFFFFU;
      const uint16_t flags =
          frame.payload.size() >= 15 ? gaia::getShort(frame.payload, 13) : 0;
      logger_.debug("VFO RX=" + std::to_string(rx) + " TX=" + std::to_string(tx) +
                    " flags=0x" +
                    [&]() {
                      std::ostringstream out;
                      out << std::hex << flags;
                      return out.str();
                    }());
    }
    break;
  }
  case gaia::NotificationType::PositionChange:
    break;
  default:
    logger_.debug(std::string("Radio notification: ") +
                  gaia::notificationName(type));
    break;
  }
}

void RadioSession::handleHtStatus(const std::vector<uint8_t> &payload,
                                  size_t offset) {
  const RadioStatus status = parseHtStatus(payload, offset);
  {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = status;
  }
  if (statusCallback_) {
    statusCallback_(status);
  }
}

RadioStatus RadioSession::parseHtStatus(const std::vector<uint8_t> &payload,
                                        size_t offset) {
  RadioStatus status;
  if (offset + 3 >= payload.size()) {
    return status;
  }
  const uint8_t flags = payload[offset];
  const uint8_t flags2 = payload[offset + 1];
  const uint8_t flags3 = payload[offset + 2];
  const uint8_t flags4 = payload[offset + 3];

  status.powerOn = (flags & 0x80) != 0;
  status.inTx = (flags & 0x40) != 0;
  status.squelchOpen = (flags & 0x20) != 0;
  status.inRx = (flags & 0x10) != 0;
  status.doubleChannel = (flags & 0x0C) >> 2;
  status.scanning = (flags & 0x02) != 0;
  status.fmRadio = (flags & 0x01) != 0;

  status.gpsLocked = (flags2 & 0x08) != 0;
  status.hfpConnected = (flags2 & 0x04) != 0;
  status.aocConnected = (flags2 & 0x02) != 0;

  status.rssi = (flags3 & 0xF0) >> 4;
  status.region = ((flags3 & 0x0F) << 2) | ((flags4 & 0xC0) >> 6);
  const int lower = (flags2 & 0xF0) >> 4;
  const int upper = (flags4 & 0x3C) >> 2;
  status.channelId = (upper << 4) | lower;
  return status;
}

void RadioSession::onControlDisconnected() {
  if (stopping_.load()) {
    return;
  }
  logger_.warning("Radio control connection lost");
  connected_.store(false);
  controlLost_.store(true);

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pending_) {
      pending_->done = true;
    }
  }
  pendingCv_.notify_all();

  if (disconnectCallback_) {
    disconnectCallback_();
  }
}

void RadioSession::vfoKeepAliveLoop() {
  // FREQ_MODE_SET_PAR is not permanent on all firmware: without a refresh the
  // radio can fall back to its stored APRS memory channel. Re-assert the
  // frequency/VFO mode periodically while the node is connected.
  while (!stopping_.load() && vfoKeepAlive_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(vfoKeepAliveMs_));
    if (stopping_.load() || !vfoKeepAlive_.load() || !connected_.load()) {
      break;
    }

    const uint16_t rxTone = static_cast<uint16_t>(
        std::lround(std::max(0.0, rxToneHz_) * 100.0));
    const uint16_t txTone = static_cast<uint16_t>(
        std::lround(std::max(0.0, txToneHz_) * 100.0));
    std::vector<uint8_t> data(16, 0);
    put32(data, 0, simplexFrequencyHz_);
    put32(data, 4, simplexFrequencyHz_);
    put16(data, 8, rxTone);
    put16(data, 10, txTone);
    put16(data, 12, 0x0003);
    put16(data, 14, 0x61A8);

    std::string error;
    gaia::Frame reply;
    if (!sendCommand(gaia::kGroupBasic,
                     static_cast<uint16_t>(gaia::Command::FreqModeSetPar), data,
                     true, &reply, error, 1000)) {
      logger_.debug("VFO keepalive failed: " + error);
    } else if (!reply.payload.empty() && reply.payload[0] != 0) {
      logger_.debug("VFO keepalive returned status " +
                    std::to_string(static_cast<unsigned>(reply.payload[0])));
    }
  }
}

void RadioSession::pollLoop() {
  int failures = 0;
  while (!stopping_.load() && connected_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(statusPollMs_));
    if (stopping_.load() || !connected_.load()) {
      break;
    }
    std::string error;
    RadioStatus status;
    if (getHtStatus(status, error)) {
      failures = 0;
    } else {
      ++failures;
      logger_.debug("Status poll failed: " + error);
      if (failures >= 3) {
        logger_.warning("Too many status poll failures; dropping connection");
        connected_.store(false);
        onControlDisconnected();
        break;
      }
    }
  }
}

} // namespace vr76
