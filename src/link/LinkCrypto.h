#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vr76::link {

bool hmacSha256(const std::string &key, const uint8_t *data, size_t size,
                uint8_t digest[32]);
bool randomBytes(uint8_t *data, size_t size);
bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t size);

} // namespace vr76::link
