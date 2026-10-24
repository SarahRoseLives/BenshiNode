#include "ToneGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>

namespace vr76 {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

double dbToAmplitude(double db) {
  return std::pow(10.0, db / 20.0) * 32767.0;
}

int16_t clampSample(double value) {
  if (value > 32767.0) {
    return 32767;
  }
  if (value < -32768.0) {
    return -32768;
  }
  return static_cast<int16_t>(std::lround(value));
}

void appendSine(std::vector<int16_t> &out, double frequencyHz, int milliseconds,
                double amplitude, int fadeMs = 5) {
  if (milliseconds <= 0) {
    return;
  }
  const size_t count =
      static_cast<size_t>(ToneGenerator::kSampleRate) * milliseconds / 1000;
  const size_t fade =
      std::min(count / 2, static_cast<size_t>(ToneGenerator::kSampleRate) *
                              static_cast<size_t>(fadeMs) / 1000);
  out.reserve(out.size() + count);
  for (size_t i = 0; i < count; ++i) {
    double envelope = 1.0;
    if (fade > 0) {
      if (i < fade) {
        envelope = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) /
                                        (2.0 * static_cast<double>(fade)));
      } else if (i + fade >= count) {
        const size_t tail = count - 1 - i;
        envelope = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(tail) /
                                        (2.0 * static_cast<double>(fade)));
      }
    }
    const double value =
        amplitude * envelope *
        std::sin(kTwoPi * frequencyHz * static_cast<double>(i) /
                 static_cast<double>(ToneGenerator::kSampleRate));
    out.push_back(clampSample(value));
  }
}

void appendSilence(std::vector<int16_t> &out, int milliseconds) {
  if (milliseconds <= 0) {
    return;
  }
  const size_t count =
      static_cast<size_t>(ToneGenerator::kSampleRate) * milliseconds / 1000;
  out.insert(out.end(), count, 0);
}

void dtmfFrequencies(char digit, double &low, double &high) {
  switch (digit) {
  case '1':
    low = 697.0;
    high = 1209.0;
    break;
  case '2':
    low = 697.0;
    high = 1336.0;
    break;
  case '3':
    low = 697.0;
    high = 1477.0;
    break;
  case 'A':
    low = 697.0;
    high = 1633.0;
    break;
  case '4':
    low = 770.0;
    high = 1209.0;
    break;
  case '5':
    low = 770.0;
    high = 1336.0;
    break;
  case '6':
    low = 770.0;
    high = 1477.0;
    break;
  case 'B':
    low = 770.0;
    high = 1633.0;
    break;
  case '7':
    low = 852.0;
    high = 1209.0;
    break;
  case '8':
    low = 852.0;
    high = 1336.0;
    break;
  case '9':
    low = 852.0;
    high = 1477.0;
    break;
  case 'C':
    low = 852.0;
    high = 1633.0;
    break;
  case '*':
    low = 941.0;
    high = 1209.0;
    break;
  case '0':
    low = 941.0;
    high = 1336.0;
    break;
  case '#':
    low = 941.0;
    high = 1477.0;
    break;
  case 'D':
    low = 941.0;
    high = 1633.0;
    break;
  default:
    low = 0.0;
    high = 0.0;
    break;
  }
}

void appendDtmfDigit(std::vector<int16_t> &out, char digit, int toneMs,
                     double levelDb) {
  double low = 0.0;
  double high = 0.0;
  dtmfFrequencies(static_cast<char>(std::toupper(static_cast<unsigned char>(digit))),
                  low, high);
  if (low <= 0.0 || high <= 0.0) {
    return;
  }
  const size_t count =
      static_cast<size_t>(ToneGenerator::kSampleRate) * toneMs / 1000;
  const size_t fade =
      static_cast<size_t>(ToneGenerator::kSampleRate) * 3 / 1000;
  const double amplitude = dbToAmplitude(levelDb) * 0.5;
  out.reserve(out.size() + count);
  for (size_t i = 0; i < count; ++i) {
    double envelope = 1.0;
    if (fade > 0 && i < fade) {
      envelope = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) /
                                      (2.0 * static_cast<double>(fade)));
    } else if (fade > 0 && i + fade >= count) {
      const size_t tail = count - 1 - i;
      envelope = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(tail) /
                                      (2.0 * static_cast<double>(fade)));
    }
    const double t = static_cast<double>(i) /
                     static_cast<double>(ToneGenerator::kSampleRate);
    const double value =
        amplitude * envelope * (std::sin(kTwoPi * low * t) +
                                std::sin(kTwoPi * high * t));
    out.push_back(clampSample(value));
  }
}

std::string morseFor(char c) {
  switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
  case 'A': return ".-";
  case 'B': return "-...";
  case 'C': return "-.-.";
  case 'D': return "-..";
  case 'E': return ".";
  case 'F': return "..-.";
  case 'G': return "--.";
  case 'H': return "....";
  case 'I': return "..";
  case 'J': return ".---";
  case 'K': return "-.-";
  case 'L': return ".-..";
  case 'M': return "--";
  case 'N': return "-.";
  case 'O': return "---";
  case 'P': return ".--.";
  case 'Q': return "--.-";
  case 'R': return ".-.";
  case 'S': return "...";
  case 'T': return "-";
  case 'U': return "..-";
  case 'V': return "...-";
  case 'W': return ".--";
  case 'X': return "-..-";
  case 'Y': return "-.--";
  case 'Z': return "--..";
  case '0': return "-----";
  case '1': return ".----";
  case '2': return "..---";
  case '3': return "...--";
  case '4': return "....-";
  case '5': return ".....";
  case '6': return "-....";
  case '7': return "--...";
  case '8': return "---..";
  case '9': return "----.";
  case '/': return "-..-.";
  case '?': return "..--..";
  case '.': return ".-.-.-";
  case ',': return "--..--";
  default: return "";
  }
}

} // namespace

std::vector<int16_t> ToneGenerator::silence(int milliseconds) {
  std::vector<int16_t> result;
  appendSilence(result, milliseconds);
  return result;
}

std::vector<int16_t> ToneGenerator::beep(double frequencyHz, int milliseconds,
                                         double levelDb) {
  std::vector<int16_t> result;
  appendSine(result, frequencyHz, milliseconds, dbToAmplitude(levelDb));
  return result;
}

std::vector<int16_t> ToneGenerator::dtmf(const std::string &digits, int toneMs,
                                         int gapMs, double levelDb) {
  std::vector<int16_t> result;
  for (char digit : digits) {
    appendDtmfDigit(result, digit, toneMs, levelDb);
    appendSilence(result, gapMs);
  }
  return result;
}

std::vector<int16_t> ToneGenerator::cw(const std::string &text, int wpm,
                                       double toneHz, double levelDb) {
  std::vector<int16_t> result;
  if (wpm <= 0) {
    wpm = 20;
  }
  const int unitMs = std::max(1, 1200 / wpm);
  bool firstLetter = true;
  bool pendingWordGap = false;

  for (char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      pendingWordGap = true;
      continue;
    }
    if (pendingWordGap && !firstLetter) {
      appendSilence(result, unitMs * 7);
      pendingWordGap = false;
    }
    const std::string code = morseFor(c);
    if (code.empty()) {
      continue;
    }
    if (!firstLetter) {
      appendSilence(result, unitMs * 3);
    }
    firstLetter = false;
    for (size_t i = 0; i < code.size(); ++i) {
      appendSine(result, toneHz, code[i] == '.' ? unitMs : unitMs * 3,
                 dbToAmplitude(levelDb));
      if (i + 1 < code.size()) {
        appendSilence(result, unitMs);
      }
    }
  }
  appendSilence(result, unitMs * 3);
  return result;
}

void ToneGenerator::append(std::vector<int16_t> &destination,
                           const std::vector<int16_t> &source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

} // namespace vr76
