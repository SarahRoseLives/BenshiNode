import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';

import 'dsp.dart';
import 'logger.dart';
import 'node_engine.dart';

class LinkClient {
  LinkClient({
    required this.logger,
    required this.node,
    required this.host,
    this.port = 7300,
    required this.key,
    required this.callsign,
  });

  final NodeLogger logger;
  final NodeEngine node;
  final String host;
  final int port;
  final String key;
  final String callsign;

  final Downsampler32to8 _downsampler = Downsampler32to8();
  final Upsampler8to32 _upsampler = Upsampler8to32();
  final List<int> _uplink32k = [];
  final List<int> _uplink8k = [];

  bool _running = false;
  bool _connected = false;
  bool _reconnect = false;
  bool _talking = false;
  int _session = 0;
  int _sequence = 0;
  DateTime _lastUplink = DateTime.now();
  DateTime _lastServerActivity = DateTime.now();
  DateTime _lastHeartbeat = DateTime.now();
  DateTime _nextSend = DateTime.now();
  DateTime _lastDiag = DateTime.now();
  int _diagSamplesIn = 0;
  int _diagFramesOut = 0;

  void Function(bool connected)? onConnectionChanged;

  bool get connected => _connected;

  void start() {
    if (_running) {
      return;
    }
    _running = true;
    unawaited(_run());
  }

  Future<void> stop() async {
    _running = false;
    _reconnect = true;
    node.onUplinkPcm = null;
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }

  Future<void> _run() async {
    while (_running) {
      try {
        await _runSession();
      } catch (error) {
        logger.warning('Link error: $error');
      }
      if (!_running) {
        break;
      }
      logger.info('Reconnecting link in 5 seconds');
      for (var i = 0; i < 50 && _running; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    }
  }

  Future<void> _runSession() async {
    final result = await _authenticate();
    if (result == null || !_running) {
      return;
    }
    final udp = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    _session = result.session;
    _sequence = 0;
    _talking = false;
    _uplink32k.clear();
    _uplink8k.clear();
    _downsampler.reset();
    _upsampler.reset();
    _lastUplink = DateTime.now();
    _lastServerActivity = DateTime.now();
    _lastHeartbeat = DateTime.now();
    _nextSend = DateTime.now();
    node.onUplinkPcm = _onUplinkPcm;

    _sendPacket(
      udp,
      result.address,
      result.udpPort,
      type: 1,
      session: _session,
      sequence: _sequence++,
    );

    _connected = true;
    onConnectionChanged?.call(true);
    logger.info('Linked to server ${result.address.address}:${result.udpPort}');

    final subscription = udp.listen((event) {
      if (event != RawSocketEvent.read) {
        return;
      }
      final datagram = udp.receive();
      if (datagram == null) {
        return;
      }
      _handleDownlink(datagram.data);
    });

    while (_running && !_reconnect) {
      final now = DateTime.now();
      if (now.difference(_lastServerActivity).inSeconds > 20) {
        logger.warning('Link server stopped responding; reconnecting');
        break;
      }
      if (!now.isBefore(_nextSend)) {
        var sendNow = now;
        for (var sent = 0; sent < 4 && !sendNow.isBefore(_nextSend); sent++) {
          _sendUplinkFrames(udp, result.address, result.udpPort);
          _nextSend = _nextSend.add(const Duration(milliseconds: 20));
          sendNow = DateTime.now();
        }
        if (sendNow.difference(_nextSend).inMilliseconds > 200) {
          _nextSend = sendNow;
        }
      }
      if (now.difference(_lastHeartbeat).inMilliseconds >= 5000) {
        _sendPacket(
          udp,
          result.address,
          result.udpPort,
          type: 4,
          session: _session,
          sequence: _sequence++,
        );
        _lastHeartbeat = now;
      }
      if (now.difference(_lastDiag).inSeconds >= 5) {
        _lastDiag = now;
        logger.debug(
          'Link uplink: $_diagSamplesIn samples in, '
          '$_diagFramesOut frames out',
        );
        _diagSamplesIn = 0;
        _diagFramesOut = 0;
      }
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }

    _connected = false;
    _reconnect = false;
    node.onUplinkPcm = null;
    node.enqueueNetworkAudio(Int16List(0), true);
    onConnectionChanged?.call(false);
    subscription.cancel();
    udp.close();
  }

  Future<({int session, int udpPort, InternetAddress address})?>
      _authenticate() async {
    Socket? socket;
    try {
      socket = await Socket.connect(
        host,
        port,
        timeout: const Duration(seconds: 5),
      );
      final iterator = StreamIterator<List<int>>(socket);
      final challenge = await _readExact(iterator, 40);
      if (challenge == null || _read32le(challenge, 0) != 0x31534E42) {
        logger.warning('Invalid link authentication challenge');
        return null;
      }
      final nonce = Uint8List.sublistView(challenge, 8, 40);
      final response = Uint8List(52);
      _write32le(response, 0, 0x31534E42);
      final callsignBytes = callsign.codeUnits.take(15).toList();
      response.setRange(4, 4 + callsignBytes.length, callsignBytes);
      if (key.isNotEmpty) {
        final digest = Hmac(sha256, key.codeUnits).convert(nonce).bytes;
        response.setRange(20, 52, digest);
      }
      socket.add(response);
      await socket.flush();
      final ok = await _readExact(iterator, 12);
      if (ok == null || _read32le(ok, 0) != 0x4B4F4E42) {
        logger.warning('Link authentication rejected');
        return null;
      }
      final session = _read32le(ok, 4);
      final udpPort = _read16le(ok, 8);
      final address = socket.remoteAddress;
      await iterator.cancel();
      return (session: session, udpPort: udpPort, address: address);
    } catch (error) {
      logger.warning('Link authentication failed: $error');
      return null;
    } finally {
      socket?.destroy();
    }
  }

  Future<Uint8List?> _readExact(
    StreamIterator<List<int>> iterator,
    int count,
  ) async {
    final builder = BytesBuilder(copy: false);
    while (builder.length < count) {
      if (!await iterator.moveNext()) {
        return null;
      }
      final chunk = iterator.current;
      builder.add(chunk);
      if (builder.length > count) {
        return null;
      }
    }
    return builder.takeBytes();
  }

  void _onUplinkPcm(Int16List pcm) {
    if (!_connected || pcm.isEmpty) {
      return;
    }
    if (_uplink32k.length > 64000) {
      _uplink32k.removeRange(0, _uplink32k.length - 64000);
    }
    _uplink32k.addAll(pcm);
    _diagSamplesIn += pcm.length;
    _lastUplink = DateTime.now();
  }

  void _sendUplinkFrames(
    RawDatagramSocket udp,
    InternetAddress address,
    int port,
  ) {
    if (_uplink32k.isNotEmpty) {
      final pcm = Int16List.fromList(_uplink32k);
      _uplink32k.clear();
      _uplink8k.addAll(_downsampler.process(pcm));
      _lastUplink = DateTime.now();
    }
    if (_uplink8k.length >= 160) {
      final frame = Int16List.fromList(_uplink8k.sublist(0, 160));
      _uplink8k.removeRange(0, 160);
      final payload = Uint8List(160);
      for (var i = 0; i < 160; i++) {
        payload[i] = linearToUlaw(frame[i]);
      }
      var flags = 0;
      if (!_talking) {
        flags |= 0x01;
        _talking = true;
      }
      _sendPacket(
        udp,
        address,
        port,
        type: 2,
        flags: flags,
        session: _session,
        sequence: _sequence++,
        payload: payload,
      );
      _diagFramesOut++;
    }
    if (_talking &&
        DateTime.now().difference(_lastUplink).inMilliseconds > 300) {
      _sendPacket(
        udp,
        address,
        port,
        type: 3,
        session: _session,
        sequence: _sequence++,
      );
      _talking = false;
    }
  }

  void _sendPacket(
    RawDatagramSocket udp,
    InternetAddress address,
    int port, {
    required int type,
    int flags = 0,
    required int session,
    required int sequence,
    Uint8List? payload,
  }) {
    final data = Uint8List(16 + (payload?.length ?? 0));
    _write32le(data, 0, 0x31444E42);
    data[4] = type;
    data[5] = flags;
    _write16le(data, 6, payload?.length ?? 0);
    _write32le(data, 8, session);
    _write32le(data, 12, sequence);
    if (payload != null) {
      data.setRange(16, data.length, payload);
    }
    udp.send(data, address, port);
  }

  void _handleDownlink(Uint8List data) {
    if (data.length < 16 || _read32le(data, 0) != 0x31444E42) {
      return;
    }
    final type = data[4];
    final flags = data[5];
    final length = _read16le(data, 6);
    if (16 + length > data.length) {
      return;
    }
    _lastServerActivity = DateTime.now();
    if (type == 4 || type == 1) {
      return;
    }
    if (type == 3) {
      node.enqueueNetworkAudio(Int16List(0), true);
      _upsampler.reset();
      return;
    }
    if (type != 2 || length == 0) {
      return;
    }
    if ((flags & 0x01) != 0) {
      _upsampler.reset();
    }
    final pcm8 = Int16List(length);
    for (var i = 0; i < length; i++) {
      pcm8[i] = ulawToLinear(data[16 + i]);
    }
    node.enqueueNetworkAudio(_upsampler.process(pcm8), false);
  }

  static int _read16le(Uint8List data, int offset) {
    return data[offset] | (data[offset + 1] << 8);
  }

  static int _read32le(Uint8List data, int offset) {
    return data[offset] |
        (data[offset + 1] << 8) |
        (data[offset + 2] << 16) |
        (data[offset + 3] << 24);
  }

  static void _write16le(Uint8List data, int offset, int value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
  }

  static void _write32le(Uint8List data, int offset, int value) {
    data[offset] = value & 0xFF;
    data[offset + 1] = (value >> 8) & 0xFF;
    data[offset + 2] = (value >> 16) & 0xFF;
    data[offset + 3] = (value >> 24) & 0xFF;
  }
}

