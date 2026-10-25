#include "DiscordClient.h"

#include "node/NodeEngine.h"

#include <dpp/dpp.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vr76::discord {

namespace {

void configureCaBundle(Logger &logger) {
#ifdef _WIN32
  if (std::getenv("SSL_CERT_FILE") != nullptr) {
    return;
  }
  char modulePath[MAX_PATH]{};
  const DWORD length =
      GetModuleFileNameA(nullptr, modulePath, static_cast<DWORD>(MAX_PATH));
  if (length == 0 || length >= MAX_PATH) {
    return;
  }
  const std::filesystem::path caPath =
      std::filesystem::path(modulePath).parent_path() / "ca-bundle.crt";
  if (!std::filesystem::exists(caPath)) {
    logger.warning("Discord CA bundle not found: " + caPath.string());
    return;
  }
  _putenv_s("SSL_CERT_FILE", caPath.string().c_str());
#else
  (void)logger;
#endif
}

class Resampler32to48 {
public:
  void reset() {
    buffer_.clear();
    position_ = 0.0;
  }

  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &out) {
    if (samples == nullptr || count == 0) {
      return;
    }
    buffer_.insert(buffer_.end(), samples, samples + count);
    while (position_ + 1.0 < static_cast<double>(buffer_.size())) {
      const size_t index = static_cast<size_t>(position_);
      const double fraction = position_ - static_cast<double>(index);
      const double value = buffer_[index] +
                           (buffer_[index + 1] - buffer_[index]) * fraction;
      out.push_back(static_cast<int16_t>(std::lround(value)));
      position_ += 2.0 / 3.0;
    }
    const size_t drop = std::min<size_t>(buffer_.size(),
                                         static_cast<size_t>(position_));
    if (drop > 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
      position_ -= static_cast<double>(drop);
    }
  }

private:
  std::vector<int16_t> buffer_;
  double position_ = 0.0;
};

class Resampler48to32 {
public:
  void reset() {
    buffer_.clear();
    position_ = 0.0;
  }

  void process(const int16_t *samples, size_t count,
               std::vector<int16_t> &out) {
    if (samples == nullptr || count == 0) {
      return;
    }
    buffer_.insert(buffer_.end(), samples, samples + count);
    while (position_ + 1.0 < static_cast<double>(buffer_.size())) {
      const size_t index = static_cast<size_t>(position_);
      const double fraction = position_ - static_cast<double>(index);
      const double value = buffer_[index] +
                           (buffer_[index + 1] - buffer_[index]) * fraction;
      out.push_back(static_cast<int16_t>(std::lround(value)));
      position_ += 3.0 / 2.0;
    }
    const size_t drop = std::min<size_t>(buffer_.size(),
                                         static_cast<size_t>(position_));
    if (drop > 0) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
      position_ -= static_cast<double>(drop);
    }
  }

private:
  std::vector<int16_t> buffer_;
  double position_ = 0.0;
};

} // namespace

DiscordClient::DiscordClient(Logger &logger, const Config &config,
                             NodeEngine &node)
    : logger_(logger), config_(config), node_(node) {
  token_ = config.getString("DISCORD", "TOKEN", "");
  const std::string guild = config.getString("DISCORD", "GUILD_ID", "");
  const std::string channel = config.getString("DISCORD", "CHANNEL_ID", "");
  try {
    guildId_ = guild.empty() ? 0 : std::stoull(guild);
    channelId_ = channel.empty() ? 0 : std::stoull(channel);
  } catch (...) {
    guildId_ = 0;
    channelId_ = 0;
  }
  autoJoin_.store(config.getBool("DISCORD", "ENABLED", false));
  rxCallbackId_ = node_.addRxAudioCallback(
      [this](const std::vector<int16_t> &pcm) { pushRadioPcm(pcm); });
}

DiscordClient::~DiscordClient() {
  if (rxCallbackId_ != 0) {
    node_.removeRxAudioCallback(rxCallbackId_);
    rxCallbackId_ = 0;
  }
  stop();
}

bool DiscordClient::start(std::string &error) {
  if (bot_) {
    return true;
  }
  if (token_.empty()) {
    error = "DISCORD/TOKEN is not configured";
    return false;
  }
  if (guildId_ == 0 || channelId_ == 0) {
    error = "DISCORD/GUILD_ID and DISCORD/CHANNEL_ID are required";
    return false;
  }

  configureCaBundle(logger_);

  try {
    bot_ = std::make_unique<dpp::cluster>(
        token_, dpp::i_guilds | dpp::i_guild_voice_states);
    bot_->on_log([this](const dpp::log_t &event) {
      switch (event.severity) {
      case dpp::ll_trace:
      case dpp::ll_debug:
        logger_.debug("Discord: " + event.message);
        break;
      case dpp::ll_info:
        logger_.info("Discord: " + event.message);
        break;
      case dpp::ll_warning:
        logger_.warning("Discord: " + event.message);
        break;
      case dpp::ll_error:
      case dpp::ll_critical:
        logger_.error("Discord: " + event.message);
        break;
      default:
        break;
      }
    });
    bot_->on_ready([this](const dpp::ready_t &) {
      ready_.store(true);
      logger_.info("Discord bot connected");
      if (autoJoin_.load()) {
        std::string joinError;
        if (!join(joinError)) {
          logger_.warning("Discord auto-join failed: " + joinError);
        }
      }
    });
    bot_->on_voice_ready([this](const dpp::voice_ready_t &) {
      voiceReady_.store(true);
      logger_.info("Discord voice channel ready");
    });
    bot_->on_voice_receive(
        [this](const dpp::voice_receive_t &event) { onVoiceReceive(event); });

    bot_->start(dpp::st_return);
    running_.store(true);
    transmitThread_ = std::thread([this]() { transmitLoop(); });
    logger_.info("Discord integration started; use 6# to join, 7# to leave");
    return true;
  } catch (const std::exception &e) {
    error = std::string("Discord startup failed: ") + e.what();
    bot_.reset();
    return false;
  }
}

void DiscordClient::stop() {
  running_.store(false);
  leave();
  if (transmitThread_.joinable()) {
    transmitThread_.join();
  }
  if (bot_) {
    try {
      bot_->shutdown();
    } catch (...) {
    }
    bot_.reset();
  }
  ready_.store(false);
  voiceReady_.store(false);
}

bool DiscordClient::join(std::string &error) {
  if (!bot_) {
    error = "Discord client is not started";
    return false;
  }
  if (!ready_.load()) {
    error = "Discord bot is not connected yet";
    return false;
  }
  if (voiceReady_.load()) {
    logger_.info("Already connected to the Discord voice channel");
    return true;
  }
  try {
    auto &shards = bot_->get_shards();
    if (shards.empty()) {
      error = "Discord has no active shards";
      return false;
    }
    auto *shard = shards.begin()->second.get();
    shard->connect_voice(dpp::snowflake(guildId_),
                         dpp::snowflake(channelId_), false, false, true);
    logger_.info("Joining Discord voice channel " +
                 std::to_string(channelId_));
    return true;
  } catch (const std::exception &e) {
    error = std::string("Discord join failed: ") + e.what();
    return false;
  }
}

void DiscordClient::leave() {
  if (!bot_ || !ready_.load()) {
    return;
  }
  try {
    auto &shards = bot_->get_shards();
    if (!shards.empty()) {
      shards.begin()->second->disconnect_voice(dpp::snowflake(guildId_));
    }
  } catch (...) {
  }
  voiceReady_.store(false);
  logger_.info("Left Discord voice channel");
}

void DiscordClient::pushRadioPcm(const std::vector<int16_t> &pcm) {
  if (pcm.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(txMutex_);
  const size_t maximum = 32000 * 3;
  if (tx32_.size() > maximum) {
    tx32_.erase(tx32_.begin(),
                tx32_.begin() + static_cast<std::ptrdiff_t>(tx32_.size() -
                                                            maximum));
  }
  tx32_.insert(tx32_.end(), pcm.begin(), pcm.end());
}

void DiscordClient::transmitLoop() {
  Resampler32to48 upsampler;
  auto nextSend = std::chrono::steady_clock::now();

  while (running_.load()) {
    std::vector<int16_t> pcm32;
    {
      std::lock_guard<std::mutex> lock(txMutex_);
      if (!tx32_.empty()) {
        pcm32.swap(tx32_);
      }
    }
    if (!pcm32.empty()) {
      upsampler.process(pcm32.data(), pcm32.size(), tx48Mono_);
    }

    while (tx48Mono_.size() >= 2880) {
      if (!voiceReady_.load()) {
        tx48Mono_.clear();
        break;
      }

      std::vector<int16_t> stereo(2880 * 2);
      for (size_t i = 0; i < 2880; ++i) {
        stereo[i * 2] = tx48Mono_[i];
        stereo[i * 2 + 1] = tx48Mono_[i];
      }
      tx48Mono_.erase(tx48Mono_.begin(),
                      tx48Mono_.begin() + 2880);

      if (!sendStereo(stereo)) {
        break;
      }

      nextSend += std::chrono::milliseconds(60);
      const auto now = std::chrono::steady_clock::now();
      if (nextSend > now) {
        std::this_thread::sleep_until(nextSend);
      } else {
        nextSend = now;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool DiscordClient::sendStereo(const std::vector<int16_t> &stereo) {
  if (!bot_) {
    return false;
  }
  try {
    auto &shards = bot_->get_shards();
    if (shards.empty()) {
      return false;
    }
    auto *shard = shards.begin()->second.get();
    auto *voice = shard->get_voice(dpp::snowflake(guildId_));
    if (voice == nullptr || voice->voiceclient == nullptr ||
        !voice->voiceclient->is_ready()) {
      return false;
    }
    voice->voiceclient->send_audio_raw(
        reinterpret_cast<uint16_t *>(const_cast<int16_t *>(stereo.data())),
        stereo.size() * sizeof(int16_t));
    return true;
  } catch (const std::exception &e) {
    logger_.warning(std::string("Discord audio send failed: ") + e.what());
    return false;
  }
}

void DiscordClient::onVoiceReceive(const dpp::voice_receive_t &event) {
  if (!voiceReady_.load() || event.audio_data.size() < 4) {
    return;
  }
  if (bot_ && event.user_id == bot_->me.id) {
    return;
  }

  const size_t frames = event.audio_data.size() / 4;
  std::vector<int16_t> mono48(frames);
  const uint8_t *data = event.audio_data.data();
  for (size_t i = 0; i < frames; ++i) {
    int16_t left = 0;
    int16_t right = 0;
    std::memcpy(&left, data + i * 4, sizeof(left));
    std::memcpy(&right, data + i * 4 + 2, sizeof(right));
    mono48[i] = static_cast<int16_t>((static_cast<int32_t>(left) + right) / 2);
  }

  static thread_local Resampler48to32 downsampler;
  std::vector<int16_t> pcm32;
  downsampler.process(mono48.data(), mono48.size(), pcm32);
  if (!pcm32.empty()) {
    node_.enqueueNetworkAudio(pcm32, false);
  }
}

} // namespace vr76::discord
