#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vr76::iax2 {

constexpr uint16_t kDefaultPort = 4569;
constexpr uint8_t kProtocolVersion = 2;

enum FrameType : uint8_t {
  FrameDtmf = 0x01,
  FrameVoice = 0x02,
  FrameVideo = 0x03,
  FrameControl = 0x04,
  FrameNull = 0x05,
  FrameIax = 0x06,
  FrameText = 0x07,
  FrameImage = 0x08,
  FrameHtml = 0x09,
  FrameComfortNoise = 0x0A,
};

enum IaxSubclass : uint8_t {
  IaxNew = 0x01,
  IaxPing = 0x02,
  IaxPong = 0x03,
  IaxAck = 0x04,
  IaxHangup = 0x05,
  IaxReject = 0x06,
  IaxAccept = 0x07,
  IaxAuthReq = 0x08,
  IaxAuthRep = 0x09,
  IaxInval = 0x0A,
  IaxLagRq = 0x0B,
  IaxLagRp = 0x0C,
  IaxRegReq = 0x0D,
  IaxRegAuth = 0x0E,
  IaxRegAck = 0x0F,
  IaxRegRej = 0x10,
  IaxRegRel = 0x11,
  IaxVnak = 0x12,
  IaxPoke = 0x1E,
  IaxUnsupport = 0x21,
};

enum ControlSubclass : uint8_t {
  ControlHangup = 0x01,
  ControlRinging = 0x03,
  ControlAnswer = 0x04,
  ControlBusy = 0x05,
  ControlCongestion = 0x08,
  ControlFlashHook = 0x09,
  ControlOption = 0x0B,
  ControlKeyRadio = 0x0C,
  ControlUnkeyRadio = 0x0D,
  ControlProgress = 0x0E,
  ControlProceeding = 0x0F,
  ControlHold = 0x10,
  ControlUnhold = 0x11,
};

enum IeId : uint8_t {
  IeCalledNumber = 0x01,
  IeCallingNumber = 0x02,
  IeCallingName = 0x04,
  IeCalledContext = 0x05,
  IeUsername = 0x06,
  IePassword = 0x07,
  IeCapability = 0x08,
  IeFormat = 0x09,
  IeLanguage = 0x0A,
  IeVersion = 0x0B,
  IeAuthMethods = 0x0E,
  IeChallenge = 0x0F,
  IeMd5Result = 0x10,
  IeRefresh = 0x13,
  IeCause = 0x16,
  IeCallingPres = 0x26,
  IeCallingTon = 0x27,
  IeCallingTns = 0x28,
  IeCauseCode = 0x2A,
};

enum AuthMethod : uint16_t {
  AuthPlaintext = 0x0001,
  AuthMd5 = 0x0002,
  AuthRsa = 0x0004,
};

enum Format : uint32_t {
  FormatGsm = 0x00000002,
  FormatUlaw = 0x00000004,
  FormatAlaw = 0x00000008,
  FormatSlin = 0x00000040,
};

enum CauseCode : uint8_t {
  CauseNormalClearing = 16,
  CauseUserBusy = 17,
  CauseNoAnswer = 19,
  CauseCallRejected = 21,
};

struct Frame {
  uint16_t source = 0;
  uint16_t destination = 0;
  uint32_t timestamp = 0;
  uint8_t oseq = 0;
  uint8_t iseq = 0;
  uint8_t type = 0;
  uint8_t subclass = 0;
  std::vector<uint8_t> payload;
};

bool isFullFrame(const uint8_t *data, size_t size);
std::vector<uint8_t> encodeFull(const Frame &frame, bool retransmit = false);
bool decodeFull(const uint8_t *data, size_t size, Frame &frame);

std::vector<uint8_t> encodeMini(uint16_t source, uint16_t timestamp,
                                const std::vector<uint8_t> &payload);
bool decodeMini(const uint8_t *data, size_t size, uint16_t &source,
                uint16_t &timestamp, std::vector<uint8_t> &payload);

class IeSet {
public:
  void addRaw(uint8_t id, const std::vector<uint8_t> &data);
  void addString(uint8_t id, const std::string &value);
  void addUint16(uint8_t id, uint16_t value);
  void addUint32(uint8_t id, uint32_t value);
  void addByte(uint8_t id, uint8_t value);

  std::vector<uint8_t> build() const;
  static IeSet parse(const uint8_t *data, size_t size);

  bool has(uint8_t id) const;
  std::string getString(uint8_t id) const;
  uint32_t getUint(uint8_t id) const;
  std::vector<uint8_t> getRaw(uint8_t id) const;

private:
  struct Entry {
    uint8_t id = 0;
    std::vector<uint8_t> data;
  };
  std::vector<Entry> entries_;
};

uint16_t read16be(const uint8_t *data);
uint32_t read32be(const uint8_t *data);
void append16be(std::vector<uint8_t> &out, uint16_t value);
void append32be(std::vector<uint8_t> &out, uint32_t value);

} // namespace vr76::iax2
