import 'dart:io';

import 'package:path_provider/path_provider.dart';

import 'dtmf_commands.dart';

class AppConfig {
  AppConfig._(this.path, this._sections);

  final String path;
  final Map<String, Map<String, String>> _sections;

  static const _template = '''
[RADIO]
MAC=
CHANNEL=
STATUS_POLL_MS=500
CONNECT_RETRY_MS=5000
SBC_BITPOOL=40

[NODE]
CALLSIGN=N0CALL
DTMF_PREFIX=
DTMF_SUFFIX=#
PARROT_MODE=dtmf
PARROT_MAX_SEC=60
PARROT_DELAY_MS=800
ID_INTERVAL_SEC=600
ID_MIN_GAP_SEC=120
CW_WPM=20
CW_TONE_HZ=700
WAV_DIR=clips
COURTESY_TONE=0
TX_TIMEOUT_SEC=180
TX_LEAD_SILENCE_MS=100
TX_TAIL_SILENCE_MS=200
SQUELCH_MODE=radio
VOX_THRESHOLD_DB=-38
VOX_HANG_MS=600
RX_GAIN_DB=0
TX_GAIN_DB=0
LOG_LEVEL=info

[PIPER]
ENABLED=1
STARTUP_PROMPT=1
TTS_RATE=0.4

[DTMF]
0=HELP
1=PARROT
2=LINK
3=IDENT
4=ECHOLINK
5=UNLINK
6=DISCORD
7=DISCORD_LEAVE
8=DISCONNECT_ALL
9=RESET

[LINK]
ENABLED=0
SERVER=
KEY=

[ECHOLINK]
ENABLED=0
DTMF=0
CALLSIGN=
PASSWORD=
NAME=
INFO=BenshiNode
NODE=
DIRECTORY=servers.echolink.org:5200
PORT_BASE=5198
''';

  static Future<AppConfig> load() async {
    final directory = await getApplicationDocumentsDirectory();
    final file = File('${directory.path}${Platform.pathSeparator}BenshiNode.ini');
    if (!file.existsSync()) {
      await file.writeAsString(_template);
    }
    return loadFromFile(file.path);
  }

  static Future<AppConfig> loadFromFile(String path) async {
    final file = File(path);
    final text = await file.readAsString();
    return AppConfig._(path, _parse(text));
  }

  static Map<String, Map<String, String>> _parse(String text) {
    final sections = <String, Map<String, String>>{};
    var current = sections.putIfAbsent('', () => {});
    for (final rawLine in text.split('\n')) {
      var line = rawLine.trim();
      if (line.isEmpty || line.startsWith(';') || line.startsWith('#')) {
        continue;
      }
      if (line.startsWith('[') && line.endsWith(']')) {
        final name = line.substring(1, line.length - 1).trim().toUpperCase();
        current = sections.putIfAbsent(name, () => {});
        continue;
      }
      final comment = line.indexOf(';');
      if (comment >= 0) {
        line = line.substring(0, comment).trim();
      }
      final equals = line.indexOf('=');
      if (equals <= 0) {
        continue;
      }
      final key = line.substring(0, equals).trim().toUpperCase();
      var value = line.substring(equals + 1).trim();
      if (value.length >= 2 &&
          ((value.startsWith('"') && value.endsWith('"')) ||
              (value.startsWith("'") && value.endsWith("'")))) {
        value = value.substring(1, value.length - 1);
      }
      current[key] = value;
    }
    return sections;
  }

  String getString(String section, String key, [String fallback = '']) {
    return _sections[section.toUpperCase()]?[key.toUpperCase()] ?? fallback;
  }

  int getInt(String section, String key, [int fallback = 0]) {
    return int.tryParse(getString(section, key)) ?? fallback;
  }

  double getDouble(String section, String key, [double fallback = 0]) {
    return double.tryParse(getString(section, key)) ?? fallback;
  }

  bool getBool(String section, String key, [bool fallback = false]) {
    final value = getString(section, key).toLowerCase();
    if (value.isEmpty) {
      return fallback;
    }
    return value == '1' || value == 'true' || value == 'yes' || value == 'on';
  }

  void set(String section, String key, String value) {
    _sections
        .putIfAbsent(section.toUpperCase(), () => {})[key.toUpperCase()] =
        value;
  }

  Future<void> save() async {
    final buffer = StringBuffer();
    final keys = _sections.keys.toList()
      ..sort((a, b) {
        if (a.isEmpty) return -1;
        if (b.isEmpty) return 1;
        return a.compareTo(b);
      });
    for (final section in keys) {
      final values = _sections[section]!;
      if (section.isNotEmpty) {
        buffer.writeln('[$section]');
      }
      for (final entry in values.entries) {
        buffer.writeln('${entry.key}=${entry.value}');
      }
      buffer.writeln();
    }
    await File(path).writeAsString(buffer.toString());
  }

  String get callsign => getString('NODE', 'CALLSIGN', 'N0CALL');
  String get mac => getString('RADIO', 'MAC');
  int get channel => int.tryParse(getString('RADIO', 'CHANNEL')) ?? -1;
  int get statusPollMs => getInt('RADIO', 'STATUS_POLL_MS', 500);
  int get connectRetryMs => getInt('RADIO', 'CONNECT_RETRY_MS', 5000);
  int get sbcBitpool => getInt('RADIO', 'SBC_BITPOOL', 40).clamp(16, 53);
  bool get ttsEnabled => getBool('PIPER', 'ENABLED', true);
  bool get startupPrompt => getBool('PIPER', 'STARTUP_PROMPT', true);
  double get ttsRate =>
      getDouble('PIPER', 'TTS_RATE', 0.4).clamp(0.2, 0.6).toDouble();
  bool get linkEnabled => getBool('LINK', 'ENABLED', false);
  String get linkServer => getString('LINK', 'SERVER');
  String get linkKey => getString('LINK', 'KEY');
  bool get echoLinkEnabled => getBool('ECHOLINK', 'ENABLED', false);
  bool get echoLinkDtmf => getBool('ECHOLINK', 'DTMF', false);
  String get echoLinkCallsign => getString('ECHOLINK', 'CALLSIGN');
  String get echoLinkPassword => getString('ECHOLINK', 'PASSWORD');
  String get echoLinkName => getString('ECHOLINK', 'NAME');
  String get echoLinkInfo => getString('ECHOLINK', 'INFO', 'BenshiNode');
  String get echoLinkNode => getString('ECHOLINK', 'NODE');
  String get echoLinkDirectory =>
      getString('ECHOLINK', 'DIRECTORY', 'servers.echolink.org:5200');
  int get echoLinkPortBase => getInt('ECHOLINK', 'PORT_BASE', 5198);

  Map<String, NodeCommand> dtmfCommands() {
    final prefix = getString('NODE', 'DTMF_PREFIX');
    final suffix = getString('NODE', 'DTMF_SUFFIX', '#');
    const defaults = {
      '0': NodeCommand.help,
      '1': NodeCommand.parrotToggle,
      '2': NodeCommand.linkConnect,
      '3': NodeCommand.ident,
      '4': NodeCommand.echoLinkConnect,
      '5': NodeCommand.echoLinkDisconnect,
      '6': NodeCommand.discordJoin,
      '7': NodeCommand.discordLeave,
      '8': NodeCommand.disconnectAll,
      '9': NodeCommand.reset,
    };
    final commands = <String, NodeCommand>{};
    for (final entry in defaults.entries) {
      final key = '$prefix${entry.key}$suffix';
      final override = getString('DTMF', entry.key).isEmpty
          ? getString('DTMF', key)
          : getString('DTMF', entry.key);
      commands[key] =
          override.isEmpty ? entry.value : (parseNodeCommand(override) ?? entry.value);
    }
    return commands;
  }
}
