#include "WavWriter.h"

#include <cstring>

namespace vr76 {

namespace {

void put16(std::ofstream &out, uint16_t value) {
  out.put(static_cast<char>(value & 0xFF));
  out.put(static_cast<char>((value >> 8) & 0xFF));
}

void put32(std::ofstream &out, uint32_t value) {
  out.put(static_cast<char>(value & 0xFF));
  out.put(static_cast<char>((value >> 8) & 0xFF));
  out.put(static_cast<char>((value >> 16) & 0xFF));
  out.put(static_cast<char>((value >> 24) & 0xFF));
}

} // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::string &path, int sampleRate) {
  close();
  sampleRate_ = sampleRate;
  dataBytes_ = 0;
  file_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
  if (!file_) {
    return false;
  }
  writeHeader(0);
  return true;
}

void WavWriter::writeHeader(uint32_t dataBytes) {
  file_.seekp(0);
  file_.write("RIFF", 4);
  put32(file_, 36 + dataBytes);
  file_.write("WAVE", 4);
  file_.write("fmt ", 4);
  put32(file_, 16);
  put16(file_, 1); // PCM
  put16(file_, 1); // mono
  put32(file_, static_cast<uint32_t>(sampleRate_));
  put32(file_, static_cast<uint32_t>(sampleRate_ * 2));
  put16(file_, 2);
  put16(file_, 16);
  file_.write("data", 4);
  put32(file_, dataBytes);
}

bool WavWriter::write(const std::vector<int16_t> &samples) {
  if (!file_.is_open() || samples.empty()) {
    return false;
  }
  const std::streamsize bytes =
      static_cast<std::streamsize>(samples.size() * sizeof(int16_t));
  file_.write(reinterpret_cast<const char *>(samples.data()), bytes);
  dataBytes_ += static_cast<uint32_t>(bytes);
  return file_.good();
}

void WavWriter::close() {
  if (!file_.is_open()) {
    return;
  }
  writeHeader(dataBytes_);
  file_.close();
}

} // namespace vr76
