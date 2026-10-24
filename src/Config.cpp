#include "Config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string trim(const std::string &value) {
  std::size_t first = 0;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return value.substr(first, last - first);
}

std::string upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

} // namespace

bool Config::load(const std::string &path, std::string &error) {
  path_ = path;
  sections_.clear();

  std::ifstream input(path);
  if (!input) {
    error = "Cannot open config file: " + path;
    return false;
  }

  std::string current;
  std::string line;
  int lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::string text = trim(line);
    if (text.empty() || text[0] == ';' || text[0] == '#') {
      continue;
    }
    if (text.front() == '[' && text.back() == ']') {
      current = trim(text.substr(1, text.size() - 2));
      if (current.empty()) {
        error = "Empty section name at line " + std::to_string(lineNumber);
        return false;
      }
      sections_[current];
      continue;
    }
    const auto equals = text.find('=');
    if (equals == std::string::npos) {
      error = "Expected key=value at line " + std::to_string(lineNumber);
      return false;
    }
    if (current.empty()) {
      error = "Key outside of a section at line " + std::to_string(lineNumber);
      return false;
    }
    std::string key = trim(text.substr(0, equals));
    std::string value = trim(text.substr(equals + 1));
    if (key.empty()) {
      error = "Empty key at line " + std::to_string(lineNumber);
      return false;
    }

    // Strip an inline semicolon comment when it is separated from the value by
    // whitespace. This keeps values such as DTMF_SUFFIX=# intact.
    char quote = '\0';
    for (size_t i = 0; i < value.size(); ++i) {
      if (quote != '\0') {
        if (value[i] == quote) {
          quote = '\0';
        }
        continue;
      }
      if (value[i] == '"' || value[i] == '\'') {
        quote = value[i];
        continue;
      }
      if (value[i] == ';' && i > 0 &&
          std::isspace(static_cast<unsigned char>(value[i - 1]))) {
        value = trim(value.substr(0, i));
        break;
      }
    }

    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    sections_[current][key] = value;
  }

  return true;
}

const Config::Section *Config::findSection(const std::string &section) const {
  auto it = sections_.find(section);
  if (it == sections_.end()) {
    return nullptr;
  }
  return &it->second;
}

bool Config::has(const std::string &section, const std::string &key) const {
  const Section *s = findSection(section);
  return s != nullptr && s->find(key) != s->end();
}

std::string Config::getString(const std::string &section,
                              const std::string &key,
                              const std::string &defaultValue) const {
  const Section *s = findSection(section);
  if (!s) {
    return defaultValue;
  }
  auto it = s->find(key);
  return it == s->end() ? defaultValue : it->second;
}

int Config::getInt(const std::string &section, const std::string &key,
                   int defaultValue) const {
  const std::string value = getString(section, key);
  if (value.empty()) {
    return defaultValue;
  }
  try {
    std::size_t used = 0;
    const int result = std::stoi(value, &used, 0);
    return used == 0 ? defaultValue : result;
  } catch (...) {
    return defaultValue;
  }
}

unsigned Config::getUnsigned(const std::string &section, const std::string &key,
                             unsigned defaultValue) const {
  const std::string value = getString(section, key);
  if (value.empty()) {
    return defaultValue;
  }
  try {
    std::size_t used = 0;
    const unsigned long result = std::stoul(value, &used, 0);
    return used == 0 ? defaultValue : static_cast<unsigned>(result);
  } catch (...) {
    return defaultValue;
  }
}

double Config::getDouble(const std::string &section, const std::string &key,
                         double defaultValue) const {
  const std::string value = getString(section, key);
  if (value.empty()) {
    return defaultValue;
  }
  try {
    std::size_t used = 0;
    const double result = std::stod(value, &used);
    return used == 0 ? defaultValue : result;
  } catch (...) {
    return defaultValue;
  }
}

bool Config::getBool(const std::string &section, const std::string &key,
                     bool defaultValue) const {
  const std::string value = upper(trim(getString(section, key)));
  if (value.empty()) {
    return defaultValue;
  }
  if (value == "1" || value == "TRUE" || value == "YES" || value == "ON") {
    return true;
  }
  if (value == "0" || value == "FALSE" || value == "NO" || value == "OFF") {
    return false;
  }
  return defaultValue;
}
