#pragma once

#include "Logger.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

namespace vr76 {

struct PairedDevice {
  std::string name;
  std::string address;
  bool radioCandidate = false;
};

class RfcommChannel {
public:
  using DataCallback = std::function<void(std::vector<uint8_t>)>;
  using DisconnectCallback = std::function<void()>;

  explicit RfcommChannel(Logger &logger);
  ~RfcommChannel();

  RfcommChannel(const RfcommChannel &) = delete;
  RfcommChannel &operator=(const RfcommChannel &) = delete;

  bool connect(const std::string &macAddress,
               const std::vector<winrt::guid> &serviceUuids,
               bool allowFallback, std::string &error);
  void close();
  bool write(const uint8_t *data, size_t size);

  void setDataCallback(DataCallback callback) { dataCallback_ = std::move(callback); }
  void setDisconnectCallback(DisconnectCallback callback) {
    disconnectCallback_ = std::move(callback);
  }

  bool isConnected() const { return connected_.load(); }
  const std::string &serviceUuid() const { return serviceUuid_; }

  static std::vector<PairedDevice> listPairedDevices(Logger &logger);
  static uint64_t parseMac(const std::string &mac);
  static std::string formatMac(uint64_t address);

private:
  void readLoop();

  Logger &logger_;
  winrt::Windows::Networking::Sockets::StreamSocket socket_{nullptr};
  winrt::Windows::Storage::Streams::DataReader reader_{nullptr};
  winrt::Windows::Storage::Streams::DataWriter writer_{nullptr};
  std::mutex writeMutex_;
  std::thread readThread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  DataCallback dataCallback_;
  DisconnectCallback disconnectCallback_;
  std::string serviceUuid_;
};

} // namespace vr76
