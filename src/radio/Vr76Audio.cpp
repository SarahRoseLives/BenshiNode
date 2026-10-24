#include "Vr76Audio.h"

#include <algorithm>
#include <thread>

namespace vr76 {

namespace {

const winrt::guid kBsAocUuid{
    0x39144315, 0x32FA, 0x40DB,
    {0x85, 0xED, 0xFB, 0xFE, 0xBA, 0x2D, 0x86, 0xE6}};
const winrt::guid kGenericAudioUuid{
    0x00001203, 0x0000, 0x1000,
    {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

} // namespace

Vr76Audio::Vr76Audio(Logger &logger) : logger_(logger), channel_(logger) {
  channel_.setDataCallback([this](std::vector<uint8_t> data) { onData(std::move(data)); });
}

Vr76Audio::~Vr76Audio() { disconnect(); }

bool Vr76Audio::connect(const std::string &macAddress, std::string &error) {
  channel_.setDataCallback([this](std::vector<uint8_t> data) { onData(std::move(data)); });
  return channel_.connect(macAddress, {kBsAocUuid, kGenericAudioUuid}, true, error);
}

void Vr76Audio::disconnect() {
  txActive_.store(false);
  {
    std::lock_guard<std::mutex> lock(txMutex_);
    txRemainder_.clear();
  }
  channel_.close();
  parser_.clear();
}

bool Vr76Audio::beginTx() {
  if (!channel_.isConnected()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(txMutex_);
  txRemainder_.clear();
  txActive_.store(true);
  return true;
}

bool Vr76Audio::sendPcm(const int16_t *samples, size_t count) {
  if (samples == nullptr || count == 0 || !txActive_.load()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(txMutex_);
  txRemainder_.insert(txRemainder_.end(), samples, samples + count);

  while (txRemainder_.size() >= static_cast<size_t>(SbcCodec::kSamplesPerFrame)) {
    std::vector<uint8_t> encoded;
    if (!codec_.encode(txRemainder_.data(), SbcCodec::kSamplesPerFrame, encoded)) {
      logger_.warning("SBC encode failed");
      return false;
    }
    const auto frame = frameAudio(static_cast<uint8_t>(AudioCommand::TransmitAudio),
                                  encoded);
    if (!channel_.write(frame.data(), frame.size())) {
      logger_.warning("Failed to write transmit audio");
      return false;
    }
    txRemainder_.erase(
        txRemainder_.begin(),
        txRemainder_.begin() + SbcCodec::kSamplesPerFrame);
  }
  return true;
}

bool Vr76Audio::endTx() {
  if (!txActive_.exchange(false)) {
    return true;
  }

  std::lock_guard<std::mutex> lock(txMutex_);
  if (!txRemainder_.empty()) {
    std::vector<int16_t> padded(SbcCodec::kSamplesPerFrame, 0);
    std::copy(txRemainder_.begin(), txRemainder_.end(), padded.begin());
    std::vector<uint8_t> encoded;
    if (codec_.encode(padded.data(), padded.size(), encoded)) {
      const auto frame =
          frameAudio(static_cast<uint8_t>(AudioCommand::TransmitAudio), encoded);
      channel_.write(frame.data(), frame.size());
    }
    txRemainder_.clear();
  }

  return sendEndFrame();
}

bool Vr76Audio::sendEndFrame() {
  const auto end = endAudioFrame();
  bool ok = channel_.write(end.data(), end.size());
  // A second end frame shortly afterwards is harmless and recovers firmware
  // that occasionally misses the first one while its playout buffer drains.
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  ok = channel_.write(end.data(), end.size()) && ok;
  if (!ok) {
    logger_.warning("Failed to write audio end frame");
  }
  return ok;
}

void Vr76Audio::onData(std::vector<uint8_t> data) {
  parser_.push(data.data(), data.size());

  std::vector<uint8_t> frame;
  while (parser_.next(frame)) {
    if (frame.empty()) {
      continue;
    }
    const uint8_t command = frame[0];
    switch (static_cast<AudioCommand>(command)) {
    case AudioCommand::ReceivedAudioAlt:
    case AudioCommand::TransmitAudio: {
      std::vector<int16_t> pcm;
      if (codec_.decode(frame.data() + 1, frame.size() - 1, pcm) && !pcm.empty()) {
        if (pcmCallback_) {
          pcmCallback_(pcm, false);
        }
      }
      break;
    }
    case AudioCommand::TransmitEcho: {
      std::vector<int16_t> pcm;
      if (codec_.decode(frame.data() + 1, frame.size() - 1, pcm) && !pcm.empty()) {
        if (pcmCallback_) {
          pcmCallback_(pcm, true);
        }
      }
      break;
    }
    case AudioCommand::AudioEnd:
      if (endCallback_) {
        endCallback_(false);
      }
      break;
    case AudioCommand::AudioAck:
      break;
    default:
      logger_.debug("Unknown audio frame command 0x" +
                    std::to_string(static_cast<unsigned>(command)));
      break;
    }
  }
}

} // namespace vr76
