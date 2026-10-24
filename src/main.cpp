#include "Config.h"
#include "Logger.h"
#include "bluetooth/RfcommChannel.h"
#include "echolink/EchoLinkClient.h"
#include "link/LinkClient.h"
#include "link/LinkServer.h"
#include "link/LinkSocket.h"
#include "node/NodeEngine.h"
#include "node/ToneGenerator.h"
#include "radio/RadioSession.h"
#include "util/WavWriter.h"

#include <winrt/Windows.Foundation.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI consoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
      type == CTRL_CLOSE_EVENT) {
    g_stop.store(true);
    return TRUE;
  }
  return FALSE;
}

void printUsage() {
  std::cout
      << "BenshiNode - VR-N76 simplex node\n\n"
      << "Usage: BenshiNode [options]\n\n"
      << "  --config <file>      Configuration file (default: BenshiNode.ini)\n"
      << "  --list               List paired Bluetooth devices\n"
      << "  --probe              Connect and print radio information\n"
      << "  --dump               Print radio settings and APRS/BSS state\n"
      << "  --monitor [file.wav] Receive audio and optionally record it\n"
      << "  --test-tx            Transmit a short 1 kHz test tone\n"
      << "  --tap-id             Transmit the configured callsign in CW\n"
      << "  --unkey              Force the radio out of transmit\n"
      << "  --server             Run as a link server/reflector (no radio)\n"
      << "  --connect <host[:port]>  Link this node to a server\n"
      << "  --echolink <node>    Connect to an EchoLink node (explicit)\n"
      << "  --vfo-test           Try FREQ_MODE_SET_PAR flag variants\n"
      << "  --restore-aprs       Restore channel 31 to APRS at 144.390 MHz\n"
      << "  --verbose            Enable debug logging\n"
      << "  --help               Show this help\n";
}

double dbFromPcm(const std::vector<int16_t> &pcm) {
  if (pcm.empty()) {
    return -120.0;
  }
  double sum = 0.0;
  for (int16_t sample : pcm) {
    const double value = static_cast<double>(sample) / 32768.0;
    sum += value * value;
  }
  const double rms = std::sqrt(sum / static_cast<double>(pcm.size()));
  return rms <= 1e-9 ? -120.0 : 20.0 * std::log10(rms);
}

void waitWhile(bool condition, int milliseconds) {
  const auto end =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
  while (condition && !g_stop.load() &&
         std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

bool parseServerEndpoint(const std::string &text, std::string &host,
                         uint16_t &port) {
  if (text.empty()) {
    return false;
  }
  host = text;
  port = vr76::link::kDefaultControlPort;
  const auto colon = text.rfind(':');
  if (colon != std::string::npos && colon + 1 < text.size()) {
    host = text.substr(0, colon);
    try {
      const int parsed = std::stoi(text.substr(colon + 1));
      if (parsed <= 0 || parsed > 65535) {
        return false;
      }
      port = static_cast<uint16_t>(parsed);
    } catch (...) {
      return false;
    }
  }
  return !host.empty();
}

} // namespace

int main(int argc, char **argv) {
  std::string configPath = "BenshiNode.ini";
  bool list = false;
  bool probe = false;
  bool dump = false;
  bool monitor = false;
  bool testTx = false;
  bool tapId = false;
  bool unkey = false;
  bool vfoTest = false;
  bool restoreAprs = false;
  bool server = false;
  bool verbose = false;
  std::string monitorWav;
  std::string connectTarget;
  std::string echoLinkTarget;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      configPath = argv[++i];
    } else if (arg == "--list") {
      list = true;
    } else if (arg == "--probe") {
      probe = true;
    } else if (arg == "--dump") {
      dump = true;
    } else if (arg == "--monitor") {
      monitor = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        monitorWav = argv[++i];
      }
    } else if (arg == "--test-tx") {
      testTx = true;
    } else if (arg == "--tap-id") {
      tapId = true;
    } else if (arg == "--unkey") {
      unkey = true;
    } else if (arg == "--server") {
      server = true;
    } else if (arg == "--connect" && i + 1 < argc) {
      connectTarget = argv[++i];
    } else if (arg == "--echolink" && i + 1 < argc) {
      echoLinkTarget = argv[++i];
    } else if (arg == "--vfo-test") {
      vfoTest = true;
    } else if (arg == "--restore-aprs") {
      restoreAprs = true;
    } else if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      return 0;
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      printUsage();
      return 2;
    }
  }

  Logger logger(verbose ? LogLevel::Debug : LogLevel::Info);
  Config config;
  std::string error;
  const bool configLoaded = config.load(configPath, error);
  if (!configLoaded && !list) {
    logger.error(error);
    return 1;
  }

  if (configLoaded) {
    LogLevel configuredLevel = LogLevel::Info;
    if (parseLogLevel(config.getString("NODE", "LOG_LEVEL", "info"),
                      configuredLevel)) {
      if (!verbose) {
        logger.setLevel(configuredLevel);
      }
    }
    const std::string logFile = config.getString("NODE", "LOG_FILE", "");
    if (!logFile.empty() && !logger.openFile(logFile)) {
      logger.warning("Could not open log file: " + logFile);
    }
  }

  SetConsoleCtrlHandler(consoleHandler, TRUE);
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  if (server) {
    if (!connectTarget.empty()) {
      logger.error("--server and --connect cannot be used together");
      winrt::uninit_apartment();
      return 2;
    }
    vr76::link::LinkServer linkServer(logger, config);
    if (!linkServer.start(error)) {
      logger.error("Could not start link server: " + error);
      winrt::uninit_apartment();
      return 1;
    }
    logger.info("Server running. Press Ctrl+C to stop.");
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    linkServer.stop();
    vr76::link::socketCleanup();
    winrt::uninit_apartment();
    return 0;
  }

  if (list) {
    const auto devices = vr76::RfcommChannel::listPairedDevices(logger);
    if (devices.empty()) {
      std::cout << "No paired Bluetooth devices found.\n";
    } else {
      for (const auto &device : devices) {
        std::cout << device.address << "  " << device.name
                  << (device.radioCandidate ? "  [VR-N76 candidate]" : "")
                  << "\n";
      }
    }
    winrt::uninit_apartment();
    return 0;
  }

  vr76::RadioSession session(logger, config);

  if (unkey) {
    if (!session.connect(error)) {
      logger.error(error);
      winrt::uninit_apartment();
      return 1;
    }
    std::string unkeyError;
    if (!session.forceUnkey(unkeyError)) {
      logger.error(unkeyError);
    }
    session.disconnect();
    winrt::uninit_apartment();
    return 0;
  }

  if (probe || dump || vfoTest || restoreAprs) {
    if (!session.connect(error)) {
      logger.error(error);
      winrt::uninit_apartment();
      return 1;
    }
    if (restoreAprs) {
      std::vector<uint8_t> raw;
      std::string restoreError;
      if (!session.readChannelRecord(251, raw, restoreError)) {
        std::cerr << "Could not read APRS template channel 251: "
                  << restoreError << "\n";
      } else {
        raw[0] = 31;
        const uint32_t aprsFrequency = 144390000;
        raw[1] = static_cast<uint8_t>((aprsFrequency >> 24) & 0xFF);
        raw[2] = static_cast<uint8_t>((aprsFrequency >> 16) & 0xFF);
        raw[3] = static_cast<uint8_t>((aprsFrequency >> 8) & 0xFF);
        raw[4] = static_cast<uint8_t>(aprsFrequency & 0xFF);
        raw[5] = static_cast<uint8_t>((aprsFrequency >> 24) & 0xFF);
        raw[6] = static_cast<uint8_t>((aprsFrequency >> 16) & 0xFF);
        raw[7] = static_cast<uint8_t>((aprsFrequency >> 8) & 0xFF);
        raw[8] = static_cast<uint8_t>(aprsFrequency & 0xFF);
        raw[9] = 0;
        raw[10] = 0;
        raw[11] = 0;
        raw[12] = 0;
        for (size_t i = 0; i < 10; ++i) {
          raw[15 + i] = 0x20;
        }
        const char *aprsName = "APRS";
        for (size_t i = 0; i < 4; ++i) {
          raw[15 + i] = static_cast<uint8_t>(aprsName[i]);
        }
        if (!session.writeChannelRecord(raw, restoreError)) {
          std::cerr << "Could not restore channel 31: " << restoreError << "\n";
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(300));
          std::string selectError;
          session.setChannel(31, selectError);
          std::cout << "Restored channel 31 to APRS at 144.390 MHz\n";
        }
      }
      session.disconnect();
      winrt::uninit_apartment();
      return 0;
    }
    if (vfoTest) {
      const uint16_t variants[] = {0x0000, 0x0001, 0x0002, 0x0003,
                                   0x0103, 0x0A03, 0x8003, 0x0004};
      for (uint16_t flags : variants) {
        std::string vfoError;
        if (!session.setSimplexFrequencyWithFlags(145100000, 0.0, 0.0, flags,
                                                  vfoError)) {
          std::cout << "flags 0x" << std::hex << flags << std::dec
                    << ": set failed: " << vfoError << "\n";
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        uint32_t rx = 0;
        uint32_t tx = 0;
        uint16_t resultFlags = 0;
        if (session.getVfoFrequency(rx, tx, resultFlags, vfoError)) {
          std::cout << "flags 0x" << std::hex << flags
                    << " -> status flags 0x" << resultFlags << std::dec
                    << ", RX " << rx / 1000000.0 << " MHz\n";
        } else {
          std::cout << "flags 0x" << std::hex << flags << std::dec
                    << ": status failed: " << vfoError << "\n";
        }
      }
    }
    if (probe) {
      const auto &info = session.deviceInfo();
      const auto &status = session.status();
      std::cout << "Connected to " << session.macAddress() << "\n"
                << "  product: " << info.productId << "\n"
                << "  hardware: " << static_cast<unsigned>(info.hardwareVersion)
                << "  software: " << info.softwareVersion << "\n"
                << "  channels: " << info.channelCount
                << "  regions: " << info.regionCount << "\n"
                << "  power: " << (status.powerOn ? "on" : "off")
                << "  channel: " << status.channelId
                << "  rssi: " << status.rssi
                << "  rx: " << (status.inRx ? "yes" : "no")
                << "  tx: " << (status.inTx ? "yes" : "no") << "\n";
    }
    std::string configuration;
    std::string dumpError;
    if (session.dumpConfiguration(configuration, dumpError)) {
      std::cout << configuration;
    } else {
      std::cerr << "Could not read radio configuration: " << dumpError << "\n";
    }
    session.disconnect();
    winrt::uninit_apartment();
    return 0;
  }

  vr76::WavWriter recorder;
  if (monitor && !monitorWav.empty() && !recorder.open(monitorWav)) {
    logger.warning("Could not open monitor WAV file: " + monitorWav);
  }

  std::unique_ptr<vr76::NodeEngine> node;
  std::unique_ptr<vr76::link::LinkClient> link;
  std::unique_ptr<vr76::echolink::EchoLinkClient> echoLink;
  std::string linkHost;
  uint16_t linkPort = vr76::link::kDefaultControlPort;
  std::string linkKey;
  bool linkRequested = false;
  std::string echoTarget = echoLinkTarget;
  bool echoAuto = !echoTarget.empty();
  const bool echoDtmf = config.getBool("ECHOLINK", "DTMF", false);

  if (!monitor && !testTx && !tapId) {
    std::string linkServer = connectTarget;
    if (linkServer.empty() && config.getBool("LINK", "ENABLED", false)) {
      linkServer = config.getString("LINK", "SERVER", "");
    }
    if (!linkServer.empty()) {
      if (!parseServerEndpoint(linkServer, linkHost, linkPort)) {
        logger.error("Invalid link server address: " + linkServer);
        winrt::uninit_apartment();
        return 2;
      }
      linkKey = config.getString("LINK", "KEY", "");
      linkRequested = true;
    }

    if (!echoAuto && config.getBool("ECHOLINK", "ENABLED", false)) {
      echoTarget = config.getString("ECHOLINK", "NODE", "");
      echoAuto = !echoTarget.empty();
    }
    if (echoAuto && echoTarget.empty()) {
      logger.error("ECHOLINK/NODE is empty");
      winrt::uninit_apartment();
      return 2;
    }
    if (echoDtmf && echoTarget.empty()) {
      echoTarget = config.getString("ECHOLINK", "NODE", "");
      if (echoTarget.empty()) {
        logger.error("ECHOLINK/NODE is required for DTMF EchoLink control");
        winrt::uninit_apartment();
        return 2;
      }
    }
    const bool echoCapable = echoAuto || echoDtmf;
    if (linkRequested && echoCapable) {
      logger.error("LINK and ECHOLINK cannot be enabled at the same time");
      winrt::uninit_apartment();
      return 2;
    }
    if (echoCapable && config.getString("ECHOLINK", "IP", "").empty() &&
        config.getString("ECHOLINK", "PASSWORD", "").empty()) {
      logger.error("ECHOLINK/PASSWORD is required before connecting");
      winrt::uninit_apartment();
      return 2;
    }

    node = std::make_unique<vr76::NodeEngine>(logger, config, session);
    session.setStatusCallback(
        [&](const vr76::RadioStatus &status) { node->onRadioStatus(status); });
    session.setAudioPcmCallback(
        [&](const std::vector<int16_t> &pcm, bool transmit) {
          node->onAudioPcm(pcm, transmit);
        });
    session.setAudioEndCallback(
        [&](bool transmit) { node->onAudioEnd(transmit); });

    if (linkRequested) {
      link = std::make_unique<vr76::link::LinkClient>(logger, *node);
    }
    if (echoCapable) {
      echoLink =
          std::make_unique<vr76::echolink::EchoLinkClient>(logger, config, *node);
      node->setEchoLinkControl(
          [&](bool connect, const std::string &target) {
            if (!echoLink) {
              return;
            }
            if (connect) {
              const std::string dialTarget =
                  target.empty() ? echoTarget : target;
              std::string echoError;
              if (!echoLink->start(dialTarget, echoError)) {
                logger.error("EchoLink connect failed: " + echoError);
              }
            } else {
              echoLink->stop();
              logger.info("EchoLink disconnected");
            }
          });
    }
    node->start();
    if (link) {
      std::string linkError;
      if (!link->start(linkHost, linkPort, linkKey,
                       config.getString("NODE", "CALLSIGN", ""), linkError)) {
        logger.error("Could not start link client: " + linkError);
      }
    }
    if (echoAuto && echoLink) {
      std::string echoError;
      if (!echoLink->start(echoTarget, echoError)) {
        logger.error("Could not start EchoLink client: " + echoError);
      }
    }
    if (echoDtmf && !echoAuto) {
      logger.info("EchoLink idle. Send *4# to connect, *5# to disconnect.");
    }
  } else {
    session.setStatusCallback([&](const vr76::RadioStatus &status) {
      logger.info("Radio status: power=" + std::string(status.powerOn ? "on" : "off") +
                  " rx=" + (status.inRx ? "1" : "0") +
                  " tx=" + (status.inTx ? "1" : "0") +
                  " sq=" + (status.squelchOpen ? "1" : "0") +
                  " rssi=" + std::to_string(status.rssi) +
                  " ch=" + std::to_string(status.channelId));
    });
    session.setAudioPcmCallback(
        [&](const std::vector<int16_t> &pcm, bool transmit) {
          if (transmit) {
            return;
          }
          const double db = dbFromPcm(pcm);
          logger.info("RX audio " + std::to_string(pcm.size()) +
                      " samples, " + std::to_string(static_cast<int>(db)) +
                      " dBFS");
          if (recorder.isOpen()) {
            recorder.write(pcm);
          }
        });
    session.setAudioEndCallback(
        [&](bool transmit) {
          if (!transmit) {
            logger.info("RX audio run ended");
          }
        });
  }

  session.setDisconnectCallback(
      []() { std::cout << "Radio disconnected\n"; });

  const int retryMs = std::max(500, config.getInt("RADIO", "CONNECT_RETRY_MS", 5000));

  while (!g_stop.load()) {
    if (!session.connect(error)) {
      logger.error(error);
      waitWhile(true, retryMs);
      continue;
    }

    if (testTx) {
      std::string txError;
      if (!session.transmitTestTone(1000.0, 2000, txError)) {
        logger.error("Test tone failed: " + txError);
      } else {
        logger.info("Test tone complete");
      }
      std::string unkeyError;
      if (!session.forceUnkey(unkeyError)) {
        logger.error("Failed to unkey radio: " + unkeyError);
      }
      session.disconnect();
      break;
    }

    if (tapId) {
      const std::string callsign = config.getString("NODE", "CALLSIGN", "");
      if (callsign.empty()) {
        logger.error("NODE/CALLSIGN is empty; cannot send CW ID");
        session.disconnect();
        break;
      }
      const int wpm = std::max(5, config.getInt("NODE", "CW_WPM", 20));
      const double toneHz = config.getInt("NODE", "CW_TONE_HZ", 700);
      const auto cw = vr76::ToneGenerator::cw(callsign, wpm, toneHz, -12.0);
      std::vector<int16_t> pcm;
      const auto lead = vr76::ToneGenerator::silence(250);
      const auto tail = vr76::ToneGenerator::silence(400);
      pcm.reserve(lead.size() + cw.size() + tail.size());
      pcm.insert(pcm.end(), lead.begin(), lead.end());
      pcm.insert(pcm.end(), cw.begin(), cw.end());
      pcm.insert(pcm.end(), tail.begin(), tail.end());

      const int channelId = config.getInt("RADIO", "CHANNEL", -1);
      logger.info("Sending CW ID " + callsign + " on channel " +
                  (channelId >= 0 ? std::to_string(channelId) +
                                        " (radio display " +
                                        std::to_string(channelId + 1) + ")"
                                  : std::string("(current)")));
      std::string txError;
      if (!session.transmitPcm(pcm, txError)) {
        logger.error("CW ID transmit failed: " + txError);
      } else {
        logger.info("CW ID complete");
      }
      std::string unkeyError;
      if (!session.forceUnkey(unkeyError)) {
        logger.error("Failed to unkey radio: " + unkeyError);
      }
      session.disconnect();
      break;
    }

    if (monitor) {
      logger.info("Monitoring receive audio. Press Ctrl+C to stop.");
      while (!g_stop.load() && session.isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } else {
      logger.info("Node running. Press Ctrl+C to stop.");
      while (!g_stop.load() && session.isConnected()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    session.disconnect();
    if (!g_stop.load()) {
      logger.warning("Connection lost; reconnecting in " +
                     std::to_string(retryMs) + " ms");
      waitWhile(true, retryMs);
    }
  }

  if (echoLink) {
    echoLink->stop();
  }
  if (link) {
    link->stop();
  }
  if (node) {
    node->stop();
  }
  session.disconnect();
  recorder.close();
  if (link || echoLink) {
    vr76::link::socketCleanup();
  }
  winrt::uninit_apartment();
  return 0;
}
