import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import 'app_config.dart';
import 'link_client.dart';
import 'logger.dart';
import 'node_engine.dart';
import 'radio_session.dart';
import 'tts_service.dart';
import '../services/bluetooth_classic.dart';

class NodeController extends ChangeNotifier {
  final NodeLogger logger = NodeLogger();
  AppConfig? _config;
  RadioSession? _radio;
  NodeEngine? _engine;
  LinkClient? _link;
  TtsService? _tts;
  bool _running = false;
  bool _radioConnecting = false;
  bool _bluetoothWarned = false;
  String? lastRadioError;
  String? lastRadioMac;
  DateTime _nextRadioAttempt = DateTime.fromMillisecondsSinceEpoch(0);
  Timer? _radioTimer;

  AppConfig? get config => _config;
  RadioSession? get radio => _radio;
  NodeEngine? get engine => _engine;
  LinkClient? get link => _link;
  bool get running => _running;
  bool get radioConnected => _radio?.connected ?? false;
  bool get linkConnected => _link?.connected ?? false;

  Future<void> initialize() async {
    if (_config != null) {
      return;
    }
    _config = await AppConfig.load();
    _radio = RadioSession(
      logger,
      statusPollMs: _config!.statusPollMs,
      bitpool: _config!.sbcBitpool,
    );
    _tts = TtsService(logger, rate: _config!.ttsRate);
    _engine = NodeEngine(
      config: _config!,
      logger: logger,
      radio: _radio!,
      tts: _tts!,
    );
    _engine!.linkControl = _onLinkControl;
    _engine!.disconnectAllControl = _disconnectAll;
    logger.info('BenshiNode Android ready');
    notifyListeners();
  }

  Future<void> start() async {
    if (_running) {
      return;
    }
    await initialize();
    _running = true;
    await WakelockPlus.enable();
    await _engine!.start();
    if (_config!.linkEnabled && _config!.linkServer.isNotEmpty) {
      unawaited(connectLink());
    }
    _radioTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      _radioTick();
    });
    unawaited(_radioTick());
    notifyListeners();
  }

  Future<void> stop() async {
    _running = false;
    _radioTimer?.cancel();
    _radioTimer = null;
    await _link?.stop();
    _link = null;
    await _radio?.disconnect();
    await _engine?.stop();
    await WakelockPlus.disable();
    notifyListeners();
  }

  Future<void> _radioTick() async {
    if (!_running || _radio == null) {
      return;
    }
    final radio = _radio!;
    if (radio.connected || _radioConnecting) {
      return;
    }
    final now = DateTime.now();
    if (now.isBefore(_nextRadioAttempt)) {
      return;
    }
    final mac = _config!.mac;
    if (mac.isEmpty) {
      return;
    }
    lastRadioMac = mac;
    final available = await BluetoothClassic.instance.isAvailable();
    if (!available) {
      lastRadioError = 'Bluetooth is off';
      if (!_bluetoothWarned) {
        _bluetoothWarned = true;
        logger.warning('Bluetooth is off; enable it to connect the radio');
        notifyListeners();
      }
      _nextRadioAttempt = now.add(
        Duration(milliseconds: _config!.connectRetryMs),
      );
      return;
    }
    _bluetoothWarned = false;
    _radioConnecting = true;
    try {
      final ok = await radio.connect(mac);
      if (ok) {
        lastRadioError = null;
        _nextRadioAttempt = DateTime.fromMillisecondsSinceEpoch(0);
        _engine!.setRadioReady(true);
        final channel = _config!.channel;
        if (channel >= 0) {
          final applied = await radio.setChannel(channel);
          if (!applied) {
            logger.warning('Could not select channel $channel');
          }
        }
      } else {
        lastRadioError = 'Radio did not respond';
        logger.warning('Radio connect failed; will retry');
        _nextRadioAttempt = DateTime.now().add(
          Duration(milliseconds: _config!.connectRetryMs),
        );
      }
    } catch (error) {
      lastRadioError = error.toString();
      logger.warning('Radio connect error: $error');
      _nextRadioAttempt = DateTime.now().add(
        Duration(milliseconds: _config!.connectRetryMs),
      );
    } finally {
      _radioConnecting = false;
      notifyListeners();
    }
  }

  Future<void> retryRadio() async {
    _nextRadioAttempt = DateTime.fromMillisecondsSinceEpoch(0);
    await _radioTick();
  }

  Future<void> connectRadio() async {
    await initialize();
    if (!_running) {
      await start();
      return;
    }
    await _radioTick();
  }

  Future<void> connectLink() async {
    await initialize();
    final config = _config!;
    if (config.linkServer.isEmpty) {
      logger.warning('LINK/SERVER is not configured');
      return;
    }
    await _link?.stop();
    final endpoint = _parseEndpoint(config.linkServer);
    if (endpoint == null) {
      logger.warning('Invalid link server address: ${config.linkServer}');
      return;
    }
    final client = LinkClient(
      logger: logger,
      node: _engine!,
      host: endpoint.host,
      port: endpoint.port,
      key: config.linkKey,
      callsign: config.callsign,
    );
    client.onConnectionChanged = (connected) {
      unawaited(
        _engine!.speakPromptKey(
          connected ? 'link_connected' : 'link_disconnected',
        ),
      );
      if (connected) {
        _config!.set('LINK', 'ENABLED', '1');
        unawaited(_config!.save());
      }
      notifyListeners();
    };
    _link = client;
    client.start();
    notifyListeners();
  }

  Future<void> disconnectLink() async {
    await _link?.stop();
    _link = null;
    notifyListeners();
  }

  Future<void> _onLinkControl(bool connect) async {
    if (connect) {
      await connectLink();
    } else {
      await disconnectLink();
    }
  }

  Future<void> _disconnectAll() async {
    await disconnectLink();
  }

  Future<void> setChannel(int channel) async {
    await initialize();
    if (_radio == null || !_radio!.connected) {
      logger.warning('Radio is not connected');
      return;
    }
    final ok = await _radio!.setChannel(channel);
    if (ok) {
      _config!.set('RADIO', 'CHANNEL', channel.toString());
      await _config!.save();
      logger.info('Selected channel $channel');
    } else {
      logger.warning('Channel change failed');
    }
    notifyListeners();
  }

  Future<void> saveSettings() async {
    await _config?.save();
    notifyListeners();
  }

  Future<void> reload() async {
    final wasRunning = _running;
    await stop();
    _config = null;
    _radio = null;
    _engine = null;
    _tts = null;
    _link = null;
    await initialize();
    if (wasRunning) {
      await start();
    }
  }

  ({String host, int port})? _parseEndpoint(String value) {
    final trimmed = value.trim();
    if (trimmed.isEmpty) {
      return null;
    }
    final colon = trimmed.lastIndexOf(':');
    if (colon <= 0) {
      return (host: trimmed, port: 7300);
    }
    final host = trimmed.substring(0, colon);
    final port = int.tryParse(trimmed.substring(colon + 1));
    if (port == null || port < 1 || port > 65535) {
      return null;
    }
    return (host: host, port: port);
  }
}
