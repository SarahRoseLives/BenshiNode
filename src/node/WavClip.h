#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vr76 {

class WavClip {
public:
  static bool load(const std::string &path, std::vector<int16_t> &pcm,
                   std::string &error, int targetSampleRate = 32000);
};

} // namespace vr76
