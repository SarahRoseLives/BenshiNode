#pragma once

#include "Config.h"
#include "Logger.h"
#include "bluetooth/RfcommChannel.h"
#include "radio/GaiaProtocol.h"
#include "radio/Vr76Audio.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace vr76 {

struct RadioStatus {
  bool powerOn = false;
  bool inTx = false;
  bool squelchOpen = false;
  bool inRx = false;
  int doubleChannel = 0;
  bool scanning = false;
  bool fmRadio = false;
  bool gpsLocked = false;
  bool hfpConnected = false;
  bool aocConnected = false;
  int rssi = 0;
  int region = 0;
  int channelId = 0;
};

struct DeviceInfo {
  uint8_t vendorId = 0;
  uint16_t productId = 0;
  uint8_t hardwareVersion = 0;
  uint16_t softwareVersion = 0;
  int channelCount = 0;
  int regionCount = 0;
  bool supportsVfo = false;
  bool supportsDmr = false;
};

struct ChannelInfo {
  int channelId = -1;
  std::string name;
  uint32_t rxFrequencyHz = 0;
  uint32_t txFrequencyHz = 0;
  int rxModulation = 0;
  int txModulation = 0;
};

class RadioSession {
public:
  using StatusCallback = std::function<void(const RadioStatus &)>;
  using AudioPcmCallback =
      std::function<void(const std::vector<int16_t> &, bool transmit)>;
  using AudioEndCallback = std::function<void(bool transmit)>;
  using DisconnectCallback = std::function<void()>;

  RadioSession(Logger &logger, const Config &config);
  ~RadioSession();

  bool connect(std::string &error);
  void disconnect();
  bool isConnected() const { return connected_.load(); }

  void setStatusCallback(StatusCallback callback) {
    statusCallback_ = std::move(callback);
  }
  void setAudioPcmCallback(AudioPcmCallback callback) {
    audioPcmCallback_ = std::move(callback);
  }
  void setAudioEndCallback(AudioEndCallback callback) {
    audioEndCallback_ = std::move(callback);
  }
  void setDisconnectCallback(DisconnectCallback callback) {
    disconnectCallback_ = std::move(callback);
  }

  bool getHtStatus(RadioStatus &status, std::string &error);
  bool setPower(bool on, std::string &error);
  bool setVolume(int level, std::string &error);
  bool setVfo(int vfo, std::string &error);
  bool setChannel(int channelId, std::string &error);
  bool setSimplexFrequency(uint32_t frequencyHz, double rxToneHz,
                          double txToneHz, std::string &error);
  bool setSimplexFrequencyWithFlags(uint32_t frequencyHz, double rxToneHz,
                                    double txToneHz, uint16_t flags,
                                    std::string &error);
  bool dumpConfiguration(std::string &text, std::string &error);
  bool getVfoFrequency(uint32_t &rxHz, uint32_t &txHz, uint16_t &flags,
                       std::string &error);
  bool readChannel(int channelId, ChannelInfo &channel, std::string &error);
  bool readChannelRecord(int channelId, std::vector<uint8_t> &raw,
                         std::string &error);
  bool writeChannelRecord(const std::vector<uint8_t> &raw, std::string &error);
  bool transmitPcm(const std::vector<int16_t> &pcm, std::string &error);
  bool forceUnkey(std::string &error);
  bool transmitTestTone(double frequencyHz, int milliseconds, std::string &error);

  const RadioStatus &status() const { return status_; }
  const DeviceInfo &deviceInfo() const { return deviceInfo_; }
  const std::string &macAddress() const { return macAddress_; }
  Vr76Audio &audio() { return audio_; }

private:
  struct Pending {
    uint16_t group = 0;
    uint16_t command = 0;
    bool done = false;
    gaia::Frame frame;
  };

  void onControlData(std::vector<uint8_t> data);
  void handleFrame(const gaia::Frame &frame);
  void handleNotification(const gaia::Frame &frame);
  void handleHtStatus(const std::vector<uint8_t> &payload, size_t offset);
  void onControlDisconnected();
  void pollLoop();
  void vfoKeepAliveLoop();
  void stopThreads();

  bool sendCommand(uint16_t group, uint16_t command,
                   const std::vector<uint8_t> &data, bool expectReply,
                   gaia::Frame *reply, std::string &error, int timeoutMs = 1500);
  bool getDeviceInfo(std::string &error);
  bool registerNotifications(std::string &error);
  bool readSettings(std::vector<uint8_t> &raw, std::string &error);
  bool readBssSettings(std::vector<uint8_t> &raw, std::string &error);
  bool writeSettings(const std::vector<uint8_t> &raw, std::string &error);
  bool readChannelRaw(int channelId, std::vector<uint8_t> &raw,
                      std::string &error);
  bool writeChannelRaw(const std::vector<uint8_t> &raw, std::string &error);
  bool setDualWatch(int mode, std::string &error);
  bool setAprsSharing(bool enabled, std::string &error);
  bool configureRadio(std::string &error);

  static RadioStatus parseHtStatus(const std::vector<uint8_t> &payload,
                                   size_t offset);

  Logger &logger_;
  const Config &config_;
  std::string macAddress_;
  int statusPollMs_ = 500;
  int vfoKeepAliveMs_ = 1000;
  int txLeadMs_ = 1000;
  uint32_t simplexFrequencyHz_ = 0;
  int configuredVfo_ = 0;
  int configuredChannel_ = -1;
  int dualWatchMode_ = -1;
  bool aprsSharingConfigured_ = false;
  bool aprsSharingEnabled_ = true;
  double rxToneHz_ = 0.0;
  double txToneHz_ = 0.0;

  RfcommChannel control_;
  Vr76Audio audio_;
  gaia::Decoder controlDecoder_;

  std::atomic<bool> connected_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> controlLost_{false};
  std::thread pollThread_;
  std::thread vfoKeepAliveThread_;
  std::atomic<bool> vfoKeepAlive_{false};
  std::mutex pollMutex_;

  std::mutex requestMutex_;
  std::mutex pendingMutex_;
  std::condition_variable pendingCv_;
  std::shared_ptr<Pending> pending_;

  std::mutex statusMutex_;
  RadioStatus status_;
  DeviceInfo deviceInfo_;

  StatusCallback statusCallback_;
  AudioPcmCallback audioPcmCallback_;
  AudioEndCallback audioEndCallback_;
  DisconnectCallback disconnectCallback_;
};

} // namespace vr76
