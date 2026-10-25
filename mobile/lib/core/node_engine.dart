import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter/foundation.dart';

import 'app_config.dart';
import 'dtmf_commands.dart';
import 'dtmf_decoder.dart';
import 'dsp.dart';
import 'logger.dart';
import 'radio_session.dart';
import 'tone_generator.dart';
import 'tts_service.dart';

class TxClip {
  TxClip(this.pcm, this.name, this.notBefore, {this.allowDrop = true});

  final Int16List pcm;
  final String name;
  final DateTime notBefore;
  final bool allowDrop;
}

class NodeEngine extends ChangeNotifier {
  NodeEngine({
    required this.config,
    required this.logger,
    required this.radio,
    required this.tts,
  }) {
    _callsign = config.callsign;
    _parrotAuto = config.getString('NODE', 'PARROT_MODE', 'dtmf').toLowerCase() == 'auto';
    _parrotEnabled = _parrotAuto;
    _courtesyTone = config.getBool('NODE', 'COURTESY_TONE', false);
    _maxParrotSeconds = config.getInt('NODE', 'PARROT_MAX_SEC', 60).clamp(5, 600);
    _parrotDelayMs = config.getInt('NODE', 'PARROT_DELAY_MS', 800);
    _voxHangMs = config.getInt('NODE', 'VOX_HANG_MS', 600);
    _txLeadSilenceMs = config.getInt('NODE', 'TX_LEAD_SILENCE_MS', 100);
    _txTailSilenceMs = config.getInt('NODE', 'TX_TAIL_SILENCE_MS', 200);
    _txTimeoutSec = config.getInt('NODE', 'TX_TIMEOUT_SEC', 180);
    _idIntervalSec = config.getInt('NODE', 'ID_INTERVAL_SEC', 600);
    _idMinGapSec = config.getInt('NODE', 'ID_MIN_GAP_SEC', 120);
    _voxThresholdDb = config.getInt('NODE', 'VOX_THRESHOLD_DB', -38).toDouble();
    _cwWpm = config.getInt('NODE', 'CW_WPM', 20);
    _cwToneHz = config.getInt('NODE', 'CW_TONE_HZ', 700).toDouble();
    _rxGainDb = config.getDouble('NODE', 'RX_GAIN_DB', 0);
    _txGainDb = config.getDouble('NODE', 'TX_GAIN_DB', 0);
    _ttsEnabled = config.ttsEnabled;
    _rxRunLimit = 32000 * _maxParrotSeconds;
    _configureSquelch();
    _configureCommands();
    _dtmf = DtmfDecoder();
    _ackClip = ToneGenerator.beep(1000, 100, levelDb: -16);
    _courtesyClip = ToneGenerator.beep(900, 120, levelDb: -18);
    if (_callsign.isNotEmpty) {
      _identClip = ToneGenerator.cw(_callsign, _cwWpm, _cwToneHz);
    }
    _helpClip = ToneGenerator.cw('HELP', _cwWpm, _cwToneHz);
  }

  final AppConfig config;
  final NodeLogger logger;
  final RadioSession radio;
  final TtsService tts;

  late final DtmfDecoder _dtmf;
  late final DtmfCommandParser _parser;
  late Int16List _ackClip;
  late Int16List _courtesyClip;
  Int16List _identClip = Int16List(0);
  Int16List _helpClip = Int16List(0);

  String _callsign = '';
  bool _ttsEnabled = true;
  bool _parrotAuto = false;
  bool _parrotEnabled = false;
  bool _courtesyTone = false;
  bool _useRadioSquelch = true;
  bool _useVoxSquelch = true;
  int _maxParrotSeconds = 60;
  int _parrotDelayMs = 800;
  int _voxHangMs = 600;
  int _txLeadSilenceMs = 100;
  int _txTailSilenceMs = 200;
  int _txTimeoutSec = 180;
  int _idIntervalSec = 600;
  int _idMinGapSec = 120;
  double _voxThresholdDb = -38;
  int _cwWpm = 20;
  double _cwToneHz = 700;
  double _rxGainDb = 0;
  double _txGainDb = 0;
  String _dialPrefix = '4';
  int _rxRunLimit = 32000 * 60;

  bool _running = false;
  bool _radioReady = false;
  bool _carrierOpen = false;
  bool _radioRx = false;
  bool _voxOpen = false;
  bool _txActive = false;
  bool _cancelTx = false;
  bool _dtmfMuteActive = false;
  bool _suppressParrot = false;
  bool _startupPlayed = false;
  bool _pendingCommandValid = false;
  NodeCommand _pendingCommand = NodeCommand.none;
  String _pendingCommandArg = '';
  DateTime _lastVoice = DateTime.now();
  DateTime _lastActivity = DateTime.now();
  DateTime _lastDigit = DateTime.now();
  DateTime _lastIdent = DateTime.now();
  DateTime _lastNetworkAudio = DateTime.now();
  DateTime _dtmfMuteUntil = DateTime.fromMillisecondsSinceEpoch(0);
  DateTime _lastRxLog = DateTime.fromMillisecondsSinceEpoch(0);
  DateTime _lastUiNotify = DateTime.fromMillisecondsSinceEpoch(0);
  DateTime _lastTxBlockLog = DateTime.fromMillisecondsSinceEpoch(0);
  double _lastRxDb = -120;
  DateTime _lastRxAudioAt = DateTime.fromMillisecondsSinceEpoch(0);

  final List<int> _rxRun = [];
  final List<TxClip> _txQueue = [];
  final List<int> _networkPcm = [];
  bool _networkActive = false;
  bool _networkEnd = false;
  RadioStatus _status = RadioStatus();
  Timer? _tickTimer;
  Completer<void>? _txWake;
  bool _txLoopRunning = false;

  void Function(Int16List pcm)? onUplinkPcm;
  Future<void> Function(bool connect)? linkControl;
  Future<void> Function()? disconnectAllControl;
  Future<void> Function(bool connect, String target)? echoLinkControl;

  bool get parrotEnabled => _parrotEnabled;
  bool get txActive => _txActive;
  bool get radioReady => _radioReady;
  bool get linkAudioActive => _networkActive;
  RadioStatus get status => _status;
  String get callsign => _callsign;
  double get lastRxDb => _lastRxDb;
  DateTime get lastRxAudioAt => _lastRxAudioAt;

  void _configureSquelch() {
    final mode = config.getString('NODE', 'SQUELCH_MODE', 'auto').toLowerCase();
    switch (mode) {
      case 'radio':
        _useRadioSquelch = true;
        _useVoxSquelch = false;
      case 'vox':
        _useRadioSquelch = false;
        _useVoxSquelch = true;
      default:
        _useRadioSquelch = true;
        _useVoxSquelch = true;
    }
  }

  void _configureCommands() {
    _parser = DtmfCommandParser();
    final prefix = config.getString('NODE', 'DTMF_PREFIX');
    final suffix = config.getString('NODE', 'DTMF_SUFFIX', '#');
    _parser.prefix = prefix;
    _parser.suffix = suffix.isEmpty ? '#' : suffix;
    _dialPrefix = config.getString('ECHOLINK', 'DTMF_PREFIX', '4');
    _parser.dialPrefix = _dialPrefix;
    _parser.setCommands(config.dtmfCommands());
  }

  Future<void> start() async {
    if (_running) {
      return;
    }
    _running = true;
    _lastVoice = DateTime.now();
    _lastActivity = DateTime.now();
    _lastDigit = DateTime.now();
    _lastIdent = DateTime.now();
    radio.audio.onPcm = _onAudioPcm;
    radio.audio.onEnd = _onAudioEnd;
    radio.onStatus = _onRadioStatus;
    radio.onDisconnected = _onRadioDisconnected;
    logger.info('Node callsign: ${_callsign.isEmpty ? '(none)' : _callsign}');
    logger.info(
      'Parrot mode: ${_parrotAuto ? 'auto' : 'DTMF'}, '
      'VOX threshold ${_voxThresholdDb.round()} dB',
    );
    unawaited(_loadClips());
    _tickTimer = Timer.periodic(const Duration(milliseconds: 100), (_) => _tick());
    if (!_txLoopRunning) {
      _txLoopRunning = true;
      unawaited(_txLoop());
    }
    notifyListeners();
  }

  Future<void> stop() async {
    if (!_running) {
      return;
    }
    _running = false;
    _tickTimer?.cancel();
    _tickTimer = null;
    _cancelTx = true;
    _wakeTx();
    await radio.audio.endTx();
    _txActive = false;
    notifyListeners();
  }

  Future<void> _loadClips() async {
    if (_ttsEnabled) {
      final spelled = _callsign.split('').join(' ');
      final identText = 'This is $spelled';
      final helpText =
          'BenshiNode help. This is $_callsign. Press 0 for help. '
          'Press 1 to toggle parrot. Press 2 to connect the network link. '
          'Press 3 to send identification. Press 4 to connect EchoLink. '
          'Press 4 plus a node number to dial EchoLink. Press 5 to '
          'disconnect EchoLink. Press 6 to join Discord. Press 7 to leave '
          'Discord. Press 8 to disconnect all links. Press 9 to reset the node.';
      final ident = await tts.synthesize(identText);
      if (ident != null) {
        _identClip = ident;
      }
      final help = await tts.synthesize(helpText);
      if (help != null) {
        _helpClip = help;
      }
    }
    if (_identClip.isEmpty && _callsign.isNotEmpty) {
      _identClip = ToneGenerator.cw(_callsign, _cwWpm, _cwToneHz);
    }
    if (_helpClip.isEmpty) {
      _helpClip = ToneGenerator.cw('HELP', _cwWpm, _cwToneHz);
    }
  }

  Future<void> _speakPrompt(String key, String text) async {
    if (_ttsEnabled) {
      final pcm = await tts.synthesize(text);
      if (pcm != null && pcm.isNotEmpty) {
        _enqueueClip(pcm, key, DateTime.now());
        return;
      }
    }
    _enqueueClip(_ackClip, key, DateTime.now());
  }

  Future<void> speakPromptKey(String key) async {
    switch (key) {
      case 'startup':
        await _speakPrompt('startup', 'BenshiNode ready. This is $_callsign.');
      case 'voice_test':
        await _speakPrompt(
          'voice_test',
          'BenshiNode voice test. This is $_callsign.',
        );
      case 'parrot_on':
        await _speakPrompt('parrot_on', 'Parrot on.');
      case 'parrot_off':
        await _speakPrompt('parrot_off', 'Parrot off.');
      case 'reset':
        await _speakPrompt('reset', 'Node reset.');
      case 'echo_connecting':
        await _speakPrompt('echo_connecting', 'Connecting EchoLink.');
      case 'echo_disconnected':
        await _speakPrompt('echo_disconnected', 'EchoLink disconnected.');
      case 'discord_unavailable':
        await _speakPrompt(
          'discord_unavailable',
          'Discord is not available on Android.',
        );
      case 'link_connected':
        await _speakPrompt('link_connected', 'Connected to Network.');
      case 'link_disconnected':
        await _speakPrompt('link_disconnected', 'Disconnected from Network.');
      case 'all_disconnected':
        await _speakPrompt(
          'all_disconnected',
          'Disconnected from all links.',
        );
      default:
        await _speakPrompt(key, key);
    }
  }

  void setRadioReady(bool ready) {
    _radioReady = ready;
    if (ready && !_startupPlayed && config.startupPrompt) {
      _startupPlayed = true;
      unawaited(speakPromptKey('startup'));
    }
    notifyListeners();
  }

  void _onRadioStatus(RadioStatus status) {
    final previous = _status;
    final changed = status.powerOn != previous.powerOn ||
        status.inTx != previous.inTx ||
        status.inRx != previous.inRx ||
        status.squelchOpen != previous.squelchOpen ||
        status.channelId != previous.channelId;
    _status = status;
    if (changed) {
      logger.info(
        'Radio status: power=${status.powerOn} tx=${status.inTx} '
        'rx=${status.inRx} sq=${status.squelchOpen} '
        'rssi=${status.rssi} ch=${status.channelId}',
      );
    }
    _radioRx = _useRadioSquelch && (status.inRx || status.squelchOpen);
    final now = DateTime.now();
    if (_radioRx) {
      if (!_carrierOpen) {
        _openCarrier();
      }
      _lastVoice = now;
    }
    _wakeTx();
    notifyListeners();
  }

  void _onRadioDisconnected() {
    _radioReady = false;
    logger.warning('Radio control connection lost');
    notifyListeners();
  }

  void _onAudioPcm(Int16List pcm, bool transmit) {
    if (pcm.isEmpty || transmit) {
      return;
    }
    final db = rmsDb(pcm);
    final now = DateTime.now();
    _lastRxDb = db;
    _lastRxAudioAt = now;
    if (now.difference(_lastRxLog).inSeconds >= 5) {
      _lastRxLog = now;
      logger.debug('RX audio ${db.round()} dB');
    }
    if (now.difference(_lastUiNotify).inMilliseconds >= 500) {
      _lastUiNotify = now;
      notifyListeners();
    }
    var dtmfActivity = false;
    var commandComplete = false;
    Int16List? uplink;

    if (_useVoxSquelch && db >= _voxThresholdDb) {
      _voxOpen = true;
      _lastVoice = now;
      if (!_carrierOpen) {
        _openCarrier();
      }
    }

    if (_carrierOpen || _radioRx) {
      if (!_carrierOpen) {
        _openCarrier();
      }
      final remaining = _rxRun.length >= _rxRunLimit
          ? 0
          : _rxRunLimit - _rxRun.length;
      if (remaining > 0) {
        final count = math.min(remaining, pcm.length);
        final mute = _dtmfMuteActive || now.isBefore(_dtmfMuteUntil);
        if (!mute) {
          uplink = pcm;
        }
        if (mute) {
          _rxRun.addAll(Uint8List(count));
        } else {
          final gained = applyGain(
            Int16List.fromList(pcm.sublist(0, count)),
            _rxGainDb,
          );
          _rxRun.addAll(gained);
        }
      }

      _dtmf.process(pcm, (digit) {
        final digitNow = DateTime.now();
        dtmfActivity = true;
        _lastDigit = digitNow;
        logger.debug('DTMF digit $digit');
        if (_parser.buffer.isEmpty &&
            _parser.prefix.isNotEmpty &&
            digit == _parser.prefix) {
          _dtmfMuteActive = true;
        }
        _dtmfMuteUntil = digitNow.add(const Duration(milliseconds: 120));
        _muteRxRunTail(90);
        final result = _parser.feed(digit);
        if (result.completed) {
          _dtmfMuteActive = false;
          _dtmfMuteUntil = DateTime.now().add(const Duration(milliseconds: 120));
          _muteRxRunTail(90);
          commandComplete = true;
          if (result.command != NodeCommand.none) {
            _pendingCommand = result.command;
            _pendingCommandArg = result.command == NodeCommand.echoLinkDial
                ? result.arg
                : '';
            _pendingCommandValid = true;
          } else {
            logger.warning('Unknown DTMF command');
          }
          _suppressParrot = true;
        }
      });
    }

    if (commandComplete || dtmfActivity) {
      uplink = null;
    }
    if (commandComplete) {
      logger.info('DTMF command: ${nodeCommandName(_pendingCommand)}');
    }
    final callback = onUplinkPcm;
    if (callback != null && uplink != null) {
      callback(uplink);
    }
    _wakeTx();
  }

  void _onAudioEnd() {
    logger.debug('Radio audio run ended');
  }

  void _tick() {
    if (!_running) {
      return;
    }
    final now = DateTime.now();
    var run = const <int>[];
    var hadCommand = false;
    var command = NodeCommand.none;
    var commandArg = '';

    if (_voxOpen && now.difference(_lastVoice).inMilliseconds > _voxHangMs) {
      _voxOpen = false;
    }
    if (_carrierOpen &&
        !_radioRx &&
        !_voxOpen &&
        now.difference(_lastVoice).inMilliseconds > _voxHangMs) {
      final closed = _closeCarrier(now);
      run = closed.run;
      hadCommand = closed.hadCommand;
      command = closed.command;
      commandArg = closed.arg;
    }
    if (_parser.buffer.isNotEmpty &&
        now.difference(_lastDigit).inMilliseconds > 2000) {
      _parser.reset();
      _dtmfMuteActive = false;
      _dtmfMuteUntil = DateTime.fromMillisecondsSinceEpoch(0);
    }
    if (!_carrierOpen &&
        !_txActive &&
        _txQueue.isEmpty &&
        _callsign.isNotEmpty &&
        now.difference(_lastIdent).inSeconds >= _idIntervalSec &&
        now.difference(_lastActivity).inSeconds >= _idMinGapSec) {
      _queueIdent(now);
    }
    if (run.isNotEmpty || hadCommand) {
      _handleRxEnd(Int16List.fromList(run), hadCommand, command, commandArg);
    }
  }

  ({List<int> run, bool hadCommand, NodeCommand command, String arg})
      _closeCarrier(DateTime now) {
    if (!_carrierOpen) {
      return (
        run: const <int>[],
        hadCommand: false,
        command: NodeCommand.none,
        arg: '',
      );
    }
    _carrierOpen = false;
    final run = List<int>.from(_rxRun);
    _rxRun.clear();
    final hadCommand = _pendingCommandValid;
    final command = _pendingCommand;
    final arg = _pendingCommandArg;
    _pendingCommandValid = false;
    _pendingCommand = NodeCommand.none;
    _pendingCommandArg = '';
    _parser.reset();
    _dtmf.reset();
    _dtmfMuteActive = false;
    _dtmfMuteUntil = DateTime.fromMillisecondsSinceEpoch(0);
    _lastActivity = now;
    logger.info('Squelch closed');
    _wakeTx();
    return (run: run, hadCommand: hadCommand, command: command, arg: arg);
  }

  void _openCarrier() {
    if (_carrierOpen) {
      return;
    }
    _carrierOpen = true;
    _rxRun.clear();
    _suppressParrot = false;
    _pendingCommandValid = false;
    _pendingCommand = NodeCommand.none;
    _pendingCommandArg = '';
    _dtmf.reset();
    _parser.reset();
    _dtmfMuteActive = false;
    _dtmfMuteUntil = DateTime.fromMillisecondsSinceEpoch(0);
    final now = DateTime.now();
    _lastVoice = now;
    _lastDigit = now;
    if (!_txActive) {
      if (_txQueue.isNotEmpty) {
        logger.info('Local RF activity: dropping queued transmit audio');
        _txQueue.clear();
      }
      if (_networkActive || _networkPcm.isNotEmpty) {
        logger.info('Local RF activity: dropping queued link audio');
        _networkPcm.clear();
        _networkActive = false;
        _networkEnd = false;
      }
    }
    logger.info('Squelch open');
    _wakeTx();
    notifyListeners();
  }

  void _muteRxRunTail(int milliseconds) {
    if (_rxRun.isEmpty || milliseconds <= 0) {
      return;
    }
    final count = math.min(_rxRun.length, 32000 * milliseconds ~/ 1000);
    final start = _rxRun.length - count;
    for (var i = start; i < _rxRun.length; i++) {
      _rxRun[i] = 0;
    }
  }

  void _handleRxEnd(
    Int16List run,
    bool hadCommand,
    NodeCommand command,
    String commandArg,
  ) {
    if (hadCommand) {
      executeCommand(command, run, commandArg);
      return;
    }
    if (_parrotEnabled && !_suppressParrot && run.isNotEmpty) {
      _enqueueClip(
        run,
        'parrot',
        DateTime.now().add(Duration(milliseconds: _parrotDelayMs)),
      );
    } else if (_courtesyTone && run.isNotEmpty) {
      _enqueueClip(
        _courtesyClip,
        'courtesy',
        DateTime.now().add(Duration(milliseconds: _parrotDelayMs)),
      );
    }
  }

  Future<void> executeCommand(
    NodeCommand command, [
    Int16List? run,
    String commandArg = '',
  ]) async {
    switch (command) {
      case NodeCommand.parrotToggle:
        _parrotEnabled = !_parrotEnabled;
        if (!_parrotEnabled) {
          _cancelTx = _txActive;
          _txQueue.clear();
        }
        logger.info('Parrot ${_parrotEnabled ? 'enabled' : 'disabled'}');
        if (_parrotEnabled) {
          await speakPromptKey('parrot_on');
          if (run != null && run.isNotEmpty) {
            _enqueueClip(
              run,
              'parrot',
              DateTime.now().add(Duration(milliseconds: _parrotDelayMs)),
            );
          }
        } else {
          await speakPromptKey('parrot_off');
        }
      case NodeCommand.help:
        _enqueueClip(_helpClip, 'help', DateTime.now());
      case NodeCommand.ident:
        if (_identClip.isNotEmpty) {
          _enqueueClip(_identClip, 'ident', DateTime.now());
          _lastIdent = DateTime.now();
        }
      case NodeCommand.reset:
        _cancelTx = _txActive;
        _txQueue.clear();
        _rxRun.clear();
        _pendingCommandValid = false;
        _suppressParrot = false;
        logger.info('Node state reset');
        await speakPromptKey('reset');
      case NodeCommand.echoLinkConnect:
        logger.info('DTMF EchoLink connect to configured node');
        final control = echoLinkControl;
        if (control != null) {
          await control(true, '');
        } else {
          logger.warning('EchoLink control is not configured');
        }
        await speakPromptKey('echo_connecting');
      case NodeCommand.echoLinkDial:
        logger.info('DTMF EchoLink dial $commandArg');
        final control = echoLinkControl;
        if (control != null) {
          await control(true, commandArg);
        } else {
          logger.warning('EchoLink control is not configured');
        }
        await speakPromptKey('echo_connecting');
      case NodeCommand.echoLinkDisconnect:
        logger.info('DTMF EchoLink disconnect');
        final control = echoLinkControl;
        if (control != null) {
          await control(false, '');
        }
        await speakPromptKey('echo_disconnected');
      case NodeCommand.discordJoin:
        logger.warning('Discord is not supported on Android');
        await speakPromptKey('discord_unavailable');
      case NodeCommand.discordLeave:
        logger.warning('Discord is not supported on Android');
        await speakPromptKey('discord_unavailable');
      case NodeCommand.linkConnect:
        logger.info('DTMF network link connect');
        final control = linkControl;
        if (control != null) {
          await control(true);
        } else {
          logger.warning('Link control is not configured');
        }
      case NodeCommand.disconnectAll:
        logger.info('DTMF disconnect all links');
        final all = disconnectAllControl;
        if (all != null) {
          await all();
        } else {
          await linkControl?.call(false);
          await echoLinkControl?.call(false, '');
        }
        await speakPromptKey('all_disconnected');
      case NodeCommand.none:
        break;
    }
    notifyListeners();
  }

  Future<void> runCommand(NodeCommand command, [String arg = '']) async {
    await executeCommand(command, null, arg);
  }

  void testBeep() {
    logger.info('Queueing test tone');
    _enqueueClip(
      ToneGenerator.beep(1000, 1200, levelDb: -12),
      'test-tone',
      DateTime.now(),
    );
  }

  Future<void> testVoice() async {
    logger.info('Queueing test voice prompt');
    await speakPromptKey('voice_test');
  }

  bool submitDtmfCode(String code) {
    if (code.isEmpty) {
      return false;
    }
    final commands = config.dtmfCommands();
    final command = commands[code];
    if (command != null) {
      unawaited(runCommand(command));
      return true;
    }
    if (_dialPrefix.isNotEmpty &&
        code.length > _dialPrefix.length + 1 &&
        code.startsWith(_dialPrefix) &&
        code.endsWith('#') &&
        (_parser.suffix.isNotEmpty)) {
      final digits = code.substring(_dialPrefix.length, code.length - 1);
      if (digits.isNotEmpty && digits.codeUnits.every((c) => c >= 0x30 && c <= 0x39)) {
        unawaited(runCommand(NodeCommand.echoLinkDial, digits));
        return true;
      }
    }
    return false;
  }

  void enqueueNetworkAudio(Int16List pcm, bool end) {
    if (!_running) {
      return;
    }
    if (pcm.isNotEmpty) {
      if (_networkPcm.length > 32000 * 5) {
        _networkPcm.removeRange(0, _networkPcm.length - 32000 * 5);
      }
      _networkPcm.addAll(pcm);
    }
    if (end) {
      _networkEnd = true;
    }
    _networkActive = true;
    _lastNetworkAudio = DateTime.now();
    _wakeTx();
  }

  void _enqueueClip(
    Int16List pcm,
    String name,
    DateTime notBefore, {
    bool allowDrop = true,
  }) {
    if (pcm.isEmpty || !_running) {
      return;
    }
    _txQueue.add(TxClip(pcm, name, notBefore, allowDrop: allowDrop));
    _wakeTx();
  }

  void _queueIdent(DateTime now) {
    if (_identClip.isEmpty) {
      return;
    }
    _txQueue.add(TxClip(_identClip, 'ident', now));
    _lastIdent = now;
    _wakeTx();
  }

  void _wakeTx() {
    final wake = _txWake;
    if (wake != null && !wake.isCompleted) {
      wake.complete();
    }
    _txWake = null;
  }

  Future<void> _waitForWake(int milliseconds) async {
    final wake = Completer<void>();
    _txWake = wake;
    await wake.future.timeout(
      Duration(milliseconds: milliseconds),
      onTimeout: () {},
    );
    if (identical(_txWake, wake)) {
      _txWake = null;
    }
  }

  void _logTxBlocked() {
    final now = DateTime.now();
    if (now.difference(_lastTxBlockLog).inSeconds < 2) {
      return;
    }
    _lastTxBlockLog = now;
    logger.warning(
      'TX blocked: carrier=$_carrierOpen inTx=${_status.inTx} '
      'power=${_status.powerOn} ready=$_radioReady '
      'audio=${radio.audio.connected} queue=${_txQueue.length}',
    );
  }

  Future<void> _txLoop() async {
    while (_running) {
      if (_txQueue.isEmpty && !_networkActive) {
        await _waitForWake(100);
        continue;
      }
      if (_txQueue.isEmpty && _networkActive) {
        await _transmitNetwork();
        continue;
      }
      final clip = _txQueue.first;
      final now = DateTime.now();
      if (now.isBefore(clip.notBefore)) {
        final wait = clip.notBefore.difference(now).inMilliseconds;
        await _waitForWake(wait.clamp(1, 100));
        continue;
      }
      if (_carrierOpen ||
          _status.inTx ||
          !_status.powerOn ||
          !_radioReady ||
          !radio.audio.connected) {
        _logTxBlocked();
        await _waitForWake(150);
        continue;
      }
      if (_cancelTx) {
        _cancelTx = false;
        _txQueue.clear();
        continue;
      }
      _txQueue.removeAt(0);
      await _transmitClip(clip);
    }
    _txLoopRunning = false;
  }

  Future<void> _transmitClip(TxClip clip) async {
    _txActive = true;
    _cancelTx = false;
    notifyListeners();
    var ok = false;
    try {
      final lead = 32000 * _txLeadSilenceMs ~/ 1000;
      final tail = 32000 * _txTailSilenceMs ~/ 1000;
      final gained = applyGain(clip.pcm, _txGainDb);
      final pcm = Int16List(lead + gained.length + tail);
      pcm.setRange(lead, lead + gained.length, gained);
      logger.info('TX: ${clip.name} (${pcm.length * 1000 ~/ 32000} ms)');
      ok = radio.audio.beginTx();
      if (!ok) {
        logger.error('Could not start transmit audio');
      } else {
        final stopwatch = Stopwatch()..start();
        var offset = 0;
        const chunkSamples = 32000 ~/ 50;
        while (offset < pcm.length && _running && !_cancelTx) {
          final count = math.min(chunkSamples, pcm.length - offset);
          final chunk = Int16List.fromList(pcm.sublist(offset, offset + count));
          if (!radio.audio.sendPcm(chunk)) {
            logger.warning('Transmit audio write failed');
            ok = false;
            break;
          }
          offset += count;
          final targetMs = offset * 1000 ~/ 32000;
          final wait = targetMs - stopwatch.elapsedMilliseconds;
          if (wait > 0) {
            await Future<void>.delayed(Duration(milliseconds: wait));
          }
          if (stopwatch.elapsed.inSeconds > _txTimeoutSec) {
            logger.warning('Transmit timeout; aborting');
            _cancelTx = true;
            break;
          }
        }
        final totalMs = pcm.length * 1000 ~/ 32000;
        final remaining = totalMs - stopwatch.elapsedMilliseconds;
        if (remaining > 0 && !_cancelTx) {
          await Future<void>.delayed(Duration(milliseconds: remaining));
        }
        if (_cancelTx) {
          logger.info('Transmission cancelled');
        } else if (ok) {
          logger.info('TX complete: ${clip.name}');
        }
      }
    } catch (error) {
      ok = false;
      logger.error('Transmit failed: $error');
    } finally {
      try {
        await radio.audio.endTx();
      } catch (error) {
        logger.warning('Could not end transmit audio: $error');
      }
      try {
        final unkeyed = await radio.forceUnkey();
        if (!unkeyed) {
          logger.warning('Radio may still be keyed');
        }
      } catch (error) {
        logger.warning('Force unkey failed: $error');
      }
      _txActive = false;
      _lastActivity = DateTime.now();
      _cancelTx = false;
      notifyListeners();
    }
  }

  Future<void> _transmitNetwork() async {
    if (_carrierOpen ||
        _status.inTx ||
        !_status.powerOn ||
        !_radioReady ||
        !radio.audio.connected) {
      _logTxBlocked();
      await _waitForWake(150);
      return;
    }
    if (_networkPcm.isEmpty) {
      if (_networkEnd ||
          DateTime.now().difference(_lastNetworkAudio).inMilliseconds > 500) {
        _networkActive = false;
        _networkEnd = false;
        return;
      }
      await _waitForWake(20);
      return;
    }

    _txActive = true;
    _cancelTx = false;
    notifyListeners();
    final ok = radio.audio.beginTx();
    if (!ok) {
      logger.error('Could not start network transmit audio');
    }
    final stopwatch = Stopwatch()..start();
    var sentSamples = 0;
    while (_running && _networkActive && !_cancelTx) {
      Int16List chunk;
      var end = false;
      if (_networkPcm.isNotEmpty) {
        final count = math.min(640, _networkPcm.length);
        chunk = Int16List.fromList(_networkPcm.sublist(0, count));
        _networkPcm.removeRange(0, count);
      } else {
        end = _networkEnd ||
            DateTime.now().difference(_lastNetworkAudio).inMilliseconds > 500;
        chunk = Int16List(0);
      }
      if (chunk.isNotEmpty) {
        if (ok && !radio.audio.sendPcm(chunk)) {
          logger.warning('Link transmit audio write failed');
        }
        sentSamples += chunk.length;
        final targetMs = sentSamples * 1000 ~/ 32000;
        final wait = targetMs - stopwatch.elapsedMilliseconds;
        if (wait > 0) {
          await Future<void>.delayed(Duration(milliseconds: wait));
        }
      } else if (end) {
        break;
      } else {
        await Future<void>.delayed(const Duration(milliseconds: 10));
      }
    }
    if (ok) {
      await radio.audio.endTx();
    }
    final unkeyed = await radio.forceUnkey();
    if (!unkeyed) {
      logger.warning('Radio may still be keyed');
    }
    _txActive = false;
    _networkActive = false;
    _networkEnd = false;
    _networkPcm.clear();
    _lastActivity = DateTime.now();
    _cancelTx = false;
    notifyListeners();
  }
}
