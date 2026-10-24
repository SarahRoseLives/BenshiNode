#include "Config.h"
#include "echolink/EchoLinkProtocol.h"
#include "link/G711.h"
#include "link/LinkProtocol.h"
#include "link/Resampler.h"
#include "node/DtmfCommands.h"
#include "node/DtmfDecoder.h"
#include "node/ToneGenerator.h"
#include "radio/AudioFraming.h"
#include "radio/GaiaProtocol.h"
#include "radio/SbcCodec.h"

#include <cmath>
#include <cstdio>

extern "C" {
#include <gsm.h>
}
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    ++failures;
  }
}

void testConfig() {
  const auto path =
      std::filesystem::temp_directory_path() / "BenshiNode_core_test.ini";
  {
    std::ofstream out(path);
    out << "[NODE]\nCALLSIGN=N0CALL\nPARROT_MODE=dtmf\nTX_TIMEOUT_SEC=120\n"
        << "COURTESY_TONE=1\n[DTMF]\n*1=PARROT\n";
  }

  Config config;
  std::string error;
  check(config.load(path.string(), error), "config loads");
  check(config.getString("NODE", "CALLSIGN") == "N0CALL", "config callsign");
  check(config.getInt("NODE", "TX_TIMEOUT_SEC", 0) == 120, "config int");
  check(config.getBool("NODE", "COURTESY_TONE", false), "config bool");
  check(config.getString("DTMF", "*1") == "PARROT", "config dtmf key");
  std::filesystem::remove(path);
}

void testGaia() {
  const auto command =
      gaia::buildCommandByte(gaia::kGroupBasic,
                             static_cast<uint16_t>(gaia::Command::GetHtStatus),
                             0x42);
  check(command.size() == 5, "gaia command size");
  check(command[0] == 0x00 && command[1] == 0x02, "gaia group");
  check(command[2] == 0x00 && command[3] == 0x14, "gaia command");

  const auto framed = gaia::encode(command);
  check(framed.size() == 9, "gaia frame size");
  check(framed[0] == 0xFF && framed[1] == 0x01, "gaia frame sync");
  check(framed[3] == 1, "gaia payload length");

  gaia::Decoder decoder;
  decoder.push(framed.data(), framed.size());
  gaia::Frame frame;
  check(decoder.next(frame), "gaia decode");
  check(frame.group == gaia::kGroupBasic, "gaia decoded group");
  check(frame.command == 20, "gaia decoded command");
  check(!frame.isResponse, "gaia request bit");
  check(frame.payload.size() == 1 && frame.payload[0] == 0x42,
        "gaia payload");

  const auto reply = gaia::buildCommand(
      gaia::kGroupBasic,
      static_cast<uint16_t>(0x8000 | static_cast<uint16_t>(gaia::Command::GetHtStatus)),
      {0x00, 0x80, 0x00, 0x00, 0x00});
  const auto framedReply = gaia::encode(reply);
  decoder.push(framedReply.data(), framedReply.size());
  check(decoder.next(frame), "gaia reply decode");
  check(frame.isResponse, "gaia reply bit");
  check(frame.command == 20, "gaia reply command");
  check(frame.payload.size() == 5, "gaia reply payload");
}

void testAudioFraming() {
  const std::vector<uint8_t> payload = {0x01, 0x7E, 0x02, 0x7D, 0x03, 0x00};
  const auto framed = vr76::frameAudio(0x00, payload);
  check(framed.front() == 0x7E && framed.back() == 0x7E, "audio frame markers");

  vr76::AudioFrameParser parser;
  parser.push(framed.data(), framed.size());
  std::vector<uint8_t> parsed;
  check(parser.next(parsed), "audio frame parse");
  check(parsed.size() == payload.size() + 1, "audio frame size");
  check(parsed[0] == 0x00, "audio frame command");
  for (size_t i = 0; i < payload.size(); ++i) {
    check(parsed[i + 1] == payload[i], "audio frame escaped payload");
  }

  const auto end = vr76::endAudioFrame();
  check(end.size() == 11 && end.front() == 0x7E && end.back() == 0x7E,
        "audio end frame");
}

void testSbc() {
  vr76::SbcCodec codec;
  codec.setBitpool(40);
  std::vector<int16_t> samples(vr76::SbcCodec::kSamplesPerFrame);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<int16_t>(
        9000.0 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 32000.0));
  }

  std::vector<uint8_t> encoded;
  check(codec.encode(samples.data(), samples.size(), encoded), "sbc encode");
  check(encoded.size() > 4 && encoded[0] == 0x9C, "sbc sync word");

  std::vector<int16_t> decoded;
  check(codec.decode(encoded.data(), encoded.size(), decoded), "sbc decode");
  check(decoded.size() == samples.size(), "sbc sample count");

  double sum = 0.0;
  for (int16_t sample : decoded) {
    sum += static_cast<double>(sample) * sample;
  }
  const double rms = std::sqrt(sum / decoded.size());
  check(rms > 500.0, "sbc decoded signal present");
}

double toneAmplitude(const std::vector<int16_t> &samples, double frequency,
                     int sampleRate = 32000) {
  const double coeff = 2.0 * std::cos(2.0 * 3.141592653589793 * frequency /
                                     static_cast<double>(sampleRate));
  double q0 = 0.0;
  double q1 = 0.0;
  double q2 = 0.0;
  for (int16_t sample : samples) {
    q0 = coeff * q1 - q2 + static_cast<double>(sample);
    q2 = q1;
    q1 = q0;
  }
  const double power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
  return 2.0 * std::sqrt(power > 0.0 ? power : 0.0) /
         static_cast<double>(samples.size());
}

void testCwSbc() {
  const auto cw = vr76::ToneGenerator::cw("N0CALL", 20, 700.0, -12.0);
  vr76::SbcCodec codec;
  codec.setBitpool(18);

  std::vector<uint8_t> stream;
  std::vector<int16_t> block(vr76::SbcCodec::kSamplesPerFrame, 0);
  size_t offset = 0;
  while (offset < cw.size()) {
    std::fill(block.begin(), block.end(), static_cast<int16_t>(0));
    const size_t count =
        std::min(block.size(), cw.size() - offset);
    std::copy(cw.begin() + static_cast<std::ptrdiff_t>(offset),
              cw.begin() + static_cast<std::ptrdiff_t>(offset + count),
              block.begin());
    std::vector<uint8_t> encoded;
    check(codec.encode(block.data(), block.size(), encoded),
          "cw sbc encode");
    stream.insert(stream.end(), encoded.begin(), encoded.end());
    offset += count;
  }

  std::vector<int16_t> decoded;
  check(codec.decode(stream.data(), stream.size(), decoded),
        "cw sbc decode");
  const double tone = toneAmplitude(decoded, 700.0);
  const double other = toneAmplitude(decoded, 1750.0);
  std::cerr << "cw sbc tone 700Hz=" << tone << " 1750Hz=" << other << "\n";
  check(tone > 1000.0, "cw sbc tone amplitude");
  check(tone > other * 4.0, "cw sbc tone purity");
}

void testDtmf() {
  const auto pcm = vr76::ToneGenerator::dtmf("123A", 100, 50, -12.0);
  vr76::DtmfDecoder decoder;
  std::string digits;
  decoder.process(pcm.data(), pcm.size(),
                  [&](char digit, int) { digits.push_back(digit); });
  if (digits != "123A") {
    std::cerr << "  dtmf decoded: '" << digits << "'\n";
  }
  check(digits == "123A", "dtmf decode sequence");
}

void testLinkAudio() {
  std::vector<int16_t> input(160);
  for (size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<int16_t>(
        8000.0 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 8000.0));
  }

  std::vector<int16_t> encoded(input.size());
  std::vector<int16_t> decoded(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    encoded[i] = vr76::linearToUlaw(input[i]);
    decoded[i] = vr76::ulawToLinear(static_cast<uint8_t>(encoded[i]));
  }
  const double tone = toneAmplitude(decoded, 1000.0, 8000);
  check(tone > 3000.0, "g711 tone amplitude");

  vr76::Downsampler32to8 down;
  vr76::Upsampler8to32 up;
  std::vector<int16_t> pcm32(640);
  for (size_t i = 0; i < pcm32.size(); ++i) {
    pcm32[i] = static_cast<int16_t>(
        8000.0 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 32000.0));
  }
  std::vector<int16_t> pcm8;
  down.process(pcm32.data(), pcm32.size(), pcm8);
  check(pcm8.size() == 160, "downsampler size");
  std::vector<int16_t> back;
  up.process(pcm8.data(), pcm8.size(), back);
  check(back.size() == 640, "upsampler size");
  check(toneAmplitude(back, 1000.0) > 3000.0, "resampler tone");

  vr76::link::AudioPacket packet;
  packet.type = vr76::link::PacketType::Audio;
  packet.flags = vr76::link::FlagStart;
  packet.session = 42;
  packet.sequence = 7;
  packet.payload = {1, 2, 3, 4, 5};
  const auto data = vr76::link::encodeAudioPacket(packet);
  vr76::link::AudioPacket decodedPacket;
  check(vr76::link::decodeAudioPacket(data.data(), data.size(), decodedPacket),
        "link packet decode");
  check(decodedPacket.session == 42 && decodedPacket.sequence == 7,
        "link packet fields");
  check(decodedPacket.payload == packet.payload, "link packet payload");
}

void testEchoLinkProtocol() {
  const auto sdes = vr76::echolink::makeSdesPacket("N0CALL", "N0CALL", "");
  check(vr76::echolink::isRtcpSdes(sdes.data(), sdes.size()),
        "echolink sdes packet");
  check(!vr76::echolink::isRtcpBye(sdes.data(), sdes.size()),
        "echolink sdes is not bye");
  std::string name;
  check(vr76::echolink::parseSdesItem(sdes.data(), sdes.size(), 2, name),
        "echolink parse name");
  check(name.find("N0CALL") != std::string::npos, "echolink name content");

  const auto bye = vr76::echolink::makeByePacket();
  check(vr76::echolink::isRtcpBye(bye.data(), bye.size()),
        "echolink bye packet");

  const uint8_t payload[132] = {};
  const auto voice = vr76::echolink::makeVoicePacket(42, payload, 132);
  const uint8_t *frames = nullptr;
  size_t frameBytes = 0;
  check(vr76::echolink::parseVoicePacket(voice.data(), voice.size(), frames,
                                         frameBytes),
        "echolink voice packet");
  check(frameBytes == 132, "echolink voice payload size");

  const std::string list =
      "@@@\n2\n"
      "*TEST*\nConference [ON 12:00]\n179729\n203.0.113.10\n"
      "N0CALL\nTest station\n12345\n203.0.113.11\n"
      "+++\n";
  std::vector<vr76::echolink::Station> stations;
  check(vr76::echolink::parseDirectoryList(list, stations),
        "echolink directory parse");
  check(stations.size() == 2, "echolink directory station count");
  vr76::echolink::Station found;
  check(vr76::echolink::findStation(stations, "179729", found),
        "echolink find by node id");
  check(found.callsign == "*TEST*", "echolink node id match");
  check(vr76::echolink::findStation(stations, "k8sdr", found),
        "echolink find by callsign");
}

void testGsmCodec() {
  gsm encoder = gsm_create();
  gsm decoder = gsm_create();
  check(encoder != nullptr && decoder != nullptr, "gsm codec create");

  gsm_signal input[160] = {};
  for (int i = 0; i < 160; ++i) {
    input[i] = static_cast<gsm_signal>(
        8000.0 * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 8000.0));
  }
  gsm_byte frame[33] = {};
  gsm_encode(encoder, input, frame);

  gsm_signal output[160] = {};
  check(gsm_decode(decoder, frame, output) == 0, "gsm decode");
  double sum = 0.0;
  for (int i = 0; i < 160; ++i) {
    sum += static_cast<double>(output[i]) * output[i];
  }
  check(std::sqrt(sum / 160.0) > 500.0, "gsm decoded signal");

  gsm_destroy(encoder);
  gsm_destroy(decoder);
}

void testCommands() {
  std::map<std::string, vr76::NodeCommand> commands;
  commands["*1#"] = vr76::NodeCommand::ParrotToggle;
  commands["*2#"] = vr76::NodeCommand::Help;
  vr76::DtmfCommandParser parser;
  parser.setCommands(commands);

  vr76::NodeCommand command = vr76::NodeCommand::None;
  std::string code;
  check(!parser.feed('*', command, code), "dtmf prefix not complete");
  check(!parser.feed('1', command, code), "dtmf digit not complete");
  check(parser.feed('#', command, code), "dtmf command complete");
  check(code == "*1#", "dtmf command code");
  check(command == vr76::NodeCommand::ParrotToggle, "dtmf command value");

  check(parser.feed('*', command, code) == false, "dtmf restart prefix");
  check(parser.feed('9', command, code) == false, "dtmf unknown digit");
  check(parser.feed('#', command, code), "dtmf unknown complete");
  check(command == vr76::NodeCommand::None, "dtmf unknown yields none");

  parser.setDialPrefix("*4");
  const std::string dial = "*4179729#";
  for (size_t i = 0; i + 1 < dial.size(); ++i) {
    check(!parser.feed(dial[i], command, code), "dtmf dial in progress");
  }
  check(parser.feed('#', command, code), "dtmf dial complete");
  check(command == vr76::NodeCommand::EchoLinkDial, "dtmf dial command");
  check(code == "179729", "dtmf dial address");
}

} // namespace

int main() {
  testConfig();
  testGaia();
  testAudioFraming();
  testSbc();
  testCwSbc();
  testDtmf();
  testLinkAudio();
  testEchoLinkProtocol();
  testGsmCodec();
  testCommands();

  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "All core tests passed\n";
  return 0;
}
