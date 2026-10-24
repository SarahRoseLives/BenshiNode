#include "DtmfDecoder.h"

#include <algorithm>
#include <cmath>

namespace vr76 {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kLowFrequencies[4] = {697.0, 770.0, 852.0, 941.0};
constexpr double kHighFrequencies[4] = {1209.0, 1336.0, 1477.0, 1633.0};

double goertzelAmplitude(const int16_t *samples, int count, double frequency) {
  const double coeff =
      2.0 * std::cos(kTwoPi * frequency / static_cast<double>(DtmfDecoder::kSampleRate));
  double q0 = 0.0;
  double q1 = 0.0;
  double q2 = 0.0;
  double windowSum = 0.0;
  for (int i = 0; i < count; ++i) {
    // Hann window: suppresses leakage from the neighbouring DTMF frequencies.
    const double window =
        0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) /
                            static_cast<double>(count - 1));
    windowSum += window;
    q0 = coeff * q1 - q2 + static_cast<double>(samples[i]) * window;
    q2 = q1;
    q1 = q0;
  }
  const double power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
  if (power <= 0.0 || windowSum <= 0.0) {
    return 0.0;
  }
  return 2.0 * std::sqrt(power) / windowSum;
}

} // namespace

void DtmfDecoder::reset() {
  buffer_.clear();
  activeDigit_ = '\0';
  activeBlocks_ = 0;
  silenceBlocks_ = 0;
  reported_ = false;
}

char DtmfDecoder::digitForFrequencies(int lowIndex, int highIndex) {
  static const char table[4][4] = {
      {'1', '2', '3', 'A'},
      {'4', '5', '6', 'B'},
      {'7', '8', '9', 'C'},
      {'*', '0', '#', 'D'},
  };
  if (lowIndex < 0 || lowIndex > 3 || highIndex < 0 || highIndex > 3) {
    return '\0';
  }
  return table[lowIndex][highIndex];
}

void DtmfDecoder::process(const int16_t *samples, size_t count,
                          const DigitCallback &onDigit) {
  if (samples == nullptr || count == 0) {
    return;
  }
  buffer_.insert(buffer_.end(), samples, samples + count);

  while (buffer_.size() >= kBlockSize) {
    const int16_t *block = buffer_.data();

    double lowAmps[4];
    double highAmps[4];
    for (int i = 0; i < 4; ++i) {
      lowAmps[i] = goertzelAmplitude(block, kBlockSize, kLowFrequencies[i]);
      highAmps[i] = goertzelAmplitude(block, kBlockSize, kHighFrequencies[i]);
    }

    int lowIndex = 0;
    int highIndex = 0;
    for (int i = 1; i < 4; ++i) {
      if (lowAmps[i] > lowAmps[lowIndex]) {
        lowIndex = i;
      }
      if (highAmps[i] > highAmps[highIndex]) {
        highIndex = i;
      }
    }

    double lowSecond = 0.0;
    double highSecond = 0.0;
    for (int i = 0; i < 4; ++i) {
      if (i != lowIndex) {
        lowSecond = std::max(lowSecond, lowAmps[i]);
      }
      if (i != highIndex) {
        highSecond = std::max(highSecond, highAmps[i]);
      }
    }

    const double lowPeak = lowAmps[lowIndex];
    const double highPeak = highAmps[highIndex];
    const bool tonePresent = lowPeak > 900.0 && highPeak > 900.0 &&
                             lowPeak > lowSecond * 1.8 &&
                             highPeak > highSecond * 1.8;

    const char digit =
        tonePresent ? digitForFrequencies(lowIndex, highIndex) : '\0';

    if (digit != '\0') {
      silenceBlocks_ = 0;
      if (digit == activeDigit_) {
        ++activeBlocks_;
      } else {
        activeDigit_ = digit;
        activeBlocks_ = 1;
        reported_ = false;
      }
      const int durationMs = activeBlocks_ * kBlockSize * 1000 / kSampleRate;
      if (!reported_ && durationMs >= 45) {
        reported_ = true;
        if (onDigit) {
          onDigit(digit, durationMs);
        }
      }
    } else {
      ++silenceBlocks_;
      if (silenceBlocks_ >= 2) {
        activeDigit_ = '\0';
        activeBlocks_ = 0;
        reported_ = false;
      }
    }

    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(kBlockSize));
  }
}

} // namespace vr76
