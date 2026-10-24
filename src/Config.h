#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Config {
public:
  bool load(const std::string &path, std::string &error);

  bool has(const std::string &section, const std::string &key) const;
  std::string getString(const std::string &section, const std::string &key,
                        const std::string &defaultValue = "") const;
  int getInt(const std::string &section, const std::string &key,
             int defaultValue = 0) const;
  unsigned getUnsigned(const std::string &section, const std::string &key,
                       unsigned defaultValue = 0) const;
  double getDouble(const std::string &section, const std::string &key,
                   double defaultValue = 0.0) const;
  bool getBool(const std::string &section, const std::string &key,
               bool defaultValue = false) const;

  const std::string &path() const { return path_; }

private:
  using Section = std::map<std::string, std::string>;
  std::string path_;
  std::map<std::string, Section> sections_;

  const Section *findSection(const std::string &section) const;
};
