#pragma once

#include "Config.h"
#include "Logger.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace vr76 {

class PiperTts {
public:
  struct Request {
    std::string key;
    std::string text;
  };
  using Prompts = std::map<std::string, std::vector<int16_t>>;

  PiperTts(Logger &logger, const Config &config);

  bool enabled() const { return enabled_; }
  bool synthesize(const std::string &key, const std::string &text,
                  std::vector<int16_t> &pcm, std::string &error);
  bool synthesizeBatch(const std::vector<Request> &requests, Prompts &prompts,
                       std::string &error);
  bool spellCallsign(const std::string &callsign, std::vector<int16_t> &pcm,
                     std::string &error);

private:
  bool loadCached(const std::string &key, const std::string &text,
                  std::vector<int16_t> &pcm) const;
  std::string cachePath(const std::string &key,
                        const std::string &text) const;
  bool runPiper(const std::vector<Request> &requests, std::string &error);
  static std::string spellToken(char character);

  Logger &logger_;
  bool enabled_ = false;
  std::string exe_;
  std::string model_;
  std::string cacheDir_;
  double lengthScale_ = 1.0;
  double sentenceSilence_ = 0.05;
  int timeoutMs_ = 30000;
  mutable std::mutex mutex_;
};

} // namespace vr76
