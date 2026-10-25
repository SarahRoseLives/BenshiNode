#include "RfcommChannel.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace vr76 {

namespace {

namespace bluetooth = winrt::Windows::Devices::Bluetooth;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace rfcomm = winrt::Windows::Devices::Bluetooth::Rfcomm;
namespace sockets = winrt::Windows::Networking::Sockets;
namespace streams = winrt::Windows::Storage::Streams;

const winrt::guid kBsAocUuid{
    0x39144315, 0x32FA, 0x40DB,
    {0x85, 0xED, 0xFB, 0xFE, 0xBA, 0x2D, 0x86, 0xE6}};

std::string hresultMessage(const winrt::hresult_error &error) {
  std::ostringstream out;
  out << winrt::to_string(error.message()) << " (0x" << std::hex
      << static_cast<uint32_t>(error.code()) << ")";
  return out.str();
}

std::string uuidString(const winrt::guid &uuid) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer),
                "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                static_cast<unsigned long>(uuid.Data1), uuid.Data2, uuid.Data3,
                uuid.Data4[0], uuid.Data4[1], uuid.Data4[2], uuid.Data4[3],
                uuid.Data4[4], uuid.Data4[5], uuid.Data4[6], uuid.Data4[7]);
  return buffer;
}

void ensureApartment() {
  // WinRT writers/readers may be used from the TX and poll worker threads.
  static thread_local bool initialized = false;
  if (!initialized) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    initialized = true;
  }
}

} // namespace

RfcommChannel::RfcommChannel(Logger &logger) : logger_(logger) {}

RfcommChannel::~RfcommChannel() { close(); }

uint64_t RfcommChannel::parseMac(const std::string &mac) {
  std::string hex;
  for (char c : mac) {
    if (c != ':' && c != '-') {
      hex.push_back(c);
    }
  }
  try {
    return std::stoull(hex, nullptr, 16);
  } catch (...) {
    return 0;
  }
}

std::string RfcommChannel::formatMac(uint64_t address) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "%02X:%02X:%02X:%02X:%02X:%02X",
                static_cast<unsigned>((address >> 40) & 0xFF),
                static_cast<unsigned>((address >> 32) & 0xFF),
                static_cast<unsigned>((address >> 24) & 0xFF),
                static_cast<unsigned>((address >> 16) & 0xFF),
                static_cast<unsigned>((address >> 8) & 0xFF),
                static_cast<unsigned>(address & 0xFF));
  return buffer;
}

bool RfcommChannel::connect(const std::string &macAddress,
                            const std::vector<winrt::guid> &serviceUuids,
                            bool allowFallback, std::string &error) {
  close();
  serviceUuid_.clear();

  const uint64_t address = parseMac(macAddress);
  if (address == 0) {
    error = "Invalid Bluetooth MAC address: " + macAddress;
    return false;
  }

  ensureApartment();
  try {
    auto device = bluetooth::BluetoothDevice::FromBluetoothAddressAsync(address).get();
    if (!device) {
      error = "Bluetooth device not found: " + macAddress;
      return false;
    }

    rfcomm::RfcommDeviceService service{nullptr};
    for (const auto &uuid : serviceUuids) {
      try {
        auto result = device.GetRfcommServicesForIdAsync(
                                  rfcomm::RfcommServiceId::FromUuid(uuid),
                                  bluetooth::BluetoothCacheMode::Uncached)
                          .get();
        if (result.Error() == bluetooth::BluetoothError::Success &&
            result.Services().Size() > 0) {
          service = result.Services().GetAt(0);
          serviceUuid_ = uuidString(uuid);
          break;
        }
      } catch (const winrt::hresult_error &e) {
        logger_.debug("RFCOMM service probe " + uuidString(uuid) + " failed: " +
                      hresultMessage(e));
      }
    }

    if (!service && allowFallback) {
      auto result =
          device.GetRfcommServicesAsync(bluetooth::BluetoothCacheMode::Uncached).get();
      if (result.Error() == bluetooth::BluetoothError::Success &&
          result.Services().Size() > 0) {
        service = result.Services().GetAt(0);
        serviceUuid_ = "fallback";
      }
    }

    if (!service) {
      error = "No matching RFCOMM service found. Make sure the radio is paired "
              "as both an audio and a control device.";
      return false;
    }

    logger_.info("Connecting RFCOMM service " + serviceUuid_ + " on " +
                 macAddress);
    socket_ = sockets::StreamSocket();
    socket_
        .ConnectAsync(service.ConnectionHostName(), service.ConnectionServiceName(),
                      sockets::SocketProtectionLevel::
                          BluetoothEncryptionAllowNullAuthentication)
        .get();

    reader_ = streams::DataReader(socket_.InputStream());
    reader_.InputStreamOptions(streams::InputStreamOptions::Partial);
    writer_ = streams::DataWriter(socket_.OutputStream());

    running_.store(true);
    connected_.store(true);
    readThread_ = std::thread([this]() { readLoop(); });
    return true;
  } catch (const winrt::hresult_error &e) {
    error = "Bluetooth connection failed: " + hresultMessage(e);
  } catch (const std::exception &e) {
    error = std::string("Bluetooth connection failed: ") + e.what();
  }

  close();
  return false;
}

void RfcommChannel::readLoop() {
  ensureApartment();
  try {
    while (running_.load()) {
      const uint32_t available = reader_.LoadAsync(4096).get();
      if (available == 0) {
        break;
      }
      std::vector<uint8_t> buffer(available);
      reader_.ReadBytes(
          winrt::array_view<uint8_t>(buffer.data(), buffer.data() + buffer.size()));
      if (dataCallback_ && !buffer.empty()) {
        dataCallback_(std::move(buffer));
      }
    }
  } catch (const winrt::hresult_error &e) {
    if (running_.load()) {
      logger_.warning("RFCOMM read failed: " + hresultMessage(e));
    }
  } catch (...) {
    if (running_.load()) {
      logger_.warning("RFCOMM read failed with an unknown error");
    }
  }

  const bool wasConnected = connected_.exchange(false);
  running_.store(false);
  if (wasConnected) {
    logger_.info("RFCOMM channel disconnected");
    if (disconnectCallback_) {
      disconnectCallback_();
    }
  }
}

bool RfcommChannel::write(const uint8_t *data, size_t size) {
  if (!connected_.load() || data == nullptr || size == 0) {
    return false;
  }
  ensureApartment();
  try {
    std::lock_guard<std::mutex> lock(writeMutex_);
    writer_.WriteBytes(winrt::array_view<const uint8_t>(data, data + size));
    writer_.StoreAsync().get();
    return true;
  } catch (const winrt::hresult_error &e) {
    logger_.warning("RFCOMM write failed: " + hresultMessage(e));
    return false;
  } catch (...) {
    logger_.warning("RFCOMM write failed with an unknown error");
    return false;
  }
}

void RfcommChannel::close() {
  running_.store(false);
  connected_.store(false);

  try {
    if (socket_) {
      socket_.Close();
    }
  } catch (...) {
  }

  if (readThread_.joinable()) {
    readThread_.join();
  }

  // Callbacks are intentionally kept until after the read thread has stopped.
  // They may be invoked by the read loop while close() is waiting for it, and
  // the owning objects outlive this channel.
  reader_ = nullptr;
  writer_ = nullptr;
  socket_ = nullptr;
}

std::vector<PairedDevice> RfcommChannel::listPairedDevices(Logger &logger) {
  std::vector<PairedDevice> result;
  try {
    const auto selector =
        bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true);
    const auto devices =
        enumeration::DeviceInformation::FindAllAsync(selector).get();
    for (const auto &info : devices) {
      try {
        auto device = bluetooth::BluetoothDevice::FromIdAsync(info.Id()).get();
        if (!device) {
          continue;
        }
        PairedDevice entry;
        entry.name = winrt::to_string(device.Name());
        entry.address = formatMac(device.BluetoothAddress());

        try {
          auto services = device
                              .GetRfcommServicesForIdAsync(
                                  rfcomm::RfcommServiceId::FromUuid(kBsAocUuid),
                                  bluetooth::BluetoothCacheMode::Cached)
                              .get();
          entry.radioCandidate =
              services.Error() == bluetooth::BluetoothError::Success &&
              services.Services().Size() > 0;
        } catch (...) {
        }
        result.push_back(std::move(entry));
      } catch (const winrt::hresult_error &e) {
        logger.debug("Could not inspect paired device: " + hresultMessage(e));
      }
    }
  } catch (const winrt::hresult_error &e) {
    logger.error("Could not enumerate paired Bluetooth devices: " +
                 hresultMessage(e));
  }
  return result;
}

std::vector<BluetoothDeviceInfo> RfcommChannel::discoverDevices(std::string &error) {
  std::vector<BluetoothDeviceInfo> result;
  try {
    ensureApartment();

    // DeviceInformation.Pairing() does not reliably report the pairing state of
    // classic Bluetooth devices. Use the Bluetooth pairing-state selector as
    // the source of truth and match entries by their Bluetooth address.
    std::vector<uint64_t> pairedAddresses;
    try {
      const auto pairedSelector =
          bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true);
      const auto pairedDevices =
          enumeration::DeviceInformation::FindAllAsync(pairedSelector).get();
      for (const auto &pairedInfo : pairedDevices) {
        try {
          auto pairedDevice =
              bluetooth::BluetoothDevice::FromIdAsync(pairedInfo.Id()).get();
          if (pairedDevice) {
            pairedAddresses.push_back(pairedDevice.BluetoothAddress());
          }
        } catch (...) {
        }
      }
    } catch (...) {
    }

    const auto selector = bluetooth::BluetoothDevice::GetDeviceSelector();
    const auto devices =
        enumeration::DeviceInformation::FindAllAsync(selector).get();
    for (const auto &info : devices) {
      try {
        auto device = bluetooth::BluetoothDevice::FromIdAsync(info.Id()).get();
        if (!device) {
          continue;
        }
        BluetoothDeviceInfo entry;
        entry.id = winrt::to_string(info.Id());
        entry.name = winrt::to_string(device.Name());
        const uint64_t address = device.BluetoothAddress();
        entry.address = formatMac(address);
        try {
          entry.paired = info.Pairing().IsPaired();
        } catch (...) {
        }
        if (!entry.paired) {
          entry.paired = std::find(pairedAddresses.begin(), pairedAddresses.end(),
                                   address) != pairedAddresses.end();
        }
        std::string lowerName = entry.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) {
                         return static_cast<char>(std::tolower(c));
                       });
        entry.radioCandidate =
            lowerName.find("vr-n") != std::string::npos ||
            lowerName.find("n7600") != std::string::npos ||
            lowerName.find("benshi") != std::string::npos;
        if (!entry.radioCandidate && entry.paired) {
          try {
            auto services =
                device
                    .GetRfcommServicesForIdAsync(
                        rfcomm::RfcommServiceId::FromUuid(kBsAocUuid),
                        bluetooth::BluetoothCacheMode::Cached)
                    .get();
            entry.radioCandidate =
                services.Error() == bluetooth::BluetoothError::Success &&
                services.Services().Size() > 0;
          } catch (...) {
          }
        }
        result.push_back(std::move(entry));
      } catch (...) {
      }
    }
  } catch (const winrt::hresult_error &e) {
    error = "Bluetooth scan failed: " + hresultMessage(e);
  } catch (...) {
    error = "Bluetooth scan failed";
  }
  return result;
}

bool RfcommChannel::pairDevice(const std::string &deviceId, std::string &error) {
  try {
    ensureApartment();
    auto info = enumeration::DeviceInformation::CreateFromIdAsync(
                    winrt::to_hstring(deviceId))
                    .get();
    if (!info) {
      error = "Bluetooth device was not found";
      return false;
    }
    auto pairing = info.Pairing();
    if (pairing.IsPaired()) {
      return true;
    }
    auto result = pairing.Custom()
                      .PairAsync(enumeration::DevicePairingKinds::ConfirmOnly)
                      .get();
    const auto status = result.Status();
    if (status == enumeration::DevicePairingResultStatus::Paired ||
        status == enumeration::DevicePairingResultStatus::AlreadyPaired) {
      return true;
    }
    error = "Pairing was not completed (status " +
            std::to_string(static_cast<int>(status)) + ")";
    return false;
  } catch (const winrt::hresult_error &e) {
    error = "Pairing failed: " + hresultMessage(e);
    return false;
  } catch (...) {
    error = "Pairing failed";
    return false;
  }
}

} // namespace vr76
