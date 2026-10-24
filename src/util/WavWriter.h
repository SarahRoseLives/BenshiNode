#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace vr76 {

class WavWriter {
public:
  ~WavWriter();

  bool open(const std::string &path, int sampleRate = 32000);
  bool write(const std::vector<int16_t> &samples);
  void close();
  bool isOpen() const { return file_.is_open(); }

private:
  void writeHeader(uint32_t dataBytes);

  std::ofstream file_;
  int sampleRate_ = 32000;
  uint32_t dataBytes_ = 0;
};

} // namespace vr76
