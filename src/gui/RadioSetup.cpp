#include "RadioSetup.h"

#include "bluetooth/RfcommChannel.h"

#include <algorithm>
#include <string>
#include <vector>

namespace vr76::gui {

namespace {

constexpr wchar_t kClassName[] = L"BenshiNodeRadioSetup";

enum : int {
  ID_INTRO = 5000,
  ID_LIST = 5001,
  ID_STATUS = 5002,
  ID_REFRESH = 5003,
  ID_PAIR = 5004,
  ID_USE = 5005,
  ID_CANCEL = 5006
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

struct RadioDialog {
  HWND hwnd = nullptr;
  HWND intro = nullptr;
  HWND list = nullptr;
  HWND status = nullptr;
  HWND refresh = nullptr;
  HWND pair = nullptr;
  HWND use = nullptr;
  HWND cancel = nullptr;
  HFONT font = nullptr;
  std::vector<vr76::BluetoothDeviceInfo> devices;
  std::string currentMac;
  std::string selectedMac;
  bool selectedFound = false;
  bool closed = false;

  void createControls() {
    font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
    intro = CreateWindowExW(0, L"STATIC",
                            L"Put the radio in Bluetooth pairing mode, then Refresh. "
                            L"Pair every entry for the radio: Windows may show separate "
                            L"control and audio entries. Select the entry BenshiNode "
                            L"should use and choose Use this radio.",
                            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr,
                            nullptr, nullptr);
    list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                           WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 0, 0,
                           0, 0, hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LIST)),
                           nullptr, nullptr);
    status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                             0, hwnd, nullptr, nullptr, nullptr);
    refresh = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE,
                              0, 0, 0, 0, hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH)),
                              nullptr, nullptr);
    pair = CreateWindowExW(0, L"BUTTON", L"Pair selected", WS_CHILD | WS_VISIBLE,
                           0, 0, 0, 0, hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PAIR)),
                           nullptr, nullptr);
    use = CreateWindowExW(0, L"BUTTON", L"Use this radio", WS_CHILD | WS_VISIBLE,
                          0, 0, 0, 0, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_USE)),
                          nullptr, nullptr);
    cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                             0, 0, 0, 0, hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL)),
                             nullptr, nullptr);
    for (HWND control : {intro, status, refresh, pair, use, cancel}) {
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    layout();
  }

  void layout() {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    MoveWindow(intro, 16, 12, std::max(100, width - 32), 58, TRUE);
    MoveWindow(list, 16, 78, std::max(100, width - 32),
               std::max(100, height - 150), TRUE);
    MoveWindow(status, 16, height - 58, std::max(100, width - 400), 24, TRUE);
    MoveWindow(refresh, width - 380, height - 62, 82, 32, TRUE);
    MoveWindow(pair, width - 290, height - 62, 106, 32, TRUE);
    MoveWindow(use, width - 176, height - 62, 104, 32, TRUE);
    MoveWindow(cancel, width - 64, height - 62, 48, 32, TRUE);
  }

  void refreshDevices() {
    SetWindowTextW(status, L"Scanning Bluetooth devices...");
    EnableWindow(refresh, FALSE);
    EnableWindow(pair, FALSE);
    EnableWindow(use, FALSE);
    UpdateWindow(hwnd);
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::string error;
    devices = vr76::RfcommChannel::discoverDevices(error);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    EnableWindow(refresh, TRUE);
    EnableWindow(pair, TRUE);
    EnableWindow(use, TRUE);

    std::stable_sort(devices.begin(), devices.end(),
                     [](const auto &a, const auto &b) {
                       if (a.radioCandidate != b.radioCandidate)
                         return a.radioCandidate > b.radioCandidate;
                       if (a.paired != b.paired) return a.paired > b.paired;
                       return a.name < b.name;
                     });

    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    selectedFound = false;
    int selectIndex = -1;
    for (size_t i = 0; i < devices.size(); ++i) {
      const auto &device = devices[i];
      std::wstring text = device.paired ? L"[paired]   " : L"[unpaired] ";
      text += wide(device.name);
      text += L"    " + wide(device.address);
      if (device.radioCandidate) {
        text += L"    [radio]";
      }
      SendMessageW(list, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(text.c_str()));
      if (!currentMac.empty() && device.address == currentMac) {
        selectIndex = static_cast<int>(i);
      }
    }
    if (selectIndex >= 0) {
      SendMessageW(list, LB_SETCURSEL, selectIndex, 0);
    } else if (!devices.empty()) {
      SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
    if (error.empty()) {
      SetWindowTextW(status, L"Select a radio entry and pair it if needed.");
    } else {
      SetWindowTextW(status, wide(error).c_str());
    }
  }

  int selection() const {
    return static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
  }

  void pairSelected() {
    const int index = selection();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
      SetWindowTextW(status, L"Select a Bluetooth entry first.");
      return;
    }
    const auto device = devices[static_cast<size_t>(index)];
    if (device.paired) {
      SetWindowTextW(status, L"That entry is already paired.");
      return;
    }
    SetWindowTextW(status, L"Pairing... confirm the prompt on Windows.");
    EnableWindow(pair, FALSE);
    UpdateWindow(hwnd);
    std::string error;
    if (vr76::RfcommChannel::pairDevice(device.id, error)) {
      SetWindowTextW(status, L"Paired. Refreshing device list...");
      currentMac = device.address;
      refreshDevices();
    } else {
      SetWindowTextW(status, wide("Pairing failed: " + error).c_str());
      EnableWindow(pair, TRUE);
    }
  }

  void useSelected() {
    const int index = selection();
    if (index < 0 || index >= static_cast<int>(devices.size())) {
      SetWindowTextW(status, L"Select a Bluetooth entry first.");
      return;
    }
    const auto &device = devices[static_cast<size_t>(index)];
    if (!device.paired) {
      SetWindowTextW(status, L"Pair this entry before selecting it.");
      return;
    }
    selectedMac = device.address;
    DestroyWindow(hwnd);
  }

  void command(int id, int notification) {
    if (id == ID_LIST && notification == LBN_DBLCLK) {
      useSelected();
    } else if (id == ID_REFRESH) {
      refreshDevices();
    } else if (id == ID_PAIR) {
      pairSelected();
    } else if (id == ID_USE) {
      useSelected();
    } else if (id == ID_CANCEL) {
      DestroyWindow(hwnd);
    }
  }

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam) {
    RadioDialog *dialog = reinterpret_cast<RadioDialog *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
      dialog = reinterpret_cast<RadioDialog *>(create->lpCreateParams);
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
      info->ptMinTrackSize.x = 680;
      info->ptMinTrackSize.y = 420;
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      dialog->closed = true;
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }
};

} // namespace

std::string showRadioSetup(HWND parent, const std::string &currentMac) {
  RadioDialog dialog;
  dialog.currentMac = currentMac;

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &RadioDialog::windowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (RegisterClassExW(&wc) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return {};
  }

  RECT rect{0, 0, 760, 520};
  const DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&rect, style, FALSE);
  HWND hwnd = CreateWindowExW(0, kClassName, L"BenshiNode Radio Setup", style,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              parent, nullptr, instance, &dialog);
  if (hwnd == nullptr) return {};
  dialog.hwnd = hwnd;
  dialog.createControls();
  if (parent != nullptr) EnableWindow(parent, FALSE);
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  dialog.refreshDevices();

  MSG message{};
  while (!dialog.closed && GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  if (parent != nullptr) {
    MSG pending{};
    while (PeekMessageW(&pending, parent, WM_COMMAND, WM_COMMAND, PM_REMOVE)) {
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
  }
  if (dialog.font != nullptr) DeleteObject(dialog.font);
  return dialog.selectedMac;
}

} // namespace vr76::gui
