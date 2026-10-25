import 'dart:async';

import 'package:flutter/services.dart';

class BtDeviceInfo {
  const BtDeviceInfo({
    required this.name,
    required this.address,
    required this.isPaired,
    required this.isConnected,
    required this.serviceUuids,
  });

  final String name;
  final String address;
  final bool isPaired;
  final bool isConnected;
  final List<String> serviceUuids;

  factory BtDeviceInfo.fromMap(Map<Object?, Object?> map) {
    return BtDeviceInfo(
      name: (map['name'] as String?) ?? 'Unknown',
      address: (map['address'] as String?) ?? '',
      isPaired: (map['isPaired'] as bool?) ?? true,
      isConnected: (map['isConnected'] as bool?) ?? false,
      serviceUuids: ((map['serviceUuids'] as List<Object?>?) ?? const [])
          .whereType<String>()
          .toList(growable: false),
    );
  }
}

class BtConnectionEvent {
  const BtConnectionEvent(this.audio, this.event, this.address);

  final bool audio;
  final String event;
  final String address;
}

class BluetoothClassic {
  BluetoothClassic._() {
    _dataChannel.receiveBroadcastStream().listen((event) {
      _handleEvent(event, audio: false);
    });
    _audioChannel.receiveBroadcastStream().listen((event) {
      _handleEvent(event, audio: true);
    });
  }

  static final BluetoothClassic instance = BluetoothClassic._();

  static const _methodChannel =
      MethodChannel('dev.sarahsforge.benshinode/bluetooth_classic');
  static const _dataChannel =
      EventChannel('dev.sarahsforge.benshinode/bluetooth_classic_data');
  static const _audioChannel =
      EventChannel('dev.sarahsforge.benshinode/bluetooth_classic_audio');

  final _controlData = StreamController<Uint8List>.broadcast();
  final _audioData = StreamController<Uint8List>.broadcast();
  final _connectionEvents = StreamController<BtConnectionEvent>.broadcast();

  Stream<Uint8List> get controlData => _controlData.stream;
  Stream<Uint8List> get audioData => _audioData.stream;
  Stream<BtConnectionEvent> get connectionEvents => _connectionEvents.stream;

  void _handleEvent(Object? raw, {required bool audio}) {
    if (raw is! Map) {
      return;
    }
    final event = raw['event'] as String? ?? '';
    final address = (raw['address'] as String? ?? '')
        .toUpperCase()
        .replaceAll('-', ':');
    if (event == 'data') {
      final bytes = raw['data'];
      if (bytes is Uint8List) {
        (audio ? _audioData : _controlData).add(bytes);
      } else if (bytes is List<int>) {
        (audio ? _audioData : _controlData)
            .add(Uint8List.fromList(bytes));
      }
      return;
    }
    _connectionEvents.add(BtConnectionEvent(audio, event, address));
  }

  Future<bool> isAvailable() async {
    return await _methodChannel.invokeMethod<bool>('isAvailable') ?? false;
  }

  Future<void> openBluetoothSettings() async {
    await _methodChannel.invokeMethod<void>('openBluetoothSettings');
  }

  Future<List<BtDeviceInfo>> getPairedDevices() async {
    final result = await _methodChannel
        .invokeMethod<List<Object?>>('getPairedDevices');
    return _devices(result);
  }

  Future<List<BtDeviceInfo>> findCompatibleDevices() async {
    final result = await _methodChannel
        .invokeMethod<List<Object?>>('findCompatibleDevices');
    return _devices(result);
  }

  List<BtDeviceInfo> _devices(List<Object?>? result) {
    return (result ?? const [])
        .whereType<Map<Object?, Object?>>()
        .map(BtDeviceInfo.fromMap)
        .where((device) => device.address.isNotEmpty)
        .toList(growable: false);
  }

  Future<bool> connect(String address) async {
    return await _methodChannel
            .invokeMethod<bool>('connect', {'address': address}) ??
        false;
  }

  Future<bool> connectAudio(String address) async {
    return await _methodChannel
            .invokeMethod<bool>('connectAudio', {'address': address}) ??
        false;
  }
  Future<void> disconnect(String address) async {
    await _methodChannel.invokeMethod<void>('disconnect', {'address': address});
  }

  Future<void> disconnectAudio(String address) async {
    await _methodChannel
        .invokeMethod<void>('disconnectAudio', {'address': address});
  }

  Future<bool> send(String address, Uint8List data) async {
    return await _methodChannel.invokeMethod<bool>(
          'send',
          {'address': address, 'data': data},
        ) ??
        false;
  }

  Future<bool> sendAudio(String address, Uint8List data) async {
    return await _methodChannel.invokeMethod<bool>(
          'sendAudio',
          {'address': address, 'data': data},
        ) ??
        false;
  }
}
