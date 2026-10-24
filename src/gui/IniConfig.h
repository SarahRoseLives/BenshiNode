#pragma once

#include <string>
#include <vector>

namespace vr76::gui {

struct IniEntry {
  std::string section;
  std::string key;
  std::string value;
};

class IniDocument {
public:
  bool load(const std::string &path, std::string &error);
  bool save(const std::string &path, std::string &error) const;
  std::string get(const std::string &section, const std::string &key,
                  const std::string &defaultValue = "") const;
  void set(const std::string &section, const std::string &key,
           const std::string &value);
  const std::vector<IniEntry> &entries() const { return entries_; }

private:
  std::vector<std::string> sectionOrder_;
  std::vector<IniEntry> entries_;
};

bool setIniValue(const std::string &path, const std::string &section,
                 const std::string &key, const std::string &value,
                 std::string &error);

} // namespace vr76::gui
