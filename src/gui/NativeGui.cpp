#include "NativeGui.h"

#include "ConfigEditor.h"
#include "IniConfig.h"
#include "RadioSetup.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iterator>
#include <sstream>

namespace vr76::gui {

namespace {

constexpr wchar_t kWindowClass[] = L"BenshiNodeNativeGui";
constexpr int kClientWidth = 1000;
constexpr int kClientHeight = 840;

enum ControlId {
  ID_TITLE = 1000,
  ID_HELP = 1001,
  ID_IDENT = 1002,
  ID_PARROT = 1003,
  ID_RESET = 1004,
  ID_UNKEY = 1005,
  ID_STOP = 1006,
  ID_SETTINGS = 1007,
  ID_PAIR_RADIO = 1008,
  ID_RESTART = 1009,
  ID_CHANNEL_EDIT = 1030,
  ID_CHANNEL_APPLY = 1031,
  ID_ECHO_CONNECT = 1010,
  ID_ECHO_DISCONNECT = 1011,
  ID_ECHO_DIAL = 1012,
  ID_ECHO_NODE = 1013,
  ID_DISCORD_JOIN = 1020,
  ID_DISCORD_LEAVE = 1021,
  ID_KEY_0 = 2000,
  ID_KEY_1,
  ID_KEY_2,
  ID_KEY_3,
  ID_KEY_4,
  ID_KEY_5,
  ID_KEY_6,
  ID_KEY_7,
  ID_KEY_8,
  ID_KEY_9,
  ID_KEY_HASH = 2010,
  ID_KEY_CLEAR = 2011,
  ID_SEND = 2012,
  ID_LOG = 3000
};

std::wstring wide(const std::string &text) {
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

std::string narrow(const std::wstring &text) {
  if (text.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
}

HWND makeControl(HWND parent, const wchar_t *type, const wchar_t *text, DWORD style,
                 int id, HFONT font) {
  HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0,
                                 0, 0, 0, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
  if (control != nullptr && font != nullptr) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
  return control;
}

} // namespace

NativeGui::NativeGui(StateProvider state, LogProvider logs, ActionHandler action,
                     CloseHandler close, std::string configPath)
    : state_(std::move(state)), logs_(std::move(logs)),
      action_(std::move(action)), close_(std::move(close)),
      configPath_(std::move(configPath)) {}

NativeGui::~NativeGui() { stop(); }

bool NativeGui::start(std::string &error) {
  thread_ = std::thread([this]() { threadMain(); });
  std::unique_lock<std::mutex> lock(startMutex_);
  if (!startCv_.wait_for(lock, std::chrono::seconds(5),
                         [this]() { return started_; })) {
    error = "GUI startup timed out";
    return false;
  }
  if (!startError_.empty()) {
    error = startError_;
    return false;
  }
  return true;
}

void NativeGui::stop() {
  if (hwnd_ != nullptr) {
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  hwnd_ = nullptr;
}

void NativeGui::notifyStarted(const std::string &error) {
  std::lock_guard<std::mutex> lock(startMutex_);
  started_ = true;
  startError_ = error;
  startCv_.notify_all();
}

void NativeGui::threadMain() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &NativeGui::windowProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kWindowClass;
  if (RegisterClassExW(&wc) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    notifyStarted("Could not register GUI window class");
    return;
  }

  font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                      L"Segoe UI");
  titleFont_ = CreateFontW(-26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  logFont_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

  RECT rect{0, 0, kClientWidth, kClientHeight};
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  AdjustWindowRect(&rect, style, FALSE);
  hwnd_ = CreateWindowExW(0, kWindowClass, L"BenshiNode Control", style,
                          CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
                          rect.bottom - rect.top, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    notifyStarted("Could not create GUI window");
    return;
  }

  createControls();
  layoutControls();
  refresh();
  refreshLogs();
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
  SetTimer(hwnd_, 1, 500, nullptr);
  notifyStarted("");

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  KillTimer(hwnd_, 1);
  closed_.store(true);
  if (close_) {
    close_();
  }
  if (font_ != nullptr) {
    DeleteObject(font_);
  }
  if (titleFont_ != nullptr) {
    DeleteObject(titleFont_);
  }
  if (logFont_ != nullptr) {
    DeleteObject(logFont_);
  }
}

LRESULT CALLBACK NativeGui::windowProc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  NativeGui *gui = reinterpret_cast<NativeGui *>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
    gui = reinterpret_cast<NativeGui *>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(gui));
  }
  if (gui != nullptr) {
    return gui->handleMessage(hwnd, message, wparam, lparam);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT NativeGui::handleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  switch (message) {
  case WM_COMMAND:
    onCommand(LOWORD(wparam));
    return 0;
  case WM_TIMER:
    refresh();
    refreshLogs();
    return 0;
  case WM_CTLCOLORSTATIC:
    return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

void NativeGui::createControls() {
  title_ = makeControl(hwnd_, L"STATIC", L"BenshiNode", SS_LEFT, ID_TITLE,
                       titleFont_);
  callsign_ = makeControl(hwnd_, L"STATIC", L"", SS_LEFT, 0, font_);
  connection_ = makeControl(hwnd_, L"STATIC", L"Connecting", SS_RIGHT, 0, font_);
  pairButton_ = makeControl(hwnd_, L"BUTTON", L"Pair radio", BS_PUSHBUTTON,
                            ID_PAIR_RADIO, font_);
  settingsButton_ = makeControl(hwnd_, L"BUTTON", L"Settings", BS_PUSHBUTTON,
                                ID_SETTINGS, font_);
  restartButton_ = makeControl(hwnd_, L"BUTTON", L"Restart", BS_PUSHBUTTON,
                               ID_RESTART, font_);

  makeControl(hwnd_, L"BUTTON", L"Radio", BS_GROUPBOX, 0, font_);
  headline_ = makeControl(hwnd_, L"STATIC", L"Waiting for radio", SS_LEFT, 0,
                          titleFont_);
  detail_ = makeControl(hwnd_, L"STATIC", L"", SS_LEFT, 0, font_);
  channel_ = makeControl(hwnd_, L"STATIC", L"Channel: --", SS_LEFT, 0, font_);
  rssi_ = makeControl(hwnd_, L"STATIC", L"RSSI: --", SS_LEFT, 0, font_);
  txrx_ = makeControl(hwnd_, L"STATIC", L"TX: --  RX: --", SS_LEFT, 0, font_);
  voice_ = makeControl(hwnd_, L"STATIC", L"", SS_LEFT, 0, font_);

  makeControl(hwnd_, L"BUTTON", L"Quick actions (no star prefix)", BS_GROUPBOX, 0,
              font_);
  helpButton_ = makeControl(hwnd_, L"BUTTON", L"Help  0#", BS_PUSHBUTTON,
                            ID_HELP, font_);
  identButton_ = makeControl(hwnd_, L"BUTTON", L"Identify  3#", BS_PUSHBUTTON,
                             ID_IDENT, font_);
  parrotButton_ = makeControl(hwnd_, L"BUTTON", L"Parrot", BS_PUSHBUTTON,
                              ID_PARROT, font_);
  resetButton_ = makeControl(hwnd_, L"BUTTON", L"Reset  9#", BS_PUSHBUTTON,
                             ID_RESET, font_);
  unkeyButton_ = makeControl(hwnd_, L"BUTTON", L"Unkey", BS_PUSHBUTTON,
                             ID_UNKEY, font_);
  stopButton_ = makeControl(hwnd_, L"BUTTON", L"Stop node", BS_PUSHBUTTON,
                            ID_STOP, font_);

  makeControl(hwnd_, L"BUTTON", L"DTMF keypad", BS_GROUPBOX, 0, font_);
  command_ = makeControl(hwnd_, L"STATIC", L"0#", SS_CENTER | SS_CENTERIMAGE,
                         0, titleFont_);
  const wchar_t *keys[12] = {L"1", L"2", L"3", L"4", L"5", L"6",
                             L"7", L"8", L"9", L"Clear", L"0", L"#"};
  const int ids[12] = {ID_KEY_1, ID_KEY_2, ID_KEY_3, ID_KEY_4, ID_KEY_5,
                       ID_KEY_6, ID_KEY_7, ID_KEY_8, ID_KEY_9, ID_KEY_CLEAR,
                       ID_KEY_0, ID_KEY_HASH};
  for (int i = 0; i < 12; ++i) {
    HWND button = makeControl(hwnd_, L"BUTTON", keys[i], BS_PUSHBUTTON, ids[i],
                              font_);
    keypadButtons_.push_back(button);
  }
  sendButton_ = makeControl(hwnd_, L"BUTTON", L"Send command", BS_PUSHBUTTON,
                            ID_SEND, font_);

  makeControl(hwnd_, L"BUTTON", L"EchoLink", BS_GROUPBOX, 0, font_);
  echoState_ = makeControl(hwnd_, L"STATIC", L"Not configured", SS_LEFT, 0,
                           font_);
  echoConnect_ = makeControl(hwnd_, L"BUTTON", L"Connect  4#", BS_PUSHBUTTON,
                             ID_ECHO_CONNECT, font_);
  echoDisconnect_ = makeControl(hwnd_, L"BUTTON", L"Disconnect  5#",
                                BS_PUSHBUTTON, ID_ECHO_DISCONNECT, font_);
  echoNode_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                              hwnd_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ECHO_NODE)),
                              GetModuleHandleW(nullptr), nullptr);
  SendMessageW(echoNode_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  SetWindowTextW(echoNode_, L"");
  echoDial_ = makeControl(hwnd_, L"BUTTON", L"Dial", BS_PUSHBUTTON, ID_ECHO_DIAL,
                          font_);

  makeControl(hwnd_, L"BUTTON", L"Discord", BS_GROUPBOX, 0, font_);
  discordState_ = makeControl(hwnd_, L"STATIC", L"Not configured", SS_LEFT, 0,
                              font_);
  discordJoin_ = makeControl(hwnd_, L"BUTTON", L"Join  6#", BS_PUSHBUTTON,
                             ID_DISCORD_JOIN, font_);
  discordLeave_ = makeControl(hwnd_, L"BUTTON", L"Leave  7#", BS_PUSHBUTTON,
                              ID_DISCORD_LEAVE, font_);

  makeControl(hwnd_, L"BUTTON", L"Bridge", BS_GROUPBOX, 0, font_);
  bridgeState_ = makeControl(hwnd_, L"STATIC", L"Idle", SS_LEFT, 0, font_);

  makeControl(hwnd_, L"BUTTON", L"Channel", BS_GROUPBOX, 0, font_);
  channelHint_ = makeControl(hwnd_, L"STATIC", L"Blank = use the radio current channel", SS_LEFT, 0, font_);
  channelEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHANNEL_EDIT)),
                                GetModuleHandleW(nullptr), nullptr);
  SendMessageW(channelEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  channelApply_ = makeControl(hwnd_, L"BUTTON", L"Apply", BS_PUSHBUTTON,
                              ID_CHANNEL_APPLY, font_);

  makeControl(hwnd_, L"BUTTON", L"Live log", BS_GROUPBOX, 0, font_);
  log_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                             ES_READONLY | ES_AUTOVSCROLL,
                         0, 0, 0, 0, hwnd_,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LOG)),
                         GetModuleHandleW(nullptr), nullptr);
  SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(logFont_), TRUE);
}

void NativeGui::layoutControls() {
  MoveWindow(title_, 20, 14, 260, 40, TRUE);
  MoveWindow(callsign_, 20, 50, 260, 22, TRUE);
  MoveWindow(pairButton_, 500, 20, 106, 32, TRUE);
  MoveWindow(settingsButton_, 620, 20, 100, 32, TRUE);
  MoveWindow(restartButton_, 730, 20, 84, 32, TRUE);
  MoveWindow(connection_, 824, 28, 156, 22, TRUE);

  MoveWindow(GetDlgItem(hwnd_, 0), 0, 0, 0, 0, FALSE);

  const HWND radioGroup = FindWindowExW(hwnd_, nullptr, L"BUTTON", L"Radio");
  MoveWindow(radioGroup, 20, 78, 600, 120, TRUE);
  MoveWindow(headline_, 40, 102, 560, 36, TRUE);
  MoveWindow(detail_, 40, 138, 560, 22, TRUE);
  MoveWindow(channel_, 40, 166, 170, 22, TRUE);
  MoveWindow(rssi_, 225, 166, 150, 22, TRUE);
  MoveWindow(txrx_, 390, 166, 190, 22, TRUE);

  const HWND actionGroup =
      FindWindowExW(hwnd_, nullptr, L"BUTTON", L"Quick actions (no star prefix)");
  MoveWindow(actionGroup, 20, 212, 600, 110, TRUE);
  const int actionX[6] = {38, 132, 226, 320, 414, 508};
  MoveWindow(helpButton_, actionX[0], 242, 84, 44, TRUE);
  MoveWindow(identButton_, actionX[1], 242, 84, 44, TRUE);
  MoveWindow(parrotButton_, actionX[2], 242, 84, 44, TRUE);
  MoveWindow(resetButton_, actionX[3], 242, 84, 44, TRUE);
  MoveWindow(unkeyButton_, actionX[4], 242, 84, 44, TRUE);
  MoveWindow(stopButton_, actionX[5], 242, 84, 44, TRUE);

  const HWND keypadGroup =
      FindWindowExW(hwnd_, nullptr, L"BUTTON", L"DTMF keypad");
  MoveWindow(keypadGroup, 20, 336, 600, 350, TRUE);
  MoveWindow(command_, 40, 364, 560, 40, TRUE);
  const int keyX[3] = {38, 202, 366};
  const int keyY[4] = {426, 484, 542, 600};
  for (int i = 0; i < 12; ++i) {
    const int row = i / 3;
    const int col = i % 3;
    MoveWindow(keypadButtons_[static_cast<size_t>(i)], keyX[col], keyY[row],
               150, 44, TRUE);
  }
  MoveWindow(sendButton_, 40, 650, 560, 28, TRUE);

  const HWND echoGroup = FindWindowExW(hwnd_, nullptr, L"BUTTON", L"EchoLink");
  MoveWindow(echoGroup, 640, 78, 340, 150, TRUE);
  MoveWindow(echoState_, 660, 104, 300, 22, TRUE);
  MoveWindow(echoConnect_, 660, 136, 140, 36, TRUE);
  MoveWindow(echoDisconnect_, 820, 136, 140, 36, TRUE);
  MoveWindow(echoNode_, 660, 182, 170, 28, TRUE);
  MoveWindow(echoDial_, 842, 182, 118, 28, TRUE);

  const HWND discordGroup =
      FindWindowExW(hwnd_, nullptr, L"BUTTON", L"Discord");
  MoveWindow(discordGroup, 640, 242, 340, 130, TRUE);
  MoveWindow(discordState_, 660, 268, 300, 22, TRUE);
  MoveWindow(discordJoin_, 660, 300, 140, 36, TRUE);
  MoveWindow(discordLeave_, 820, 300, 140, 36, TRUE);

  const HWND bridgeGroup = FindWindowExW(hwnd_, nullptr, L"BUTTON", L"Bridge");
  MoveWindow(bridgeGroup, 640, 386, 340, 100, TRUE);
  MoveWindow(bridgeState_, 660, 416, 300, 22, TRUE);
  MoveWindow(voice_, 660, 446, 300, 22, TRUE);

  const HWND channelGroup =
      FindWindowExW(hwnd_, nullptr, L"BUTTON", L"Channel");
  MoveWindow(channelGroup, 640, 500, 340, 120, TRUE);
  MoveWindow(channelHint_, 660, 528, 300, 22, TRUE);
  MoveWindow(channelEdit_, 660, 558, 120, 28, TRUE);
  MoveWindow(channelApply_, 790, 558, 80, 28, TRUE);

  const HWND logGroup = FindWindowExW(hwnd_, nullptr, L"BUTTON", L"Live log");
  MoveWindow(logGroup, 20, 702, 960, 128, TRUE);
  MoveWindow(log_, 40, 726, 920, 90, TRUE);
}

void NativeGui::setText(HWND control, const std::string &text) {
  if (control != nullptr) {
    SetWindowTextW(control, wide(text).c_str());
  }
}

void NativeGui::setCommandDisplay() {
  const std::string text = commandText_.empty() ? "0#" : commandText_;
  SetWindowTextW(command_, wide(text).c_str());
}

void NativeGui::refresh() {
  const State state = state_();
  std::wstring windowTitle = L"BenshiNode Control";
  if (!state.callsign.empty()) {
    windowTitle += L" - " + wide(state.callsign);
  }
  SetWindowTextW(hwnd_, windowTitle.c_str());
  setText(callsign_, state.callsign.empty() ? "No callsign" : state.callsign);
  SetWindowTextW(connection_, state.connected ? L"Radio connected"
                                               : L"Radio offline");
  if (!state.connected) {
    SetWindowTextW(headline_, L"Waiting for radio");
    setText(detail_, "BenshiNode will reconnect automatically.");
  } else if (state.tx) {
    SetWindowTextW(headline_, L"Transmitting");
    setText(detail_, "The radio is on the air.");
  } else if (state.squelch || state.inRx) {
    SetWindowTextW(headline_, L"Receiving");
    setText(detail_, "Local RF activity.");
  } else {
    SetWindowTextW(headline_, L"Ready");
    setText(detail_, "Channel is quiet.");
  }
  if (state.channel >= 0) {
    setText(channel_, "Channel: " + std::to_string(state.channel) +
                          " (display " + std::to_string(state.channel + 1) +
                          ")");
  } else {
    SetWindowTextW(channel_, L"Channel: --");
  }
  setText(rssi_, "RSSI: " + std::to_string(state.rssi));
  setText(txrx_, std::string("TX: ") + (state.inTx ? "yes" : "no") +
                     "  RX: " + (state.inRx ? "yes" : "no"));
  setText(voice_, state.voice ? "Piper female voice: ready"
                              : "Piper female voice: disabled");

  SetWindowTextW(parrotButton_, state.parrot ? L"Parrot: On" : L"Parrot: Off");

  if (!state.echolinkConfigured) {
    SetWindowTextW(echoState_, L"Not configured");
  } else {
    SetWindowTextW(echoState_, state.echolink ? L"Connected" : L"Idle");
  }
  if (!state.discordConfigured) {
    SetWindowTextW(discordState_, L"Not configured");
  } else {
    SetWindowTextW(discordState_, state.discord ? L"Connected" : L"Idle");
  }
  setText(bridgeState_, state.linkAudio ? "RF/network audio active" : "Idle");
  if (state.linkConfigured && state.link) {
    setText(bridgeState_, "BenshiNode link connected");
  }

  EnableWindow(helpButton_, state.connected);
  EnableWindow(identButton_, state.connected);
  EnableWindow(parrotButton_, state.connected);
  EnableWindow(resetButton_, state.connected);
  EnableWindow(unkeyButton_, state.connected);
  EnableWindow(echoConnect_, state.connected && state.echolinkConfigured &&
                                  !state.echolink);
  EnableWindow(echoDisconnect_, state.connected && state.echolink);
  EnableWindow(echoDial_, state.connected && state.echolinkConfigured);
  EnableWindow(discordJoin_, state.connected && state.discordConfigured &&
                                  !state.discord);
  EnableWindow(discordLeave_, state.connected && state.discord);
  EnableWindow(sendButton_, state.connected);
  EnableWindow(channelEdit_, state.connected);
  EnableWindow(channelApply_, state.connected);
  for (HWND button : keypadButtons_) {
    EnableWindow(button, state.connected);
  }
  setCommandDisplay();
}

void NativeGui::refreshLogs() {
  std::vector<std::string> lines = logs_();
  if (lines == lastLogs_) {
    return;
  }
  lastLogs_ = lines;
  std::wstring text;
  for (const std::string &line : lines) {
    text += wide(line);
    text += L"\r\n";
  }
  SendMessageW(log_, WM_SETREDRAW, FALSE, 0);
  SetWindowTextW(log_, text.c_str());
  SendMessageW(log_, EM_SETSEL, static_cast<WPARAM>(text.size()),
               static_cast<LPARAM>(text.size()));
  SendMessageW(log_, EM_SCROLLCARET, 0, 0);
  SendMessageW(log_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(log_, nullptr, TRUE);
}

void NativeGui::onCommand(int id) {
  if (id >= ID_KEY_1 && id <= ID_KEY_9) {
    commandText_.push_back(static_cast<char>('1' + (id - ID_KEY_1)));
    setCommandDisplay();
    return;
  }
  switch (id) {
  case ID_KEY_0:
    commandText_.push_back('0');
    setCommandDisplay();
    break;
  case ID_KEY_HASH:
    commandText_.push_back('#');
    setCommandDisplay();
    break;
  case ID_KEY_CLEAR:
    commandText_.clear();
    setCommandDisplay();
    break;
  case ID_SEND: {
    const std::string code = commandText_.empty() ? "0#" : commandText_;
    action_({.type = "dtmf", .value = code});
    commandText_.clear();
    setCommandDisplay();
    break;
  }
  case ID_HELP:
    action_({.type = "help"});
    break;
  case ID_IDENT:
    action_({.type = "ident"});
    break;
  case ID_PARROT:
    action_({.type = "parrot"});
    break;
  case ID_RESET:
    action_({.type = "reset"});
    break;
  case ID_UNKEY:
    action_({.type = "unkey"});
    break;
  case ID_STOP:
    action_({.type = "shutdown"});
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    break;
  case ID_PAIR_RADIO: {
    const std::string mac = showRadioSetup(hwnd_, state_().mac);
    if (!mac.empty()) {
      std::string error;
      if (setIniValue(configPath_, "RADIO", "MAC", mac, error)) {
        MessageBoxW(hwnd_,
                    L"Radio saved. Restart BenshiNode to connect to it.",
                    L"BenshiNode", MB_ICONINFORMATION);
      } else {
        MessageBoxW(hwnd_, wide(error).c_str(), L"BenshiNode", MB_ICONERROR);
      }
    }
    break;
  }
  case ID_SETTINGS:
    showConfigEditor(hwnd_, configPath_);
    break;
  case ID_RESTART:
    action_({.type = "restart"});
    break;
  case ID_CHANNEL_APPLY: {
    wchar_t buffer[16]{};
    GetWindowTextW(channelEdit_, buffer, static_cast<int>(std::size(buffer)));
    action_({.type = "set_channel", .value = narrow(buffer)});
    break;
  }
  case ID_ECHO_CONNECT:
    action_({.type = "echolink_connect"});
    break;
  case ID_ECHO_DISCONNECT:
    action_({.type = "echolink_disconnect"});
    break;
  case ID_ECHO_DIAL: {
    wchar_t buffer[32]{};
    GetWindowTextW(echoNode_, buffer, static_cast<int>(std::size(buffer)));
    const std::string node = narrow(buffer);
    if (!node.empty() && std::all_of(node.begin(), node.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        })) {
      action_({.type = "echolink_dial", .value = node});
    }
    break;
  }
  case ID_DISCORD_JOIN:
    action_({.type = "discord_join"});
    break;
  case ID_DISCORD_LEAVE:
    action_({.type = "discord_leave"});
    break;
  default:
    break;
  }
}

} // namespace vr76::gui
