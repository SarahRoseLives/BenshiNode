#include "IniConfig.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vr76::gui {

namespace {

std::string trim(const std::string &value) {
  size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return value.substr(first, last - first);
}

} // namespace

bool IniDocument::load(const std::string &path, std::string &error) {
  sectionOrder_.clear();
  entries_.clear();
  std::ifstream input(path);
  if (!input) {
    error = "Cannot open configuration file: " + path;
    return false;
  }
  std::string section;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string text = trim(line);
    if (text.empty() || text[0] == ';' || text[0] == '#') {
      continue;
    }
    if (text.front() == '[' && text.back() == ']') {
      section = trim(text.substr(1, text.size() - 2));
      if (!section.empty() &&
          std::find(sectionOrder_.begin(), sectionOrder_.end(), section) ==
              sectionOrder_.end()) {
        sectionOrder_.push_back(section);
      }
      continue;
    }
    const size_t equals = text.find('=');
    if (equals == std::string::npos || section.empty()) {
      continue;
    }
    IniEntry entry;
    entry.section = section;
    entry.key = trim(text.substr(0, equals));
    entry.value = trim(text.substr(equals + 1));
    if (!entry.key.empty()) {
      entries_.push_back(std::move(entry));
    }
  }
  return true;
}

bool IniDocument::save(const std::string &path, std::string &error) const {
  try {
    if (std::filesystem::exists(path)) {
      std::error_code ec;
      std::filesystem::copy_file(path, path + ".bak",
                                 std::filesystem::copy_options::overwrite_existing,
                                 ec);
    }
  } catch (...) {
  }

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "Cannot write configuration file: " + path;
    return false;
  }
  output << "; BenshiNode configuration\n";
  for (const std::string &section : sectionOrder_) {
    output << "\n[" << section << "]\n";
    for (const IniEntry &entry : entries_) {
      if (entry.section == section) {
        output << entry.key << "=" << entry.value << "\n";
      }
    }
  }
  if (!output) {
    error = "Failed while writing configuration file";
    return false;
  }
  return true;
}

std::string IniDocument::get(const std::string &section, const std::string &key,
                             const std::string &defaultValue) const {
  for (const IniEntry &entry : entries_) {
    if (entry.section == section && entry.key == key) {
      return entry.value;
    }
  }
  return defaultValue;
}

void IniDocument::set(const std::string &section, const std::string &key,
                      const std::string &value) {
  for (IniEntry &entry : entries_) {
    if (entry.section == section && entry.key == key) {
      entry.value = value;
      return;
    }
  }
  if (std::find(sectionOrder_.begin(), sectionOrder_.end(), section) ==
      sectionOrder_.end()) {
    sectionOrder_.push_back(section);
  }
  entries_.push_back({section, key, value});
}

bool setIniValue(const std::string &path, const std::string &section,
                 const std::string &key, const std::string &value,
                 std::string &error) {
  IniDocument document;
  if (!document.load(path, error)) {
    return false;
  }
  document.set(section, key, value);
  return document.save(path, error);
}

} // namespace vr76::gui
