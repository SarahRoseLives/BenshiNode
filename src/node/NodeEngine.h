#pragma once

#include "Config.h"
#include "Logger.h"
#include "node/DtmfCommands.h"
#include "node/DtmfDecoder.h"
#include "node/ToneGenerator.h"
#include "radio/RadioSession.h"
#include "tts/PiperTts.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vr76 {

class NodeEngine {
public:
  NodeEngine(Logger &logger, const Config &config, RadioSession &session);
  ~NodeEngine();

  using RxAudioCallback = std::function<void(const std::vector<int16_t> &)>;
  using EchoLinkControl =
      std::function<void(bool connect, const std::string &target)>;
  using DiscordControl = std::function<void(bool connect)>;

  void start();
  void stop();

  void onRadioStatus(const RadioStatus &status);
  void onAudioPcm(const std::vector<int16_t> &pcm, bool transmit);
  void onAudioEnd(bool transmit);
  void setRadioReady(bool ready);

  void addRxAudioCallback(RxAudioCallback callback) {
    rxAudioCallbacks_.push_back(std::move(callback));
  }
  void setRxAudioCallback(RxAudioCallback callback) {
    rxAudioCallbacks_.clear();
    addRxAudioCallback(std::move(callback));
  }
  void setEchoLinkControl(EchoLinkControl callback) {
    echoLinkControl_ = std::move(callback);
  }
  void setDiscordControl(DiscordControl callback) {
    discordControl_ = std::move(callback);
  }
  void enqueueNetworkAudio(const std::vector<int16_t> &pcm, bool end);
  bool speakPrompt(const std::string &key);
  bool linkAudioActive() const { return networkActive_.load(); }

private:
  using Clock = std::chrono::steady_clock;

  struct TxClip {
    std::vector<int16_t> pcm;
    std::string name;
    Clock::time_point notBefore;
    bool allowDrop = true;
  };

  void timerLoop();
  void txLoop();
  void tick();

  void openCarrierLocked(Clock::time_point now);
  void closeCarrierLocked(Clock::time_point now, std::vector<int16_t> &run,
                          bool &hadCommand, NodeCommand &command,
                          std::string &commandArg);
  void muteRxRunTailLocked(int milliseconds);
  void handleRxEnd(std::vector<int16_t> run, bool hadCommand,
                   NodeCommand command, const std::string &commandArg);
  void executeCommand(NodeCommand command, const std::vector<int16_t> &run,
                      const std::string &commandArg);
  void enqueueClip(std::vector<int16_t> clip, const std::string &name,
                   Clock::time_point notBefore, bool allowDrop = true);
  void queueIdentLocked(Clock::time_point now);
  void clearTxQueueLocked();

  void loadClips();
  void loadVoicePrompts();
  std::string voicePromptText(const std::string &key) const;
  std::vector<int16_t> voiceClip(const std::string &key);
  std::vector<int16_t> applyGain(const std::vector<int16_t> &pcm,
                                 double gainDb) const;
  static double rmsDb(const int16_t *samples, size_t count);

  Logger &logger_;
  const Config &config_;
  RadioSession &session_;
  PiperTts piper_;

  std::atomic<bool> running_{false};
  std::atomic<bool> radioReady_{false};
  bool startupPromptPlayed_ = false;
  std::thread timerThread_;
  std::thread txThread_;

  mutable std::mutex mutex_;
  std::condition_variable txCv_;

  RadioStatus status_;
  bool radioRx_ = false;
  bool carrierOpen_ = false;
  bool voxOpen_ = false;
  Clock::time_point lastVoice_;
  Clock::time_point lastActivity_;
  Clock::time_point lastIdent_;
  Clock::time_point lastDigit_;

  std::vector<int16_t> rxRun_;
  size_t rxRunLimit_ = 32000 * 60;
  bool suppressParrot_ = false;
  bool pendingCommandValid_ = false;
  NodeCommand pendingCommand_ = NodeCommand::None;
  std::string pendingCommandArg_;
  bool dtmfMuteActive_ = false;
  Clock::time_point dtmfMuteUntil_{};

  DtmfDecoder dtmf_;
  DtmfCommandParser commandParser_;

  std::deque<TxClip> txQueue_;
  std::deque<int16_t> networkPcm_;
  std::atomic<bool> networkActive_{false};
  bool networkEnd_ = false;
  Clock::time_point lastNetworkAudio_{};
  size_t networkLimitSamples_ = 32000 * 5;
  std::atomic<bool> txActive_{false};
  std::atomic<bool> cancelTx_{false};
  std::vector<RxAudioCallback> rxAudioCallbacks_;
  EchoLinkControl echoLinkControl_;
  DiscordControl discordControl_;

  bool parrotEnabled_ = false;
  bool parrotAuto_ = false;
  bool courtesyTone_ = false;
  bool useRadioSquelch_ = true;
  bool useVoxSquelch_ = true;
  int maxParrotSeconds_ = 60;
  int parrotDelayMs_ = 800;
  int voxHangMs_ = 600;
  int txLeadSilenceMs_ = 100;
  int txTailSilenceMs_ = 200;
  int txTimeoutSec_ = 180;
  int idIntervalSec_ = 600;
  int idMinGapSec_ = 120;
  int voxThresholdDb_ = -38;
  int cwWpm_ = 20;
  int cwToneHz_ = 700;
  double rxGainDb_ = 0.0;
  double txGainDb_ = 0.0;
  std::string callsign_;
  std::string wavDir_;

  std::mutex voiceMutex_;
  std::map<std::string, std::vector<int16_t>> voiceClips_;
  std::vector<int16_t> identClip_;
  std::vector<int16_t> helpClip_;
  std::vector<int16_t> ackClip_;
  std::vector<int16_t> courtesyClip_;
};

} // namespace vr76
