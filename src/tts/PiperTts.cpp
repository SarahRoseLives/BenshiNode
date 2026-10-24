#include "PiperTts.h"

#include "node/WavClip.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vr76 {

namespace {

std::string sanitizeKey(const std::string &key) {
  std::string result;
  result.reserve(key.size());
  for (unsigned char c : key) {
    if (std::isalnum(c) || c == '-' || c == '_') {
      result.push_back(static_cast<char>(c));
    } else {
      result.push_back('_');
    }
  }
  return result.empty() ? "prompt" : result;
}

uint64_t fnv1a(const std::string &text) {
  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string hexHash(const std::string &text) {
  std::ostringstream out;
  out << std::hex << fnv1a(text);
  return out.str();
}

std::string jsonEscape(const std::string &text) {
  std::string result;
  result.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      if (c < 0x20) {
        constexpr char hex[] = "0123456789abcdef";
        result += "\\u00";
        result += hex[(c >> 4) & 0x0f];
        result += hex[c & 0x0f];
      } else {
        result.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  return result;
}

#ifdef _WIN32

std::wstring utf8ToWide(const std::string &text) {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length);
  return result;
}

std::wstring quoteArg(const std::wstring &value) {
  if (value.empty()) {
    return L"\"\"";
  }
  if (value.find_first_of(L" \t\"") == std::wstring::npos) {
    return value;
  }
  std::wstring result = L"\"";
  size_t backslashes = 0;
  for (wchar_t c : value) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(c);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::string readSmallFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

#endif

std::filesystem::path absoluteIfPresent(const std::string &path) {
  if (path.empty()) {
    return {};
  }
  std::filesystem::path value(path);
  try {
    if (std::filesystem::exists(value)) {
      return std::filesystem::absolute(value);
    }
  } catch (...) {
  }
  return value;
}

} // namespace

PiperTts::PiperTts(Logger &logger, const Config &config)
    : logger_(logger) {
  if (!config.getBool("PIPER", "ENABLED", false)) {
    return;
  }

  const std::filesystem::path exe = absoluteIfPresent(
      config.getString("PIPER", "EXE", ""));
  const std::filesystem::path model = absoluteIfPresent(
      config.getString("PIPER", "MODEL", ""));
  std::filesystem::path cache =
      std::filesystem::path(config.getString("PIPER", "CACHE_DIR", "tts-cache"));
  try {
    cache = std::filesystem::absolute(cache);
  } catch (...) {
  }
  lengthScale_ = config.getDouble("PIPER", "LENGTH_SCALE", 1.0);
  sentenceSilence_ = config.getDouble("PIPER", "SENTENCE_SILENCE", 0.05);
  timeoutMs_ = std::max(1000, config.getInt("PIPER", "TIMEOUT_MS", 30000));

  if (exe.empty() || model.empty() ||
      !std::filesystem::exists(exe) || !std::filesystem::exists(model)) {
    logger_.warning("PIPER is enabled but EXE or MODEL is missing");
    return;
  }

  exe_ = exe.string();
  model_ = model.string();
  cacheDir_ = cache.string();
  try {
    std::filesystem::create_directories(cacheDir_);
  } catch (const std::exception &e) {
    logger_.warning(std::string("Cannot create Piper cache directory: ") +
                    e.what());
    return;
  }
  enabled_ = true;
  logger_.info("Piper voice ready: " +
               std::filesystem::path(model_).filename().string());
}

bool PiperTts::loadCached(const std::string &key, const std::string &text,
                          std::vector<int16_t> &pcm) const {
  const std::filesystem::path path(cachePath(key, text));
  if (!std::filesystem::exists(path)) {
    return false;
  }
  std::string error;
  if (WavClip::load(path.string(), pcm, error)) {
    return true;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return false;
}

std::string PiperTts::cachePath(const std::string &key,
                                const std::string &text) const {
  const std::string identity = model_ + "|" +
                               std::to_string(lengthScale_) + "|" +
                               std::to_string(sentenceSilence_) + "|" + text;
  return (std::filesystem::path(cacheDir_) /
          (sanitizeKey(key) + "_" + hexHash(identity) + ".wav"))
      .string();
}

bool PiperTts::synthesize(const std::string &key, const std::string &text,
                          std::vector<int16_t> &pcm, std::string &error) {
  Prompts prompts;
  if (!synthesizeBatch({{key, text}}, prompts, error)) {
    return false;
  }
  const auto it = prompts.find(key);
  if (it == prompts.end()) {
    error = "Piper did not return audio for " + key;
    return false;
  }
  pcm = it->second;
  return true;
}

bool PiperTts::synthesizeBatch(const std::vector<Request> &requests,
                               Prompts &prompts, std::string &error) {
  if (!enabled_) {
    error = "Piper is not enabled";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Request> missing;
  for (const Request &request : requests) {
    std::vector<int16_t> pcm;
    if (loadCached(request.key, request.text, pcm)) {
      prompts[request.key] = std::move(pcm);
    } else {
      missing.push_back(request);
    }
  }
  if (missing.empty()) {
    return true;
  }

  const auto started = std::chrono::steady_clock::now();
  if (!runPiper(missing, error)) {
    return false;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  logger_.info("Piper generated " + std::to_string(missing.size()) +
               " prompt(s) in " + std::to_string(elapsed.count()) + " ms");

  bool ok = true;
  for (const Request &request : missing) {
    std::vector<int16_t> pcm;
    if (loadCached(request.key, request.text, pcm)) {
      prompts[request.key] = std::move(pcm);
    } else {
      ok = false;
      if (error.empty()) {
        error = "Piper did not generate " + request.key;
      }
    }
  }
  return ok;
}

bool PiperTts::spellCallsign(const std::string &callsign,
                             std::vector<int16_t> &pcm, std::string &error) {
  if (callsign.empty()) {
    pcm.clear();
    return true;
  }

  std::vector<Request> requests;
  std::vector<std::string> order;
  for (char character : callsign) {
    const std::string text = spellToken(character);
    if (text.empty()) {
      continue;
    }
    std::string key = "spell_";
    if (std::isalnum(static_cast<unsigned char>(character))) {
      key.push_back(static_cast<char>(
          std::toupper(static_cast<unsigned char>(character))));
    } else {
      key += sanitizeKey(text);
    }
    requests.push_back({key, text});
    order.push_back(key);
  }
  if (requests.empty()) {
    pcm.clear();
    return true;
  }

  Prompts prompts;
  if (!synthesizeBatch(requests, prompts, error)) {
    return false;
  }

  constexpr int kSampleRate = 32000;
  const std::vector<int16_t> gap(kSampleRate * 50 / 1000, 0);
  pcm.clear();
  for (const std::string &key : order) {
    const auto it = prompts.find(key);
    if (it == prompts.end() || it->second.empty()) {
      continue;
    }
    pcm.insert(pcm.end(), it->second.begin(), it->second.end());
    pcm.insert(pcm.end(), gap.begin(), gap.end());
  }
  while (!pcm.empty() && pcm.back() == 0) {
    pcm.pop_back();
  }
  return true;
}

bool PiperTts::runPiper(const std::vector<Request> &requests,
                        std::string &error) {
#ifdef _WIN32
  const std::string inputPath =
      (std::filesystem::path(cacheDir_) /
       (".piper-input-" + std::to_string(GetCurrentProcessId()) + ".jsonl"))
          .string();
  const std::string errorPath =
      (std::filesystem::path(cacheDir_) /
       (".piper-stderr-" + std::to_string(GetCurrentProcessId()) + ".log"))
          .string();

  {
    std::ofstream input(inputPath, std::ios::binary);
    if (!input) {
      error = "Cannot create Piper input file";
      return false;
    }
    for (const Request &request : requests) {
      const std::string output =
          std::filesystem::path(cachePath(request.key, request.text))
              .generic_string();
      input << "{\"text\":\"" << jsonEscape(request.text)
            << "\",\"output_file\":\"" << jsonEscape(output) << "\"}\n";
    }
  }

  const std::wstring command =
      quoteArg(utf8ToWide(exe_)) + L" --model " +
      quoteArg(utf8ToWide(model_)) + L" --output_dir " +
      quoteArg(utf8ToWide(cacheDir_)) + L" --json-input --length_scale " +
      utf8ToWide(std::to_string(lengthScale_)) + L" --sentence_silence " +
      utf8ToWide(std::to_string(sentenceSilence_)) + L" -q";

  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE inputHandle = CreateFileW(utf8ToWide(inputPath).c_str(), GENERIC_READ,
                                   FILE_SHARE_READ, &attributes, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE outputHandle = CreateFileW(L"NUL", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &attributes, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE errorHandle = CreateFileW(utf8ToWide(errorPath).c_str(),
                                   GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
  if (inputHandle == INVALID_HANDLE_VALUE ||
      outputHandle == INVALID_HANDLE_VALUE ||
      errorHandle == INVALID_HANDLE_VALUE) {
    if (inputHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(inputHandle);
    }
    if (outputHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(outputHandle);
    }
    if (errorHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(errorHandle);
    }
    std::error_code ec;
    std::filesystem::remove(inputPath, ec);
    std::filesystem::remove(errorPath, ec);
    error = "Cannot open Piper process handles";
    return false;
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = inputHandle;
  startup.hStdOutput = outputHandle;
  startup.hStdError = errorHandle;

  PROCESS_INFORMATION process{};
  std::wstring mutableCommand = command;
  const BOOL created = CreateProcessW(
      utf8ToWide(exe_).c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(inputHandle);
  CloseHandle(outputHandle);
  CloseHandle(errorHandle);

  if (!created) {
    const DWORD code = GetLastError();
    std::error_code ec;
    std::filesystem::remove(inputPath, ec);
    std::filesystem::remove(errorPath, ec);
    error = "Cannot start Piper (Windows error " + std::to_string(code) + ")";
    return false;
  }

  const DWORD wait = WaitForSingleObject(process.hProcess,
                                         static_cast<DWORD>(timeoutMs_));
  DWORD exitCode = 0;
  if (wait == WAIT_TIMEOUT) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 2000);
    exitCode = 1;
    error = "Piper timed out";
  } else {
    GetExitCodeProcess(process.hProcess, &exitCode);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  std::string stderrText = readSmallFile(errorPath);
  std::error_code ec;
  std::filesystem::remove(inputPath, ec);
  std::filesystem::remove(errorPath, ec);

  if (exitCode != 0) {
    if (stderrText.size() > 400) {
      stderrText.resize(400);
    }
    if (!stderrText.empty()) {
      error = "Piper failed: " + stderrText;
    } else if (error.empty()) {
      error = "Piper failed with exit code " + std::to_string(exitCode);
    }
    return false;
  }
  return true;
#else
  (void)requests;
  error = "Piper is only supported on Windows";
  return false;
#endif
}

std::string PiperTts::spellToken(char character) {
  const unsigned char value = static_cast<unsigned char>(character);
  if (std::isalpha(value)) {
    return std::string(1, static_cast<char>(std::toupper(value)));
  }
  switch (value) {
  case '0':
    return "zero";
  case '1':
    return "one";
  case '2':
    return "two";
  case '3':
    return "three";
  case '4':
    return "four";
  case '5':
    return "five";
  case '6':
    return "six";
  case '7':
    return "seven";
  case '8':
    return "eight";
  case '9':
    return "nine";
  case '/':
    return "slash";
  case '-':
    return "dash";
  case '.':
    return "dot";
  case ' ':
    return {};
  default:
    return {};
  }
}

} // namespace vr76
