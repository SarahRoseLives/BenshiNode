#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vr76::gui {

struct State {
  std::string callsign;
  std::string mac;
  bool connected = false;
  bool radioReady = false;
  bool powerOn = false;
  bool inTx = false;
  bool inRx = false;
  bool squelch = false;
  bool tx = false;
  bool parrot = false;
  bool linkAudio = false;
  bool discord = false;
  bool discordConfigured = false;
  bool echolink = false;
  bool echolinkConfigured = false;
  bool link = false;
  bool linkConfigured = false;
  std::string linkServer;
  std::string linkKey;
  bool voice = false;
  int channel = -1;
  int rssi = 0;
};

struct Action {
  std::string type;
  std::string value;
  std::string extra;
};

class NativeGui {
public:
  using StateProvider = std::function<State()>;
  using LogProvider = std::function<std::vector<std::string>()>;
  using ActionHandler = std::function<void(const Action &)>;
  using CloseHandler = std::function<void()>;

  NativeGui(StateProvider state, LogProvider logs, ActionHandler action,
            CloseHandler close, std::string configPath);
  ~NativeGui();

  bool start(std::string &error);
  void stop();
  bool closed() const { return closed_.load(); }

private:
  void threadMain();
  void createControls();
  void layoutControls();
  void refresh();
  void refreshLogs();
  void onCommand(int id);
  void setText(HWND control, const std::string &text);
  void setCommandDisplay();
  void notifyStarted(const std::string &error);

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  StateProvider state_;
  LogProvider logs_;
  ActionHandler action_;
  CloseHandler close_;
  std::string configPath_;

  HWND hwnd_ = nullptr;
  std::thread thread_;
  std::mutex startMutex_;
  std::condition_variable startCv_;
  bool started_ = false;
  std::string startError_;
  std::atomic<bool> closed_{false};

  HFONT font_ = nullptr;
  HFONT titleFont_ = nullptr;
  HFONT logFont_ = nullptr;

  HWND title_ = nullptr;
  HWND callsign_ = nullptr;
  HWND connection_ = nullptr;
  HWND headline_ = nullptr;
  HWND detail_ = nullptr;
  HWND rssi_ = nullptr;
  HWND txrx_ = nullptr;
  HWND voice_ = nullptr;
  HWND command_ = nullptr;
  HWND echoNode_ = nullptr;
  HWND echoState_ = nullptr;
  HWND discordState_ = nullptr;
  HWND bridgeState_ = nullptr;
  HWND channelEdit_ = nullptr;
  HWND channelApply_ = nullptr;
  HWND channelHint_ = nullptr;
  HWND linkServer_ = nullptr;
  HWND linkKey_ = nullptr;
  HWND linkConnect_ = nullptr;
  HWND linkDisconnect_ = nullptr;
  HWND linkServerLabel_ = nullptr;
  HWND linkKeyLabel_ = nullptr;
  bool linkFieldsInitialized_ = false;
  HWND log_ = nullptr;

  HWND pairButton_ = nullptr;
  HWND settingsButton_ = nullptr;
  HWND restartButton_ = nullptr;
  HWND helpButton_ = nullptr;
  HWND identButton_ = nullptr;
  HWND parrotButton_ = nullptr;
  HWND resetButton_ = nullptr;
  HWND unkeyButton_ = nullptr;
  HWND stopButton_ = nullptr;
  HWND echoConnect_ = nullptr;
  HWND echoDisconnect_ = nullptr;
  HWND echoDial_ = nullptr;
  HWND discordJoin_ = nullptr;
  HWND discordLeave_ = nullptr;
  HWND sendButton_ = nullptr;
  std::vector<HWND> keypadButtons_;

  std::string commandText_;
  std::vector<std::string> lastLogs_;
  bool dialogOpen_ = false;
};

} // namespace vr76::gui
