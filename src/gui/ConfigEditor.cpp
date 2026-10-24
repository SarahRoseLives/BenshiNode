#include "ConfigEditor.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace vr76::gui {

namespace {

constexpr wchar_t kClassName[] = L"BenshiNodeConfigEditor";

enum : int {
  ID_EDIT = 4000,
  ID_SAVE = 4001,
  ID_RELOAD = 4002,
  ID_CLOSE = 4003,
  ID_PATH = 4004,
  ID_STATUS = 4005
};

std::wstring wide(const std::string &text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length);
  return result;
}

std::string narrow(const std::wstring &text) {
  if (text.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
}

std::string readFile(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

struct ConfigEditor {
  HWND hwnd = nullptr;
  HWND edit = nullptr;
  HWND pathLabel = nullptr;
  HWND status = nullptr;
  HWND save = nullptr;
  HWND reload = nullptr;
  HWND close = nullptr;
  HFONT font = nullptr;
  HFONT monoFont = nullptr;
  std::string path;
  bool dirty = false;

  void createControls() {
    font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
    monoFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Consolas");

    pathLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
                                ES_WANTRETURN,
                            0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EDIT)),
                            nullptr, nullptr);
    save = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE,
                          0, 0, 0, 0, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)),
                          nullptr, nullptr);
    reload = CreateWindowExW(0, L"BUTTON", L"Reload", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RELOAD)),
                             nullptr, nullptr);
    close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                            0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLOSE)),
                            nullptr, nullptr);

    for (HWND control : {pathLabel, status, save, reload, close}) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont), TRUE);
    SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(2000000), 0);
    SetWindowTextW(pathLabel, (L"Editing: " + wide(path)).c_str());
    SetWindowTextW(status, L"Changes require a BenshiNode restart.");
    layout();
  }

  void layout() {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    MoveWindow(pathLabel, 16, 10, std::max(100, width - 32), 22, TRUE);
    MoveWindow(edit, 16, 38, std::max(100, width - 32),
               std::max(100, height - 100), TRUE);
    MoveWindow(status, 16, height - 50, std::max(100, width - 300), 24, TRUE);
    MoveWindow(reload, width - 270, height - 54, 80, 32, TRUE);
    MoveWindow(save, width - 180, height - 54, 80, 32, TRUE);
    MoveWindow(close, width - 90, height - 54, 74, 32, TRUE);
  }

  void loadFromDisk() {
    const std::string text = readFile(path);
    if (text.empty() && !std::filesystem::exists(path)) {
      SetWindowTextW(status, L"Could not read the configuration file.");
      return;
    }
    SetWindowTextW(edit, wide(text).c_str());
    dirty = false;
    SetWindowTextW(status, L"Loaded. Changes require a BenshiNode restart.");
  }

  void saveToDisk() {
    const int length = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));

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
      SetWindowTextW(status, L"Could not write the configuration file.");
      return;
    }
    const std::string bytes = narrow(text);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      SetWindowTextW(status, L"Could not finish writing the configuration file.");
      return;
    }
    dirty = false;
    SetWindowTextW(status, L"Saved. Restart BenshiNode to apply changes.");
  }

  void command(int id) {
    if (id == ID_SAVE) {
      saveToDisk();
    } else if (id == ID_RELOAD) {
      if (dirty) {
        const int answer = MessageBoxW(
            hwnd, L"Discard unsaved changes and reload from disk?", L"BenshiNode",
            MB_ICONQUESTION | MB_YESNO);
        if (answer != IDYES) return;
      }
      loadFromDisk();
    } else if (id == ID_CLOSE) {
      SendMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    ConfigEditor *editor = reinterpret_cast<ConfigEditor *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      editor = reinterpret_cast<ConfigEditor *>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    if (editor == nullptr) {
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int notification = HIWORD(wparam);
      if (id == ID_EDIT && notification == EN_CHANGE) {
        editor->dirty = true;
        return 0;
      }
      editor->command(id);
      return 0;
    }
    case WM_SIZE:
      editor->layout();
      return 0;
    case WM_CTLCOLORSTATIC:
      return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    case WM_GETMINMAXINFO: {
      auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
      info->ptMinTrackSize.x = 700;
      info->ptMinTrackSize.y = 450;
      return 0;
    }
    case WM_CLOSE:
      if (editor->dirty) {
        const int answer = MessageBoxW(
            hwnd, L"Discard unsaved changes?", L"BenshiNode",
            MB_ICONQUESTION | MB_YESNO);
        if (answer != IDYES) return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }
};

} // namespace

void showConfigEditor(HWND parent, const std::string &path) {
  ConfigEditor editor;
  editor.path = path;

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &ConfigEditor::windowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (RegisterClassExW(&wc) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }

  RECT rect{0, 0, 900, 650};
  const DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&rect, style, FALSE);
  HWND hwnd = CreateWindowExW(0, kClassName, L"BenshiNode Settings", style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              parent, nullptr, instance, &editor);
  if (hwnd == nullptr) {
    return;
  }
  editor.hwnd = hwnd;
  editor.createControls();
  editor.loadFromDisk();
  if (parent != nullptr) {
    EnableWindow(parent, FALSE);
  }
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (parent != nullptr) {
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
  }
  if (editor.font != nullptr) DeleteObject(editor.font);
  if (editor.monoFont != nullptr) DeleteObject(editor.monoFont);
}

} // namespace vr76::gui
