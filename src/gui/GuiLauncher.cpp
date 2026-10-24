#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t modulePath[MAX_PATH]{};
  const DWORD length =
      GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(MAX_PATH));
  if (length == 0 || length >= MAX_PATH) {
    MessageBoxW(nullptr, L"Could not determine the application directory.",
                L"BenshiNode", MB_ICONERROR);
    return 1;
  }

  std::wstring directory(modulePath);
  const std::wstring::size_type slash = directory.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    directory.resize(slash);
  }
  const std::wstring executable = directory + L"\\BenshiNode.exe";
  if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(nullptr, L"BenshiNode.exe was not found next to the launcher.",
                L"BenshiNode", MB_ICONERROR);
    return 1;
  }

  std::wstring config = directory + L"\\BenshiNode.ini";
  if (GetFileAttributesW(config.c_str()) == INVALID_FILE_ATTRIBUTES) {
    config = directory + L"\\..\\..\\BenshiNode.ini";
  }
  if (GetFileAttributesW(config.c_str()) == INVALID_FILE_ATTRIBUTES) {
    config = directory + L"\\..\\..\\..\\BenshiNode.ini";
  }
  if (GetFileAttributesW(config.c_str()) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(nullptr,
                L"BenshiNode.ini was not found next to BenshiNode.exe or in "
                L"the repository root. Run tools\\setup.ps1 first.",
                L"BenshiNode", MB_ICONERROR);
    return 1;
  }

  wchar_t fullConfig[MAX_PATH]{};
  const DWORD configLength = GetFullPathNameW(
      config.c_str(), static_cast<DWORD>(MAX_PATH), fullConfig, nullptr);
  if (configLength == 0 || configLength >= MAX_PATH) {
    MessageBoxW(nullptr, L"Could not resolve the configuration path.",
                L"BenshiNode", MB_ICONERROR);
    return 1;
  }
  std::wstring configPath(fullConfig);
  std::wstring workingDirectory = configPath;
  const std::wstring::size_type configSlash =
      workingDirectory.find_last_of(L"\\/");
  if (configSlash != std::wstring::npos) {
    workingDirectory.resize(configSlash);
  }

  std::wstring command =
      L"\"" + executable + L"\" --gui --config \"" + configPath + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, workingDirectory.c_str(),
                      &startup, &process)) {
    MessageBoxW(nullptr, L"Could not start BenshiNode.exe.", L"BenshiNode",
                MB_ICONERROR);
    return 1;
  }

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return 0;
}
