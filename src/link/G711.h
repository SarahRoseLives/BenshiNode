#pragma once

#include <cstdint>

namespace vr76 {

// G.711 mu-law, 8 kHz mono. Standard companding used by the link audio path.
int16_t ulawToLinear(uint8_t ulaw);
uint8_t linearToUlaw(int16_t pcm);

} // namespace vr76
