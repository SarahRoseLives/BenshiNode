import 'dart:typed_data';

class GaiaFrame {
  GaiaFrame({
    required this.group,
    required this.command,
    required this.isResponse,
    required this.payload,
  });

  final int group;
  final int command;
  final bool isResponse;
  final Uint8List payload;
}

class Gaia {
  static const int groupBasic = 2;
  static const int groupExtended = 10;

  static const int getDevId = 1;
  static const int getDevInfo = 4;
  static const int readStatus = 5;
  static const int registerNotification = 6;
  static const int eventNotification = 9;
  static const int readSettings = 10;
  static const int writeSettings = 11;
  static const int readRfCh = 13;
  static const int writeRfCh = 14;
  static const int getHtStatus = 20;
  static const int setHtOnOff = 21;
  static const int getVolume = 22;
  static const int setVolume = 23;
  static const int readBssSettings = 33;
  static const int writeBssSettings = 34;
  static const int freqModeSetPar = 35;
  static const int freqModeGetStatus = 36;
  static const int doProgFunc = 66;

  static const int notifHtStatusChanged = 1;
  static const int notifHtChChanged = 5;
  static const int notifHtSettingsChanged = 6;
  static const int notifFreqModeStatusChanged = 14;

  static Uint8List buildCommand(int group, int command,
      [List<int> data = const []]) {
    final result = Uint8List(4 + data.length);
    result[0] = (group >> 8) & 0xFF;
    result[1] = group & 0xFF;
    result[2] = (command >> 8) & 0xFF;
    result[3] = command & 0xFF;
    result.setRange(4, result.length, data);
    return result;
  }

  static Uint8List buildCommandByte(int group, int command, int value) {
    return buildCommand(group, command, [value]);
  }

  static Uint8List encode(Uint8List command) {
    if (command.length < 4) {
      return Uint8List(0);
    }
    final result = Uint8List(command.length + 4);
    result[0] = 0xFF;
    result[1] = 0x01;
    result[2] = 0x00;
    result[3] = (command.length - 4) & 0xFF;
    result.setRange(4, result.length, command);
    return result;
  }

  static GaiaFrame? decodeCommand(Uint8List data) {
    if (data.length < 4) {
      return null;
    }
    final group = (data[0] << 8) | data[1];
    final rawCommand = (data[2] << 8) | data[3];
    return GaiaFrame(
      group: group,
      command: rawCommand & 0x7FFF,
      isResponse: (rawCommand & 0x8000) != 0,
      payload: Uint8List.sublistView(data, 4),
    );
  }

  static int getByte(Uint8List data, int offset) {
    return offset >= 0 && offset < data.length ? data[offset] : 0;
  }

  static int getShort(Uint8List data, int offset) {
    if (offset < 0 || offset + 1 >= data.length) {
      return 0;
    }
    return (data[offset] << 8) | data[offset + 1];
  }

  static int getInt(Uint8List data, int offset) {
    if (offset < 0 || offset + 3 >= data.length) {
      return 0;
    }
    return (data[offset] << 24) |
        (data[offset + 1] << 16) |
        (data[offset + 2] << 8) |
        data[offset + 3];
  }
}

class GaiaDecoder {
  final List<int> _buffer = [];

  void push(Uint8List data) {
    if (data.isEmpty) {
      return;
    }
    _buffer.addAll(data);
    if (_buffer.length > 1024 * 1024) {
      _buffer.removeRange(0, _buffer.length - 4096);
    }
  }

  GaiaFrame? next() {
    while (true) {
      if (_buffer.length < 2) {
        return null;
      }
      if (_buffer[0] != 0xFF || _buffer[1] != 0x01) {
        _buffer.removeAt(0);
        continue;
      }
      if (_buffer.length < 4) {
        return null;
      }
      final payloadLength = _buffer[3];
      final checksum = _buffer[2] & 0x01;
      final totalLength = payloadLength + 8 + checksum;
      if (totalLength < 8 || totalLength > 65535) {
        _buffer.removeAt(0);
        continue;
      }
      if (_buffer.length < totalLength) {
        return null;
      }
      final body = Uint8List.fromList(
        _buffer.sublist(4, payloadLength + 8),
      );
      final frame = Gaia.decodeCommand(body);
      _buffer.removeRange(0, totalLength);
      if (frame != null) {
        return frame;
      }
    }
  }

  void clear() => _buffer.clear();
}
