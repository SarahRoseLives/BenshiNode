import 'dart:async';
import 'dart:typed_data';

import '../services/bluetooth_classic.dart';
import 'gaia.dart';
import 'logger.dart';
import 'radio_audio.dart';

class RadioStatus {
  bool powerOn = false;
  bool inTx = false;
  bool inRx = false;
  bool squelchOpen = false;
  bool scanning = false;
  bool fmRadio = false;
  bool gpsLocked = false;
  bool hfpConnected = false;
  bool aocConnected = false;
  int rssi = 0;
  int channelId = -1;
  int region = 0;
  int doubleChannel = 0;
}

class RadioDeviceInfo {
  int vendorId = 0;
  int productId = 0;
  int hardwareVersion = 0;
  int softwareVersion = 0;
  int channelCount = 0;
  int regionCount = 0;
  bool supportsVfo = false;
  bool supportsDmr = false;
}

class _PendingReply {
  _PendingReply(this.group, this.command);

  final int group;
  final int command;
  final Completer<GaiaFrame> completer = Completer<GaiaFrame>();
}

class RadioSession {
  RadioSession(this.logger, {this.statusPollMs = 500, int bitpool = 18})
      : audio = RadioAudio(logger, bitpool: bitpool);

  final NodeLogger logger;
  final int statusPollMs;
  final BluetoothClassic _bt = BluetoothClassic.instance;
  final RadioAudio audio;

  final GaiaDecoder _decoder = GaiaDecoder();
  String? _mac;
  StreamSubscription<Uint8List>? _controlSubscription;
  Timer? _pollTimer;
  _PendingReply? _pending;
  Future<void> _commandChain = Future.value();
  int _pollFailures = 0;
  bool _connected = false;

  RadioStatus status = RadioStatus();
  RadioDeviceInfo deviceInfo = RadioDeviceInfo();

  void Function(RadioStatus status)? onStatus;
  void Function()? onDisconnected;

  bool get connected => _connected;
  String? get mac => _mac;

  Future<bool> connect(String mac) async {
    await disconnect();
    _mac = mac;
    _decoder.clear();
    _pollFailures = 0;

    logger.info('Connecting to $mac');
    final connected = await _bt.connect(mac);
    if (!connected) {
      logger.error('Bluetooth control connection failed');
      _mac = null;
      return false;
    }

    _controlSubscription = _bt.controlData.listen((data) {
      _onControlData(data);
    });

    try {
      await getDeviceInfo();
      await _sendCommand(
        Gaia.groupBasic,
        Gaia.registerNotification,
        const [1, 5, 6, 14],
        expectReply: false,
      );
      await getHtStatus();
    } catch (error) {
      logger.warning('Radio initialization failed: $error');
    }

    await audio.connect(mac);
    if (!audio.connected) {
      logger.error('Bluetooth audio connection failed');
      await disconnect();
      return false;
    }

    _connected = true;
    _pollTimer = Timer.periodic(Duration(milliseconds: statusPollMs), (_) {
      _poll();
    });
    logger.info('VR-N76 connected on $mac');
    return true;
  }

  Future<void> disconnect() async {
    _pollTimer?.cancel();
    _pollTimer = null;
    _connected = false;
    _controlSubscription?.cancel();
    _controlSubscription = null;
    final pending = _pending;
    _pending = null;
    if (pending != null && !pending.completer.isCompleted) {
      pending.completer.completeError(StateError('Radio disconnected'));
    }
    await audio.disconnect();
    final mac = _mac;
    _mac = null;
    if (mac != null) {
      await _bt.disconnect(mac);
    }
    _decoder.clear();
  }

  Future<void> _poll() async {
    if (!_connected) {
      return;
    }
    final ok = await getHtStatus();
    if (ok) {
      _pollFailures = 0;
      return;
    }
    _pollFailures++;
    if (_pollFailures >= 3) {
      logger.warning('Radio control connection lost');
      _connected = false;
      _pollTimer?.cancel();
      _pollTimer = null;
      onDisconnected?.call();
    }
  }

  Future<GaiaFrame?> _sendCommand(
    int group,
    int command,
    List<int> data, {
    bool expectReply = true,
    int timeoutMs = 1500,
  }) async {
    final previous = _commandChain;
    final gate = Completer<void>();
    _commandChain = gate.future;
    await previous;
    _PendingReply? pending;
    try {
      if (_mac == null) {
        throw StateError('Control channel is not connected');
      }
      if (expectReply) {
        pending = _PendingReply(group, command);
        _pending = pending;
      }
      final framed = Gaia.encode(Gaia.buildCommand(group, command, data));
      final ok = await _bt.send(_mac!, framed);
      if (!ok) {
        throw StateError('Failed to write control command');
      }
      if (!expectReply) {
        return null;
      }
      final frame = await pending!.completer.future.timeout(
        Duration(milliseconds: timeoutMs),
        onTimeout: () => throw TimeoutException('Radio reply timed out'),
      );
      return frame;
    } finally {
      if (pending != null && identical(_pending, pending)) {
        _pending = null;
      }
      gate.complete();
    }
  }

  void _onControlData(Uint8List data) {
    _decoder.push(data);
    while (true) {
      final frame = _decoder.next();
      if (frame == null) {
        return;
      }
      if (!frame.isResponse && frame.command == Gaia.eventNotification) {
        _handleNotification(frame);
        continue;
      }
      if (!frame.isResponse) {
        continue;
      }
      final pending = _pending;
      if (pending != null &&
          pending.group == frame.group &&
          pending.command == frame.command) {
        _pending = null;
        if (!pending.completer.isCompleted) {
          pending.completer.complete(frame);
        }
      }
    }
  }

  void _handleNotification(GaiaFrame frame) {
    if (frame.payload.isEmpty) {
      return;
    }
    if (frame.payload[0] == Gaia.notifHtStatusChanged) {
      _applyStatus(frame.payload, 1);
    }
  }

  Future<bool> getDeviceInfo() async {
    final frame = await _sendCommand(
      Gaia.groupBasic,
      Gaia.getDevInfo,
      const [3],
    );
    if (frame == null || frame.payload.isEmpty) {
      return false;
    }
    if (frame.payload[0] != 0) {
      return false;
    }
    final p = frame.payload;
    deviceInfo = RadioDeviceInfo()
      ..vendorId = Gaia.getByte(p, 1)
      ..productId = Gaia.getShort(p, 2)
      ..hardwareVersion = Gaia.getByte(p, 4)
      ..softwareVersion = Gaia.getShort(p, 5)
      ..channelCount = Gaia.getByte(p, 9)
      ..regionCount = (Gaia.getByte(p, 10) & 0xF0) >> 4
      ..supportsVfo = (Gaia.getByte(p, 8) & 0x02) != 0
      ..supportsDmr = (Gaia.getByte(p, 8) & 0x01) != 0;
    logger.info(
      'Radio: product=${deviceInfo.productId} '
      'hw=${deviceInfo.hardwareVersion} sw=${deviceInfo.softwareVersion} '
      'channels=${deviceInfo.channelCount} regions=${deviceInfo.regionCount}',
    );
    return true;
  }

  Future<bool> getHtStatus() async {
    try {
      final frame = await _sendCommand(
        Gaia.groupBasic,
        Gaia.getHtStatus,
        const [],
      );
      if (frame == null || frame.payload.isEmpty) {
        return false;
      }
      if (frame.payload[0] != 0) {
        return false;
      }
      _applyStatus(frame.payload, 1);
      return true;
    } catch (_) {
      return false;
    }
  }

  void _applyStatus(Uint8List payload, int offset) {
    if (offset + 3 >= payload.length) {
      return;
    }
    final flags = payload[offset];
    final flags2 = payload[offset + 1];
    final flags3 = payload[offset + 2];
    final flags4 = payload[offset + 3];
    final next = RadioStatus()
      ..powerOn = (flags & 0x80) != 0
      ..inTx = (flags & 0x40) != 0
      ..squelchOpen = (flags & 0x20) != 0
      ..inRx = (flags & 0x10) != 0
      ..doubleChannel = (flags & 0x0C) >> 2
      ..scanning = (flags & 0x02) != 0
      ..fmRadio = (flags & 0x01) != 0
      ..gpsLocked = (flags2 & 0x08) != 0
      ..hfpConnected = (flags2 & 0x04) != 0
      ..aocConnected = (flags2 & 0x02) != 0
      ..rssi = (flags3 & 0xF0) >> 4
      ..region = ((flags3 & 0x0F) << 2) | ((flags4 & 0xC0) >> 6)
      ..channelId = (((flags4 & 0x3C) >> 2) << 4) | ((flags2 & 0xF0) >> 4);
    status = next;
    onStatus?.call(next);
  }

  Future<bool> setChannel(int channelId) async {
    if (channelId < 0) {
      return true;
    }
    final frame = await _sendCommand(Gaia.groupBasic, Gaia.readSettings, const []);
    if (frame == null || frame.payload.isEmpty || frame.payload[0] != 0) {
      return false;
    }
    final raw = Uint8List.fromList(frame.payload.sublist(1));
    if (raw.length < 10) {
      return false;
    }
    final mode = (raw[1] & 0x30) >> 4;
    final channelA = ((raw[0] & 0xF0) >> 4) + (raw[9] & 0xF0);
    final channelB = (raw[0] & 0x0F) + ((raw[9] & 0x0F) << 4);
    final useB = mode == 2;
    if ((useB ? channelB : channelA) == channelId) {
      return true;
    }
    if (useB) {
      raw[0] = (raw[0] & 0xF0) | (channelId & 0x0F);
      raw[9] = (raw[9] & 0xF0) | ((channelId >> 4) & 0x0F);
    } else {
      raw[0] = (raw[0] & 0x0F) | ((channelId & 0x0F) << 4);
      raw[9] = (raw[9] & 0x0F) | (channelId & 0xF0);
    }
    await _sendCommand(
      Gaia.groupBasic,
      Gaia.writeSettings,
      raw,
      expectReply: false,
    );
    await Future<void>.delayed(const Duration(milliseconds: 250));
    final verify = await _sendCommand(
      Gaia.groupBasic,
      Gaia.readSettings,
      const [],
    );
    if (verify == null || verify.payload.isEmpty || verify.payload[0] != 0) {
      return false;
    }
    final v = Uint8List.fromList(verify.payload.sublist(1));
    if (v.length < 10) {
      return false;
    }
    final verifyA = ((v[0] & 0xF0) >> 4) + (v[9] & 0xF0);
    final verifyB = (v[0] & 0x0F) + ((v[9] & 0x0F) << 4);
    return (useB ? verifyB : verifyA) == channelId;
  }

  Future<bool> forceUnkey() async {
    await audio.endTx();
    for (var attempt = 0; attempt < 6; attempt++) {
      await Future<void>.delayed(const Duration(milliseconds: 200));
      await getHtStatus();
      if (!status.inTx) {
        return true;
      }
    }
    await _sendCommand(
      Gaia.groupBasic,
      Gaia.doProgFunc,
      const [4],
      expectReply: false,
    );
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await getHtStatus();
    if (!status.inTx) {
      return true;
    }
    await _sendCommand(
      Gaia.groupBasic,
      Gaia.doProgFunc,
      const [13],
      expectReply: false,
    );
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await getHtStatus();
    return !status.inTx;
  }
}
