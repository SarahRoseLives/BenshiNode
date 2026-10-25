#include "NodeEngine.h"

#include "node/WavClip.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <map>

namespace vr76 {

namespace {

constexpr int kSampleRate = 32000;

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string pathJoin(const std::string &dir, const std::string &name) {
  if (dir.empty()) {
    return name;
  }
  return (std::filesystem::path(dir) / name).string();
}

} // namespace

NodeEngine::NodeEngine(Logger &logger, const Config &config,
                       RadioSession &session)
    : logger_(logger), config_(config), session_(session),
      piper_(logger, config) {
  callsign_ = config_.getString("NODE", "CALLSIGN", "");
  wavDir_ = config_.getString("NODE", "WAV_DIR", "clips");

  const std::string parrotMode = lower(config_.getString("NODE", "PARROT_MODE", "dtmf"));
  parrotAuto_ = (parrotMode == "auto");
  parrotEnabled_ = parrotAuto_;

  const std::string squelchMode = lower(config_.getString("NODE", "SQUELCH_MODE", "auto"));
  if (squelchMode == "radio") {
    useRadioSquelch_ = true;
    useVoxSquelch_ = false;
  } else if (squelchMode == "vox") {
    useRadioSquelch_ = false;
    useVoxSquelch_ = true;
  } else {
    useRadioSquelch_ = true;
    useVoxSquelch_ = true;
  }

  maxParrotSeconds_ = std::max(1, config_.getInt("NODE", "PARROT_MAX_SEC", 60));
  parrotDelayMs_ = std::max(0, config_.getInt("NODE", "PARROT_DELAY_MS", 800));
  voxHangMs_ = std::max(0, config_.getInt("NODE", "VOX_HANG_MS", 600));
  txLeadSilenceMs_ = std::max(0, config_.getInt("NODE", "TX_LEAD_SILENCE_MS", 100));
  txTailSilenceMs_ = std::max(0, config_.getInt("NODE", "TX_TAIL_SILENCE_MS", 200));
  txTimeoutSec_ = std::max(1, config_.getInt("NODE", "TX_TIMEOUT_SEC", 180));
  idIntervalSec_ = std::max(10, config_.getInt("NODE", "ID_INTERVAL_SEC", 600));
  idMinGapSec_ = std::max(0, config_.getInt("NODE", "ID_MIN_GAP_SEC", 120));
  voxThresholdDb_ = config_.getInt("NODE", "VOX_THRESHOLD_DB", -38);
  cwWpm_ = std::max(5, config_.getInt("NODE", "CW_WPM", 20));
  cwToneHz_ = std::max(300, config_.getInt("NODE", "CW_TONE_HZ", 700));
  rxGainDb_ = config_.getDouble("NODE", "RX_GAIN_DB", 0.0);
  txGainDb_ = config_.getDouble("NODE", "TX_GAIN_DB", 0.0);
  courtesyTone_ = config_.getBool("NODE", "COURTESY_TONE", false);

  rxRunLimit_ = static_cast<size_t>(kSampleRate) *
                static_cast<size_t>(maxParrotSeconds_);

  std::string prefixText = config_.getString("NODE", "DTMF_PREFIX", "");
  std::string suffixText = config_.getString("NODE", "DTMF_SUFFIX", "#");
  if (suffixText.empty()) {
    suffixText = "#";
  }
  const char prefix = prefixText.empty() ? '\0' : prefixText[0];
  const char suffix = suffixText[0];
  commandParser_.setPrefix(prefix);
  commandParser_.setSuffix(suffix);
  dialPrefix_ = config_.getString("ECHOLINK", "DTMF_PREFIX", "4");
  commandParser_.setDialPrefix(dialPrefix_);

  std::map<std::string, NodeCommand> commands;
  const std::string &commandPrefix = prefixText;
  const std::string &commandSuffix = suffixText;
  const std::string defaults[] = {"0", "1", "2", "3", "4",
                                  "5", "6", "7", "8", "9"};
  const NodeCommand defaultCommands[] = {
      NodeCommand::Help, NodeCommand::ParrotToggle,
      NodeCommand::LinkConnect, NodeCommand::Ident,
      NodeCommand::EchoLinkConnect, NodeCommand::EchoLinkDisconnect,
      NodeCommand::DiscordJoin, NodeCommand::DiscordLeave,
      NodeCommand::DisconnectAll, NodeCommand::Reset};
  for (size_t i = 0; i < std::size(defaults); ++i) {
    const std::string key = commandPrefix + defaults[i] + commandSuffix;
    commands[key] = defaultCommands[i];
  }
  // Allow [DTMF] to override or add commands. The documented form is
  // "1=PARROT"; a full "1#=PARROT" key is also accepted.
  for (size_t i = 0; i < std::size(defaults); ++i) {
    std::string value =
        config_.getString("DTMF", commandPrefix + defaults[i], "");
    if (value.empty()) {
      value = config_.getString(
          "DTMF", commandPrefix + defaults[i] + commandSuffix, "");
    }
    if (!value.empty()) {
      NodeCommand command;
      if (parseNodeCommand(value, command)) {
        commands[commandPrefix + defaults[i] + commandSuffix] = command;
      }
    }
  }
  commandParser_.setCommands(commands);
  commandCodes_ = commands;
}

NodeEngine::~NodeEngine() { stop(); }

void NodeEngine::start() {
  if (running_.exchange(true)) {
    return;
  }
  loadClips();
  lastVoice_ = Clock::now();
  lastActivity_ = Clock::now();
  lastDigit_ = Clock::now();
  lastIdent_ = Clock::now();
  timerThread_ = std::thread([this]() { timerLoop(); });
  txThread_ = std::thread([this]() { txLoop(); });

  logger_.info("Node callsign: " + (callsign_.empty() ? "(none)" : callsign_));
  logger_.info(std::string("Parrot mode: ") + (parrotAuto_ ? "auto" : "DTMF") +
               ", VOX threshold " + std::to_string(voxThresholdDb_) + " dB");
}

void NodeEngine::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  cancelTx_.store(true);
  txCv_.notify_all();
  if (timerThread_.joinable()) {
    timerThread_.join();
  }
  if (txThread_.joinable()) {
    txThread_.join();
  }
  session_.audio().endTx();
  txActive_.store(false);
}

void NodeEngine::loadClips() {
  std::string error;
  const std::string identWav = pathJoin(wavDir_, "ident.wav");
  const std::string callsignWav =
      callsign_.empty() ? std::string() : pathJoin(wavDir_, callsign_ + ".wav");

  if (std::filesystem::exists(identWav) &&
      WavClip::load(identWav, identClip_, error)) {
    logger_.info("Loaded ident clip: " + identWav);
  } else if (!callsignWav.empty() && std::filesystem::exists(callsignWav) &&
             WavClip::load(callsignWav, identClip_, error)) {
    logger_.info("Loaded ident clip: " + callsignWav);
  }

  const std::string helpWav = pathJoin(wavDir_, "help.wav");
  if (std::filesystem::exists(helpWav) &&
      WavClip::load(helpWav, helpClip_, error)) {
    logger_.info("Loaded help clip: " + helpWav);
  }

  loadVoicePrompts();

  if (identClip_.empty() && !callsign_.empty()) {
    identClip_ = ToneGenerator::cw(callsign_, cwWpm_, cwToneHz_, -12.0);
  }
  if (helpClip_.empty()) {
    helpClip_ = ToneGenerator::cw("HELP", cwWpm_, cwToneHz_, -12.0);
  }

  ackClip_ = ToneGenerator::beep(1000.0, 100, -16.0);
  courtesyClip_ = ToneGenerator::beep(900.0, 120, -18.0);
}

void NodeEngine::loadVoicePrompts() {
  if (!piper_.enabled()) {
    return;
  }

  std::string error;
  const bool spellCallsign =
      config_.getBool("PIPER", "SPELL_CALLSIGN", true);
  if (spellCallsign && identClip_.empty() && !callsign_.empty() &&
      piper_.spellCallsign(callsign_, identClip_, error)) {
    logger_.info("Generated spoken callsign: " + callsign_);
  } else if (identClip_.empty() && !error.empty()) {
    logger_.warning("Spoken callsign generation failed: " + error);
  }

  std::vector<PiperTts::Request> requests;
  if (helpClip_.empty()) {
    requests.push_back({"help", voicePromptText("help")});
  }
  for (const std::string &key :
       {"startup", "parrot_on", "parrot_off", "reset", "echo_connecting",
        "echo_disconnected", "discord_joining", "discord_left",
        "link_connected", "link_disconnected", "all_disconnected"}) {
    requests.push_back({key, voicePromptText(key)});
  }

  PiperTts::Prompts prompts;
  error.clear();
  if (!piper_.synthesizeBatch(requests, prompts, error)) {
    logger_.warning("Voice prompt generation failed: " + error);
    return;
  }

  std::lock_guard<std::mutex> lock(voiceMutex_);
  for (auto &[key, pcm] : prompts) {
    voiceClips_[key] = std::move(pcm);
  }
  if (helpClip_.empty()) {
    const auto help = voiceClips_.find("help");
    if (help != voiceClips_.end()) {
      helpClip_ = help->second;
    }
  }
  logger_.info("Spoken voice prompts ready");
}

std::string NodeEngine::voicePromptText(const std::string &key) const {
  const std::string call = callsign_.empty() ? "BenshiNode" : callsign_;
  if (key == "help") {
    return config_.getString(
        "PIPER", "HELP_TEXT",
        "BenshiNode help. This is " + call +
            ". Press 0 for help. Press 1 to toggle parrot. Press 2 to "
            "connect the network link. Press 3 to send identification. "
            "Press 4 to connect EchoLink. Press 4 plus a node number to "
            "dial EchoLink. Press 5 to disconnect EchoLink. Press 6 to "
            "join Discord. Press 7 to leave Discord. Press 8 to "
            "disconnect all networks. Press 9 to reset the node.");
  }
  if (key == "startup") {
    return config_.getString("PIPER", "STARTUP_TEXT",
                             "BenshiNode ready. This is " + call + ".");
  }
  if (key == "parrot_on") {
    return "Parrot on.";
  }
  if (key == "parrot_off") {
    return "Parrot off.";
  }
  if (key == "reset") {
    return "Node reset.";
  }
  if (key == "echo_connecting") {
    return "Connecting EchoLink.";
  }
  if (key == "echo_disconnected") {
    return "EchoLink disconnected.";
  }
  if (key == "discord_joining") {
    return "Joining Discord.";
  }
  if (key == "discord_left") {
    return "Discord disconnected.";
  }
  if (key == "link_connected") {
    return config_.getString("PIPER", "LINK_CONNECTED_TEXT",
                             "Connected to Network.");
  }
  if (key == "link_disconnected") {
    return config_.getString("PIPER", "LINK_DISCONNECTED_TEXT",
                             "Disconnected from Network.");
  }
  if (key == "all_disconnected") {
    return config_.getString("PIPER", "ALL_DISCONNECTED_TEXT",
                             "Disconnected from all links.");
  }
  return {};
}

std::vector<int16_t> NodeEngine::voiceClip(const std::string &key) {
  {
    std::lock_guard<std::mutex> lock(voiceMutex_);
    const auto it = voiceClips_.find(key);
    if (it != voiceClips_.end()) {
      return it->second;
    }
  }
  if (!piper_.enabled()) {
    return {};
  }
  const std::string text = voicePromptText(key);
  if (text.empty()) {
    return {};
  }
  std::vector<int16_t> pcm;
  std::string error;
  if (!piper_.synthesize(key, text, pcm, error)) {
    logger_.warning("Voice prompt '" + key + "' failed: " + error);
    return {};
  }
  {
    std::lock_guard<std::mutex> lock(voiceMutex_);
    voiceClips_[key] = pcm;
  }
  return pcm;
}

bool NodeEngine::speakPrompt(const std::string &key) {
  std::vector<int16_t> pcm = voiceClip(key);
  if (pcm.empty()) {
    return false;
  }
  enqueueClip(std::move(pcm), key, Clock::now());
  return true;
}

void NodeEngine::playLinkStatePrompt(bool connected) {
  const std::string key = connected ? "link_connected" : "link_disconnected";
  if (!speakPrompt(key)) {
    enqueueClip(ackClip_, key, Clock::now());
  }
}

void NodeEngine::setRadioReady(bool ready) {
  radioReady_.store(ready);
  if (ready && !startupPromptPlayed_ &&
      config_.getBool("PIPER", "STARTUP_PROMPT", false)) {
    startupPromptPlayed_ = true;
    speakPrompt("startup");
  }
}

uint64_t NodeEngine::addRxAudioCallback(RxAudioCallback callback) {
  std::lock_guard<std::mutex> lock(rxCallbackMutex_);
  const uint64_t id = nextRxCallbackId_++;
  rxAudioCallbacks_[id] = std::move(callback);
  return id;
}

void NodeEngine::removeRxAudioCallback(uint64_t id) {
  std::lock_guard<std::mutex> lock(rxCallbackMutex_);
  rxAudioCallbacks_.erase(id);
}

void NodeEngine::setRxAudioCallback(RxAudioCallback callback) {
  std::lock_guard<std::mutex> lock(rxCallbackMutex_);
  rxAudioCallbacks_.clear();
  rxAudioCallbacks_[nextRxCallbackId_++] = std::move(callback);
}

void NodeEngine::onRadioStatus(const RadioStatus &status) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = status;
  radioRx_ = useRadioSquelch_ && (status.inRx || status.squelchOpen);

  const auto now = Clock::now();
  if (radioRx_) {
    if (!carrierOpen_) {
      openCarrierLocked(now);
    }
    lastVoice_ = now;
  }
  txCv_.notify_all();
}

void NodeEngine::onAudioPcm(const std::vector<int16_t> &pcm, bool transmit) {
  if (pcm.empty() || transmit) {
    return;
  }

  const double db = rmsDb(pcm.data(), pcm.size());
  const auto now = Clock::now();
  bool commandComplete = false;
  bool dtmfActivity = false;
  NodeCommand command = NodeCommand::None;
  std::string code;
  std::vector<int16_t> uplinkPcm;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (useVoxSquelch_ && db >= static_cast<double>(voxThresholdDb_)) {
      voxOpen_ = true;
      lastVoice_ = now;
      if (!carrierOpen_) {
        openCarrierLocked(now);
      }
    }

    if (carrierOpen_ || radioRx_) {
      if (!carrierOpen_) {
        openCarrierLocked(now);
      }
      const size_t remaining =
          rxRun_.size() >= rxRunLimit_ ? 0 : rxRunLimit_ - rxRun_.size();
      if (remaining > 0) {
        const size_t count = std::min(remaining, pcm.size());
        const bool mute = dtmfMuteActive_ || now < dtmfMuteUntil_;
        if (!mute) {
          uplinkPcm = pcm;
        }
        if (mute) {
          rxRun_.insert(rxRun_.end(), count, 0);
        } else {
          const auto gained = applyGain(
              std::vector<int16_t>(
                  pcm.begin(),
                  pcm.begin() + static_cast<std::ptrdiff_t>(count)),
              rxGainDb_);
          rxRun_.insert(rxRun_.end(), gained.begin(), gained.end());
        }
      }

      dtmf_.process(pcm.data(), pcm.size(),
                    [&](char digit, int duration) {
                      const auto digitNow = Clock::now();
                      dtmfActivity = true;
                      lastDigit_ = digitNow;
                      logger_.debug(std::string("DTMF digit ") + digit +
                                    " (" + std::to_string(duration) + " ms)");

                      // Remove the tone from the parrot recording. Detection
                      // happens after a few blocks, so also blank the recent
                      // tail and keep blanking while the command is active.
                      if (commandParser_.buffer().empty() &&
                          digit == commandParser_.prefix()) {
                        dtmfMuteActive_ = true;
                      }
                      dtmfMuteUntil_ =
                          digitNow + std::chrono::milliseconds(120);
                      muteRxRunTailLocked(90);

                      if (commandParser_.feed(digit, command, code)) {
                        dtmfMuteActive_ = false;
                        dtmfMuteUntil_ =
                            Clock::now() + std::chrono::milliseconds(120);
                        muteRxRunTailLocked(90);
                        commandComplete = true;
                        if (command != NodeCommand::None) {
                          pendingCommand_ = command;
                          pendingCommandArg_ =
                              (command == NodeCommand::EchoLinkDial) ? code
                                                                     : "";
                          pendingCommandValid_ = true;
                        } else {
                          logger_.warning("Unknown DTMF command: " + code);
                        }
                        suppressParrot_ = true;
                      }
                    });
    }
  }

  if (commandComplete || dtmfActivity) {
    uplinkPcm.clear();
  }
  if (commandComplete) {
    logger_.info("DTMF command: " + code);
  }
  if (!uplinkPcm.empty()) {
    // Hold the callback lock while dispatching so a client cannot be
    // destroyed while its callback is running.
    std::lock_guard<std::mutex> lock(rxCallbackMutex_);
    for (const auto &entry : rxAudioCallbacks_) {
      if (entry.second) {
        entry.second(uplinkPcm);
      }
    }
  }

  txCv_.notify_all();
}

void NodeEngine::onAudioEnd(bool transmit) {
  if (!transmit) {
    logger_.debug("Radio audio run ended");
  }
}

void NodeEngine::timerLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (running_.load()) {
      tick();
    }
  }
}

void NodeEngine::tick() {
  std::vector<int16_t> run;
  bool hadCommand = false;
  NodeCommand command = NodeCommand::None;
  std::string commandArg;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Clock::now();

    if (voxOpen_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVoice_)
                .count() > voxHangMs_) {
      voxOpen_ = false;
    }

    if (carrierOpen_ && !radioRx_ && !voxOpen_ &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVoice_)
                .count() > voxHangMs_) {
      closeCarrierLocked(now, run, hadCommand, command, commandArg);
    }

    if (!commandParser_.buffer().empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDigit_)
                .count() > 2000) {
      commandParser_.reset();
      dtmfMuteActive_ = false;
      dtmfMuteUntil_ = Clock::time_point{};
    }

    if (!carrierOpen_ && !txActive_.load() && txQueue_.empty() &&
        !callsign_.empty() &&
        std::chrono::duration_cast<std::chrono::seconds>(now - lastIdent_).count() >=
            idIntervalSec_ &&
        std::chrono::duration_cast<std::chrono::seconds>(now - lastActivity_)
                .count() >= idMinGapSec_) {
      queueIdentLocked(now);
    }
  }

  if (!run.empty() || hadCommand) {
    handleRxEnd(std::move(run), hadCommand, command, commandArg);
  }
}

void NodeEngine::openCarrierLocked(Clock::time_point now) {
  if (carrierOpen_) {
    return;
  }
  carrierOpen_ = true;
  rxRun_.clear();
  suppressParrot_ = false;
  pendingCommandValid_ = false;
  pendingCommand_ = NodeCommand::None;
  pendingCommandArg_.clear();
  dtmf_.reset();
  commandParser_.reset();
  dtmfMuteActive_ = false;
  dtmfMuteUntil_ = Clock::time_point{};
  lastVoice_ = now;
  lastDigit_ = now;
  if (!txActive_.load()) {
    if (!txQueue_.empty()) {
      logger_.info("Local RF activity: dropping queued transmit audio");
      txQueue_.clear();
    }
    if (networkActive_.load() || !networkPcm_.empty()) {
      logger_.info("Local RF activity: dropping queued link audio");
      networkPcm_.clear();
      networkActive_.store(false);
      networkEnd_ = false;
    }
  }
  logger_.info("Squelch open");
  txCv_.notify_all();
}

void NodeEngine::closeCarrierLocked(Clock::time_point now,
                                    std::vector<int16_t> &run,
                                    bool &hadCommand, NodeCommand &command,
                                    std::string &commandArg) {
  if (!carrierOpen_) {
    return;
  }
  carrierOpen_ = false;
  run = std::move(rxRun_);
  rxRun_.clear();
  hadCommand = pendingCommandValid_;
  command = pendingCommand_;
  commandArg = pendingCommandArg_;
  pendingCommandArg_.clear();
  pendingCommandValid_ = false;
  pendingCommand_ = NodeCommand::None;

  commandParser_.reset();
  dtmf_.reset();
  dtmfMuteActive_ = false;
  dtmfMuteUntil_ = Clock::time_point{};
  lastActivity_ = now;
  logger_.info("Squelch closed");
  txCv_.notify_all();
}

void NodeEngine::muteRxRunTailLocked(int milliseconds) {
  if (rxRun_.empty() || milliseconds <= 0) {
    return;
  }
  const size_t count = std::min<size_t>(
      rxRun_.size(),
      static_cast<size_t>(kSampleRate) * static_cast<size_t>(milliseconds) /
          1000);
  std::fill(rxRun_.end() - static_cast<std::ptrdiff_t>(count), rxRun_.end(),
            static_cast<int16_t>(0));
}

void NodeEngine::handleRxEnd(std::vector<int16_t> run, bool hadCommand,
                             NodeCommand command, const std::string &commandArg) {
  if (hadCommand) {
    executeCommand(command, run, commandArg);
    return;
  }

  const auto now = Clock::now();
  if (parrotEnabled_ && !suppressParrot_ && !run.empty()) {
    enqueueClip(std::move(run), "parrot",
                now + std::chrono::milliseconds(parrotDelayMs_));
  } else if (courtesyTone_ && !run.empty()) {
    enqueueClip(courtesyClip_, "courtesy",
                now + std::chrono::milliseconds(parrotDelayMs_));
  }
}

void NodeEngine::runCommand(NodeCommand command, const std::string &arg) {
  executeCommand(command, {}, arg);
}

bool NodeEngine::submitDtmfCode(const std::string &code) {
  if (code.empty()) {
    return false;
  }
  const auto it = commandCodes_.find(code);
  if (it != commandCodes_.end()) {
    runCommand(it->second);
    return true;
  }
  if (!dialPrefix_.empty() && code.size() > dialPrefix_.size() + 1 &&
      code.compare(0, dialPrefix_.size(), dialPrefix_) == 0 &&
      code.back() == '#') {
    const std::string digits =
        code.substr(dialPrefix_.size(), code.size() - dialPrefix_.size() - 1);
    if (!digits.empty() &&
        std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        })) {
      runCommand(NodeCommand::EchoLinkDial, digits);
      return true;
    }
  }
  return false;
}

RadioStatus NodeEngine::radioStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

bool NodeEngine::parrotEnabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return parrotEnabled_;
}

void NodeEngine::executeCommand(NodeCommand command,
                                const std::vector<int16_t> &run,
                                const std::string &commandArg) {
  std::lock_guard<std::mutex> commandLock(commandMutex_);
  switch (command) {
  case NodeCommand::ParrotToggle: {
    bool enabled = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      parrotEnabled_ = !parrotEnabled_;
      enabled = parrotEnabled_;
      if (!enabled) {
        cancelTx_.store(txActive_.load());
        txQueue_.clear();
      }
    }
    logger_.info(std::string("Parrot ") + (enabled ? "enabled" : "disabled"));
    if (enabled) {
      if (!speakPrompt("parrot_on")) {
        enqueueClip(ackClip_, "parrot-on", Clock::now());
      }
      if (!run.empty()) {
        enqueueClip(run, "parrot", Clock::now() + std::chrono::milliseconds(parrotDelayMs_));
      }
    } else if (!speakPrompt("parrot_off")) {
      enqueueClip(ackClip_, "parrot-off", Clock::now());
    }
    break;
  }
  case NodeCommand::Help:
    enqueueClip(helpClip_, "help", Clock::now());
    break;
  case NodeCommand::Ident:
    if (!identClip_.empty()) {
      enqueueClip(identClip_, "ident", Clock::now());
      std::lock_guard<std::mutex> lock(mutex_);
      lastIdent_ = Clock::now();
    }
    break;
  case NodeCommand::Reset:
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelTx_.store(txActive_.load());
      txQueue_.clear();
      rxRun_.clear();
      pendingCommandValid_ = false;
      suppressParrot_ = false;
    }
    logger_.info("Node state reset");
    if (!speakPrompt("reset")) {
      enqueueClip(ackClip_, "reset", Clock::now());
    }
    break;
  case NodeCommand::EchoLinkConnect:
    logger_.info("DTMF EchoLink connect to configured node");
    if (echoLinkControl_) {
      echoLinkControl_(true, "");
    } else {
      logger_.warning("EchoLink control is not configured");
    }
    if (!speakPrompt("echo_connecting")) {
      enqueueClip(ackClip_, "echolink", Clock::now());
    }
    break;
  case NodeCommand::EchoLinkDial:
    logger_.info("DTMF EchoLink dial " + commandArg);
    if (echoLinkControl_) {
      echoLinkControl_(true, commandArg);
    } else {
      logger_.warning("EchoLink control is not configured");
    }
    if (!speakPrompt("echo_connecting")) {
      enqueueClip(ackClip_, "echolink", Clock::now());
    }
    break;
  case NodeCommand::EchoLinkDisconnect:
    logger_.info("DTMF EchoLink disconnect");
    if (echoLinkControl_) {
      echoLinkControl_(false, "");
    }
    if (!speakPrompt("echo_disconnected")) {
      enqueueClip(ackClip_, "echolink", Clock::now());
    }
    break;
  case NodeCommand::DiscordJoin:
    logger_.info("DTMF Discord join");
    if (discordControl_) {
      discordControl_(true);
    } else {
      logger_.warning("Discord control is not configured");
    }
    if (!speakPrompt("discord_joining")) {
      enqueueClip(ackClip_, "discord", Clock::now());
    }
    break;
  case NodeCommand::DiscordLeave:
    logger_.info("DTMF Discord leave");
    if (discordControl_) {
      discordControl_(false);
    }
    if (!speakPrompt("discord_left")) {
      enqueueClip(ackClip_, "discord", Clock::now());
    }
    break;
  case NodeCommand::LinkConnect:
    logger_.info("DTMF network link connect");
    if (linkControl_) {
      linkControl_(true);
    } else {
      logger_.warning("Link control is not configured");
    }
    break;
  case NodeCommand::DisconnectAll:
    logger_.info("DTMF disconnect all networks");
    if (disconnectAllControl_) {
      disconnectAllControl_();
    } else {
      if (linkControl_) {
        linkControl_(false);
      }
      if (echoLinkControl_) {
        echoLinkControl_(false, "");
      }
      if (discordControl_) {
        discordControl_(false);
      }
    }
    if (!speakPrompt("all_disconnected")) {
      enqueueClip(ackClip_, "disconnect-all", Clock::now());
    }
    break;
  case NodeCommand::None:
    break;
  }
}

void NodeEngine::enqueueClip(std::vector<int16_t> clip, const std::string &name,
                             Clock::time_point notBefore, bool allowDrop) {
  if (clip.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_.load()) {
    return;
  }
  TxClip tx;
  tx.pcm = std::move(clip);
  tx.name = name;
  tx.notBefore = notBefore;
  tx.allowDrop = allowDrop;
  txQueue_.push_back(std::move(tx));
  txCv_.notify_all();
}

void NodeEngine::queueIdentLocked(Clock::time_point now) {
  if (identClip_.empty()) {
    return;
  }
  TxClip tx;
  tx.pcm = identClip_;
  tx.name = "ident";
  tx.notBefore = now;
  tx.allowDrop = true;
  txQueue_.push_back(std::move(tx));
  lastIdent_ = now;
  txCv_.notify_all();
}

void NodeEngine::enqueueNetworkAudio(const std::vector<int16_t> &pcm,
                                     bool end) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_.load()) {
    return;
  }
  if (!pcm.empty()) {
    if (networkPcm_.size() > networkLimitSamples_) {
      networkPcm_.erase(
          networkPcm_.begin(),
          networkPcm_.begin() + static_cast<std::ptrdiff_t>(
                                   networkPcm_.size() - networkLimitSamples_));
    }
    networkPcm_.insert(networkPcm_.end(), pcm.begin(), pcm.end());
  }
  if (end) {
    networkEnd_ = true;
  }
  networkActive_.store(true);
  lastNetworkAudio_ = Clock::now();
  txCv_.notify_all();
}

void NodeEngine::txLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_.load()) {
    if (txQueue_.empty() && networkActive_.load()) {
      if (carrierOpen_ || status_.inTx || !status_.powerOn ||
          !radioReady_.load() || !session_.audio().isConnected()) {
        txCv_.wait_for(lock, std::chrono::milliseconds(150));
        continue;
      }

      if (networkPcm_.empty()) {
        if (networkEnd_) {
          networkActive_.store(false);
          networkEnd_ = false;
          continue;
        }
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - lastNetworkAudio_)
                .count() > 500) {
          networkActive_.store(false);
          continue;
        }
        txCv_.wait_for(lock, std::chrono::milliseconds(20));
        continue;
      }

      txActive_.store(true);
      cancelTx_.store(false);
      lock.unlock();

      bool ok = session_.audio().beginTx();
      if (!ok) {
        logger_.error("Could not start network transmit audio");
      }
      const auto start = Clock::now();
      size_t sentSamples = 0;

      while (running_.load() && networkActive_.load() && !cancelTx_.load()) {
        std::vector<int16_t> chunk;
        bool end = false;
        lock.lock();
        if (!networkPcm_.empty()) {
          const size_t count =
              std::min<size_t>(kSampleRate / 50, networkPcm_.size());
          chunk.assign(networkPcm_.begin(),
                       networkPcm_.begin() + static_cast<std::ptrdiff_t>(count));
          networkPcm_.erase(
              networkPcm_.begin(),
              networkPcm_.begin() + static_cast<std::ptrdiff_t>(count));
        } else {
          end = networkEnd_ ||
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - lastNetworkAudio_)
                        .count() > 500;
        }
        lock.unlock();

        if (!chunk.empty()) {
          if (ok && !session_.audio().sendPcm(chunk.data(), chunk.size())) {
            logger_.warning("Link transmit audio write failed");
            ok = false;
          }
          sentSamples += chunk.size();
          const auto target =
              start + std::chrono::milliseconds(sentSamples * 1000 / kSampleRate);
          const auto now = Clock::now();
          if (target > now) {
            std::this_thread::sleep_until(target);
          }
        } else if (end) {
          break;
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }

      if (ok) {
        session_.audio().endTx();
      }
      std::string unkeyError;
      if (!session_.forceUnkey(unkeyError)) {
        logger_.warning("Radio may still be keyed: " + unkeyError);
      }

      lock.lock();
      txActive_.store(false);
      networkActive_.store(false);
      networkEnd_ = false;
      networkPcm_.clear();
      lastActivity_ = Clock::now();
      cancelTx_.store(false);
      txCv_.notify_all();
      continue;
    }

    if (txQueue_.empty()) {
      txCv_.wait_for(lock, std::chrono::milliseconds(100));
      continue;
    }

    const auto now = Clock::now();
    TxClip &front = txQueue_.front();
    if (now < front.notBefore) {
      txCv_.wait_until(lock, front.notBefore);
      continue;
    }
    if (carrierOpen_ || status_.inTx || !status_.powerOn ||
        !radioReady_.load() || !session_.audio().isConnected()) {
      txCv_.wait_for(lock, std::chrono::milliseconds(150));
      continue;
    }
    if (cancelTx_.load()) {
      cancelTx_.store(false);
      txQueue_.clear();
      continue;
    }

    TxClip clip = std::move(front);
    txQueue_.pop_front();
    txActive_.store(true);
    cancelTx_.store(false);
    lock.unlock();

    std::vector<int16_t> pcm;
    const size_t leadSamples =
        static_cast<size_t>(kSampleRate) * txLeadSilenceMs_ / 1000;
    const size_t tailSamples =
        static_cast<size_t>(kSampleRate) * txTailSilenceMs_ / 1000;
    pcm.reserve(leadSamples + clip.pcm.size() + tailSamples);
    pcm.insert(pcm.end(), leadSamples, 0);
    const auto gained = applyGain(clip.pcm, txGainDb_);
    pcm.insert(pcm.end(), gained.begin(), gained.end());
    pcm.insert(pcm.end(), tailSamples, 0);

    logger_.info("TX: " + clip.name + " (" +
                 std::to_string(pcm.size() * 1000 / kSampleRate) + " ms)");

    bool ok = session_.audio().beginTx();
    if (!ok) {
      logger_.error("Could not start transmit audio");
    } else {
      const auto start = Clock::now();
      size_t offset = 0;
      const size_t chunkSamples = kSampleRate / 50; // 20 ms
      while (offset < pcm.size() && running_.load() && !cancelTx_.load()) {
        const size_t count = std::min(chunkSamples, pcm.size() - offset);
        if (!session_.audio().sendPcm(pcm.data() + offset, count)) {
          logger_.warning("Transmit audio write failed");
          ok = false;
          break;
        }
        offset += count;

        const auto target =
            start + std::chrono::milliseconds(offset * 1000 / kSampleRate);
        std::this_thread::sleep_until(target);

        if (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start)
                .count() > txTimeoutSec_) {
          logger_.warning("Transmit timeout; aborting");
          cancelTx_.store(true);
          break;
        }
      }
      session_.audio().endTx();
      std::string unkeyError;
      if (!session_.forceUnkey(unkeyError)) {
        logger_.warning("Radio may still be keyed: " + unkeyError);
      }
      if (cancelTx_.load()) {
        logger_.info("Transmission cancelled");
      } else if (ok) {
        logger_.info("TX complete: " + clip.name);
      }
    }

    lock.lock();
    txActive_.store(false);
    lastActivity_ = Clock::now();
    cancelTx_.store(false);
    txCv_.notify_all();
  }
}

double NodeEngine::rmsDb(const int16_t *samples, size_t count) {
  if (samples == nullptr || count == 0) {
    return -120.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < count; ++i) {
    const double value = static_cast<double>(samples[i]) / 32768.0;
    sum += value * value;
  }
  const double rms = std::sqrt(sum / static_cast<double>(count));
  if (rms <= 1e-9) {
    return -120.0;
  }
  return 20.0 * std::log10(rms);
}

std::vector<int16_t> NodeEngine::applyGain(const std::vector<int16_t> &pcm,
                                           double gainDb) const {
  if (gainDb == 0.0 || pcm.empty()) {
    return pcm;
  }
  const double gain = dbToLinear(gainDb);
  std::vector<int16_t> result(pcm.size());
  for (size_t i = 0; i < pcm.size(); ++i) {
    const double value = static_cast<double>(pcm[i]) * gain;
    result[i] = value > 32767.0   ? 32767
                : value < -32768.0 ? -32768
                                   : static_cast<int16_t>(std::lround(value));
  }
  return result;
}

} // namespace vr76
