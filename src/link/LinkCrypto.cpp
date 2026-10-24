#include "LinkCrypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace vr76::link {

namespace {

class AlgorithmHandle {
public:
  ~AlgorithmHandle() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class HashHandle {
public:
  ~HashHandle() {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }
  BCRYPT_HASH_HANDLE handle_ = nullptr;
};

} // namespace

bool hmacSha256(const std::string &key, const uint8_t *data, size_t size,
                uint8_t digest[32]) {
  if (digest == nullptr || (data == nullptr && size != 0)) {
    return false;
  }

  AlgorithmHandle algorithm;
  if (BCryptOpenAlgorithmProvider(&algorithm.handle_, BCRYPT_SHA256_ALGORITHM,
                                  nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
    return false;
  }

  HashHandle hash;
  if (BCryptCreateHash(algorithm.handle_, &hash.handle_, nullptr, 0,
                       reinterpret_cast<PUCHAR>(const_cast<char *>(key.data())),
                       static_cast<ULONG>(key.size()), 0) < 0) {
    return false;
  }
  if (size > 0 &&
      BCryptHashData(hash.handle_, const_cast<PUCHAR>(data),
                     static_cast<ULONG>(size), 0) < 0) {
    return false;
  }
  if (BCryptFinishHash(hash.handle_, digest, 32, 0) < 0) {
    return false;
  }
  return true;
}

bool randomBytes(uint8_t *data, size_t size) {
  if (data == nullptr || size == 0) {
    return false;
  }
  return BCryptGenRandom(nullptr, data, static_cast<ULONG>(size),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

bool constantTimeEqual(const uint8_t *a, const uint8_t *b, size_t size) {
  if (a == nullptr || b == nullptr) {
    return false;
  }
  uint8_t difference = 0;
  for (size_t i = 0; i < size; ++i) {
    difference |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return difference == 0;
}

} // namespace vr76::link
