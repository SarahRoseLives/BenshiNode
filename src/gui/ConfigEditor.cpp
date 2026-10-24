#include "ConfigEditor.h"

#include "IniConfig.h"
#include "RadioSetup.h"

#include <algorithm>
#include <commdlg.h>
#include <string>
#include <vector>

namespace vr76::gui {

namespace {

constexpr wchar_t kClassName[] = L"BenshiNodeConfigEditor";

enum : int {
  ID_LIST = 4000,
  ID_VALUE = 4001,
  ID_LABEL = 4002,
  ID_PATH = 4003,
  ID_STATUS = 4004,
  ID_SAVE = 4005,
  ID_CLOSE = 4006,
  ID_BROWSE = 4007,
  ID_PAIR = 4008
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

struct ConfigDialog {
  HWND hwnd = nullptr;
  HWND listBox = nullptr;
  HWND valueEdit = nullptr;
  HWND labelText = nullptr;
  HWND pathLabel = nullptr;
  HWND statusText = nullptr;
  HWND saveButton = nullptr;
  HWND closeButton = nullptr;
  HWND browseButton = nullptr;
  HWND pairButton = nullptr;
  HFONT font = nullptr;
  std::string path;
  IniDocument document;
  std::vector<IniEntry> entries;
  int current = -1;
  bool suppressChange = false;
  bool dirty = false;

  void createControls() {
    font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");

    pathLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    listBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                           0, 0, 0, 0, hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)),
                           nullptr, nullptr);
    labelText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                            0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    valueEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                ES_AUTOVSCROLL | ES_WANTRETURN,
                            0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VALUE)),
                            nullptr, nullptr);
    statusText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, hwnd, nullptr, nullptr, nullptr);
    saveButton = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE,
                          0, 0, 0, 0, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)),
                          nullptr, nullptr);
    closeButton = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                            0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CLOSE)),
                            nullptr, nullptr);
    browseButton = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BROWSE)),
                             nullptr, nullptr);
    pairButton = CreateWindowExW(0, L"BUTTON", L"Pair radio...", WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PAIR)),
                           nullptr, nullptr);

    for (HWND control : {pathLabel, labelText, statusText, saveButton, closeButton, browseButton, pairButton}) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    SendMessageW(listBox, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(valueEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(valueEdit, EM_SETLIMITTEXT, static_cast<WPARAM>(200000), 0);
    SetWindowTextW(pathLabel, (L"Settings file: " + wide(path)).c_str());
    SetWindowTextW(statusText, L"Changes require a BenshiNode restart.");
    layout();
  }

  void layout() {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    MoveWindow(pathLabel, 16, 10, std::max(100, width - 32), 22, TRUE);
    MoveWindow(pairButton, width - 150, 36, 134, 30, TRUE);
    MoveWindow(listBox, 16, 74, 280, std::max(100, height - 140), TRUE);
    MoveWindow(labelText, 312, 74, std::max(100, width - 328), 24, TRUE);
    MoveWindow(valueEdit, 312, 104, std::max(100, width - 328),
               std::max(100, height - 190), TRUE);
    MoveWindow(browseButton, 312, height - 80, 90, 30, TRUE);
    MoveWindow(statusText, 16, height - 46, std::max(100, width - 380), 24, TRUE);
    MoveWindow(closeButton, width - 280, height - 50, 80, 32, TRUE);
    MoveWindow(saveButton, width - 190, height - 50, 80, 32, TRUE);
  }

  void commitCurrent() {
    if (current < 0 || current >= static_cast<int>(entries.size())) {
      return;
    }
    const int length = GetWindowTextLengthW(valueEdit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(valueEdit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    const IniEntry &entry = entries[static_cast<size_t>(current)];
    document.set(entry.section, entry.key, narrow(text));
  }

  void selectItem(int index) {
    if (index == current) return;
    commitCurrent();
    current = index;
    if (index < 0 || index >= static_cast<int>(entries.size())) {
      SetWindowTextW(labelText, L"Select a setting");
      SetWindowTextW(valueEdit, L"");
      return;
    }
    const IniEntry &entry = entries[static_cast<size_t>(index)];
    SetWindowTextW(labelText, (wide(entry.section) + L" / " + wide(entry.key)).c_str());
    suppressChange = true;
    SetWindowTextW(valueEdit, wide(document.get(entry.section, entry.key)).c_str());
    suppressChange = false;
    SetFocus(valueEdit);
  }

  void load() {
    std::string error;
    if (!document.load(path, error)) {
      SetWindowTextW(statusText, L"Could not read the configuration file.");
      return;
    }
    entries = document.entries();
    SendMessageW(listBox, LB_RESETCONTENT, 0, 0);
    for (const IniEntry &entry : entries) {
      const std::wstring text = wide(entry.section) + L"  /  " + wide(entry.key);
      SendMessageW(listBox, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    current = -1;
    dirty = false;
    if (!entries.empty()) {
      SendMessageW(listBox, LB_SETCURSEL, 0, 0);
      selectItem(0);
    }
    SetWindowTextW(statusText, L"Loaded. Changes require a BenshiNode restart.");
  }

  void saveDocument() {
    commitCurrent();
    std::string error;
    if (!document.save(path, error)) {
      SetWindowTextW(statusText, wide(error).c_str());
      return;
    }
    dirty = false;
    SetWindowTextW(statusText, L"Saved. A .bak backup was created. Restart BenshiNode to apply.");
  }

  bool browseCurrent() {
    if (current < 0 || current >= static_cast<int>(entries.size())) return false;
    const IniEntry &entry = entries[static_cast<size_t>(current)];
    const bool exe = entry.section == "PIPER" && entry.key == "EXE";
    const bool model = entry.section == "PIPER" && entry.key == "MODEL";
    if (!exe && !model) return false;

    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = exe ? L"Select piper.exe" : L"Select Piper voice model";
    dialog.lpstrFilter = exe ? L"Executable (*.exe)\0*.exe\0All files\0*.*\0"
                             : L"Piper model (*.onnx)\0*.onnx\0All files\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return false;
    suppressChange = true;
    SetWindowTextW(valueEdit, file);
    suppressChange = false;
    dirty = true;
    return true;
  }

  void pairRadio() {
    const std::string selected = showRadioSetup(hwnd, document.get("RADIO", "MAC"));
    if (selected.empty()) return;
    document.set("RADIO", "MAC", selected);
    if (current >= 0 && current < static_cast<int>(entries.size())) {
      const IniEntry &entry = entries[static_cast<size_t>(current)];
      if (entry.section == "RADIO" && entry.key == "MAC") {
        suppressChange = true;
        SetWindowTextW(valueEdit, wide(selected).c_str());
        suppressChange = false;
      }
    }
    dirty = true;
    SetWindowTextW(statusText, L"Radio selected. Save to keep it, then restart BenshiNode.");
  }

  void command(int id, int notification) {
    if (id == ID_LIST && notification == LBN_SELCHANGE) {
      selectItem(static_cast<int>(SendMessageW(listBox, LB_GETCURSEL, 0, 0)));
    } else if (id == ID_VALUE && notification == EN_CHANGE) {
      if (!suppressChange) dirty = true;
    } else if (id == ID_SAVE) {
      saveDocument();
    } else if (id == ID_CLOSE) {
      SendMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (id == ID_BROWSE) {
      browseCurrent();
    } else if (id == ID_PAIR) {
      pairRadio();
    }
  }

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    ConfigDialog *dialog = reinterpret_cast<ConfigDialog *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      dialog = reinterpret_cast<ConfigDialog *>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
    }
    if (dialog == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_COMMAND:
      dialog->command(LOWORD(wparam), HIWORD(wparam));
      return 0;
    case WM_SIZE:
      dialog->layout();
      return 0;
    case WM_CTLCOLORSTATIC:
      return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    case WM_GETMINMAXINFO: {
      auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
      info->ptMinTrackSize.x = 760;
      info->ptMinTrackSize.y = 500;
      return 0;
    }
    case WM_CLOSE:
      if (dialog->dirty) {
        const int answer = MessageBoxW(hwnd, L"Discard unsaved changes?",
                                       L"BenshiNode",
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
  ConfigDialog dialog;
  dialog.path = path;

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &ConfigDialog::windowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (RegisterClassExW(&wc) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }

  RECT rect{0, 0, 920, 650};
  const DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&rect, style, FALSE);
  HWND hwnd = CreateWindowExW(0, kClassName, L"BenshiNode Settings", style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              parent, nullptr, instance, &dialog);
  if (hwnd == nullptr) return;
  dialog.hwnd = hwnd;
  dialog.createControls();
  dialog.load();
  if (parent != nullptr) EnableWindow(parent, FALSE);
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
  if (dialog.font != nullptr) DeleteObject(dialog.font);
}

} // namespace vr76::gui
