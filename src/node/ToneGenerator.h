#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vr76 {

class ToneGenerator {
public:
  static constexpr int kSampleRate = 32000;

  static std::vector<int16_t> silence(int milliseconds);
  static std::vector<int16_t> beep(double frequencyHz, int milliseconds,
                                   double levelDb = -12.0);
  static std::vector<int16_t> dtmf(const std::string &digits,
                                   int toneMs = 100, int gapMs = 50,
                                   double levelDb = -12.0);
  static std::vector<int16_t> cw(const std::string &text, int wpm,
                                 double toneHz = 700.0,
                                 double levelDb = -12.0);

  static void append(std::vector<int16_t> &destination,
                     const std::vector<int16_t> &source);
};

} // namespace vr76
