#include "Iax2Protocol.h"

#include <algorithm>
#include <utility>

namespace vr76::iax2 {

namespace {

uint8_t encodeSubclass(uint8_t value) {
  if ((value & 0x80) == 0) {
    return value;
  }
  for (int bit = 0; bit < 8; ++bit) {
    if (value == static_cast<uint8_t>(1u << bit)) {
      return static_cast<uint8_t>(0x80 | bit);
    }
  }
  return static_cast<uint8_t>(value & 0x7F);
}

uint8_t decodeSubclass(uint8_t value) {
  if ((value & 0x80) == 0) {
    return value;
  }
  return static_cast<uint8_t>(1u << (value & 0x7F));
}

} // namespace

uint16_t read16be(const uint8_t *data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                               static_cast<uint16_t>(data[1]));
}

uint32_t read32be(const uint8_t *data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

void append16be(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append32be(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(value & 0xFF));
}

bool isFullFrame(const uint8_t *data, size_t size) {
  return data != nullptr && size >= 2 && (data[0] & 0x80) != 0;
}

std::vector<uint8_t> encodeFull(const Frame &frame, bool retransmit) {
  std::vector<uint8_t> out;
  out.reserve(12 + frame.payload.size());
  const uint16_t source = static_cast<uint16_t>(0x8000 | (frame.source & 0x7FFF));
  const uint16_t destination = static_cast<uint16_t>(
      (retransmit ? 0x8000 : 0) | (frame.destination & 0x7FFF));
  append16be(out, source);
  append16be(out, destination);
  append32be(out, frame.timestamp);
  out.push_back(frame.oseq);
  out.push_back(frame.iseq);
  out.push_back(frame.type);
  out.push_back(encodeSubclass(frame.subclass));
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}

bool decodeFull(const uint8_t *data, size_t size, Frame &frame) {
  if (!isFullFrame(data, size) || size < 12) {
    return false;
  }
  frame.source = static_cast<uint16_t>(read16be(data) & 0x7FFF);
  frame.destination = static_cast<uint16_t>(read16be(data + 2) & 0x7FFF);
  frame.timestamp = read32be(data + 4);
  frame.oseq = data[8];
  frame.iseq = data[9];
  frame.type = data[10];
  frame.subclass = decodeSubclass(data[11]);
  frame.payload.assign(data + 12, data + size);
  return true;
}

std::vector<uint8_t> encodeMini(uint16_t source, uint16_t timestamp,
                                const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.reserve(4 + payload.size());
  append16be(out, static_cast<uint16_t>(source & 0x7FFF));
  append16be(out, timestamp);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool decodeMini(const uint8_t *data, size_t size, uint16_t &source,
                uint16_t &timestamp, std::vector<uint8_t> &payload) {
  if (data == nullptr || size < 4 || (data[0] & 0x80) != 0) {
    return false;
  }
  source = static_cast<uint16_t>(read16be(data) & 0x7FFF);
  timestamp = read16be(data + 2);
  payload.assign(data + 4, data + size);
  return true;
}

void IeSet::addRaw(uint8_t id, const std::vector<uint8_t> &data) {
  Entry entry;
  entry.id = id;
  entry.data = data;
  if (entry.data.size() > 255) {
    entry.data.resize(255);
  }
  entries_.push_back(std::move(entry));
}

void IeSet::addString(uint8_t id, const std::string &value) {
  addRaw(id, std::vector<uint8_t>(value.begin(), value.end()));
}

void IeSet::addUint16(uint8_t id, uint16_t value) {
  std::vector<uint8_t> data;
  append16be(data, value);
  addRaw(id, data);
}

void IeSet::addUint32(uint8_t id, uint32_t value) {
  std::vector<uint8_t> data;
  append32be(data, value);
  addRaw(id, data);
}

void IeSet::addByte(uint8_t id, uint8_t value) {
  addRaw(id, {value});
}

std::vector<uint8_t> IeSet::build() const {
  std::vector<uint8_t> out;
  for (const Entry &entry : entries_) {
    out.push_back(entry.id);
    out.push_back(static_cast<uint8_t>(entry.data.size()));
    out.insert(out.end(), entry.data.begin(), entry.data.end());
  }
  return out;
}

IeSet IeSet::parse(const uint8_t *data, size_t size) {
  IeSet set;
  if (data == nullptr) {
    return set;
  }
  size_t offset = 0;
  while (offset + 2 <= size) {
    const uint8_t id = data[offset];
    const size_t length = data[offset + 1];
    offset += 2;
    if (offset + length > size) {
      break;
    }
    Entry entry;
    entry.id = id;
    entry.data.assign(data + offset, data + offset + length);
    set.entries_.push_back(std::move(entry));
    offset += length;
  }
  return set;
}

bool IeSet::has(uint8_t id) const {
  return std::any_of(entries_.begin(), entries_.end(),
                     [id](const Entry &entry) { return entry.id == id; });
}

std::string IeSet::getString(uint8_t id) const {
  for (const Entry &entry : entries_) {
    if (entry.id == id) {
      return std::string(entry.data.begin(), entry.data.end());
    }
  }
  return {};
}

uint32_t IeSet::getUint(uint8_t id) const {
  for (const Entry &entry : entries_) {
    if (entry.id != id) {
      continue;
    }
    switch (entry.data.size()) {
    case 1:
      return entry.data[0];
    case 2:
      return read16be(entry.data.data());
    case 4:
      return read32be(entry.data.data());
    default:
      return 0;
    }
  }
  return 0;
}

std::vector<uint8_t> IeSet::getRaw(uint8_t id) const {
  for (const Entry &entry : entries_) {
    if (entry.id == id) {
      return entry.data;
    }
  }
  return {};
}

} // namespace vr76::iax2
