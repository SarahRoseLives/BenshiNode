#pragma once

#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

enum class LogLevel { Debug = 0, Info = 1, Warning = 2, Error = 3 };

class Logger {
public:
  explicit Logger(LogLevel level = LogLevel::Info);

  bool openFile(const std::string &path);
  void setLevel(LogLevel level) { level_ = level; }
  LogLevel level() const { return level_; }

  void debug(const std::string &message);
  void info(const std::string &message);
  void warning(const std::string &message);
  void error(const std::string &message);
  std::vector<std::string> recent(size_t maximum = 200) const;

private:
  void write(LogLevel level, const std::string &message);

  LogLevel level_;
  mutable std::mutex mutex_;
  std::ofstream file_;
  std::deque<std::string> recent_;
};

const char *logLevelName(LogLevel level);
bool parseLogLevel(const std::string &text, LogLevel &level);
