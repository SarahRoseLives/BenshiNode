#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gaia {

constexpr uint16_t kGroupBasic = 2;
constexpr uint16_t kGroupExtended = 10;

enum class Command : uint16_t {
  Unknown = 0,
  GetDevId = 1,
  SetRegTimes = 2,
  GetRegTimes = 3,
  GetDevInfo = 4,
  ReadStatus = 5,
  RegisterNotification = 6,
  CancelNotification = 7,
  GetNotification = 8,
  EventNotification = 9,
  ReadSettings = 10,
  WriteSettings = 11,
  StoreSettings = 12,
  ReadRfCh = 13,
  WriteRfCh = 14,
  GetInScan = 15,
  SetInScan = 16,
  SetRemoteDeviceAddr = 17,
  GetTrustedDevice = 18,
  DelTrustedDevice = 19,
  GetHtStatus = 20,
  SetHtOnOff = 21,
  GetVolume = 22,
  SetVolume = 23,
  RadioGetStatus = 24,
  RadioSetMode = 25,
  RadioSeekUp = 26,
  RadioSeekDown = 27,
  RadioSetFreq = 28,
  ReadAdvancedSettings = 29,
  WriteAdvancedSettings = 30,
  HtSendData = 31,
  SetPosition = 32,
  ReadBssSettings = 33,
  WriteBssSettings = 34,
  FreqModeSetPar = 35,
  FreqModeGetStatus = 36,
  ReadRda1846sAgc = 37,
  WriteRda1846sAgc = 38,
  ReadFreqRange = 39,
  WriteDeEmphCoeffs = 40,
  StopRinging = 41,
  SetTxTimeLimit = 42,
  SetIsDigitalSignal = 43,
  SetHl = 44,
  SetDid = 45,
  SetIba = 46,
  GetIba = 47,
  SetTrustedDeviceName = 48,
  SetVoc = 49,
  GetVoc = 50,
  SetPhoneStatus = 51,
  ReadRfStatus = 52,
  PlayTone = 53,
  GetDid = 54,
  GetPf = 55,
  SetPf = 56,
  RxData = 57,
  WriteRegionCh = 58,
  WriteRegionName = 59,
  SetRegion = 60,
  SetPpId = 61,
  GetPpId = 62,
  ReadAdvancedSettings2 = 63,
  WriteAdvancedSettings2 = 64,
  Unlock = 65,
  DoProgFunc = 66,
  SetMsg = 67,
  GetMsg = 68,
  BleConnParam = 69,
  SetTime = 70,
  SetAprsPath = 71,
  GetAprsPath = 72,
  ReadRegionName = 73,
  SetDevId = 74,
  GetPfActions = 75,
  GetPosition = 76,
  SetSatelliteInfo = 77
};

enum class NotificationType : uint8_t {
  Unknown = 0,
  HtStatusChanged = 1,
  DataRxd = 2,
  NewInquiryData = 3,
  RestoreFactorySettings = 4,
  HtChChanged = 5,
  HtSettingsChanged = 6,
  RingingStopped = 7,
  RadioStatusChanged = 8,
  UserAction = 9,
  SystemEvent = 10,
  BssSettingsChanged = 11,
  DataTxd = 12,
  PositionChange = 13,
  FreqModeStatusChanged = 14
};

struct Frame {
  uint16_t group = 0;
  uint16_t command = 0;
  bool isResponse = false;
  std::vector<uint8_t> payload;
};

std::vector<uint8_t> buildCommand(uint16_t group, uint16_t command,
                                  const std::vector<uint8_t> &data = {});
std::vector<uint8_t> buildCommandByte(uint16_t group, uint16_t command,
                                      uint8_t value);
std::vector<uint8_t> buildCommandInt(uint16_t group, uint16_t command,
                                     uint32_t value);

// Wrap an un-framed command ([group16][cmd16][data...]) in the GAIA
// FF 01 CS LEN envelope used on Bluetooth Classic RFCOMM.
std::vector<uint8_t> encode(const std::vector<uint8_t> &command);

bool decodeCommand(const uint8_t *data, size_t size, Frame &frame);

uint8_t getByte(const std::vector<uint8_t> &data, size_t offset);
uint16_t getShort(const std::vector<uint8_t> &data, size_t offset);
uint32_t getInt(const std::vector<uint8_t> &data, size_t offset);

const char *commandName(uint16_t command);
const char *notificationName(uint8_t type);

class Decoder {
public:
  void push(const uint8_t *data, size_t size);
  void clear() { buffer_.clear(); }
  bool next(Frame &frame);

private:
  std::vector<uint8_t> buffer_;
};

} // namespace gaia
