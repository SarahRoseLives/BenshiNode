#include "Logger.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

std::string timestamp() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
  const std::time_t t = system_clock::to_time_t(now);
  std::tm local{};
  localtime_s(&local, &t);

  std::ostringstream out;
  out << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.'
      << std::setfill('0') << std::setw(3) << ms.count();
  return out.str();
}

} // namespace

Logger::Logger(LogLevel level) : level_(level) {}

bool Logger::openFile(const std::string &path) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_.is_open()) {
    file_.close();
  }
  file_.open(path, std::ios::out | std::ios::app);
  return file_.is_open();
}

void Logger::debug(const std::string &message) {
  write(LogLevel::Debug, message);
}

void Logger::info(const std::string &message) {
  write(LogLevel::Info, message);
}

void Logger::warning(const std::string &message) {
  write(LogLevel::Warning, message);
}

void Logger::error(const std::string &message) {
  write(LogLevel::Error, message);
}

void Logger::write(LogLevel level, const std::string &message) {
  if (level < level_) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string line =
      timestamp() + " [" + logLevelName(level) + "] " + message;
  std::cout << line << std::endl;
  if (file_.is_open()) {
    file_ << line << std::endl;
  }
}

const char *logLevelName(LogLevel level) {
  switch (level) {
  case LogLevel::Debug:
    return "DEBUG";
  case LogLevel::Info:
    return "INFO";
  case LogLevel::Warning:
    return "WARN";
  case LogLevel::Error:
    return "ERROR";
  }
  return "?";
}

bool parseLogLevel(const std::string &text, LogLevel &level) {
  std::string value;
  for (char c : text) {
    value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (value == "debug") {
    level = LogLevel::Debug;
    return true;
  }
  if (value == "info") {
    level = LogLevel::Info;
    return true;
  }
  if (value == "warn" || value == "warning") {
    level = LogLevel::Warning;
    return true;
  }
  if (value == "error") {
    level = LogLevel::Error;
    return true;
  }
  return false;
}
