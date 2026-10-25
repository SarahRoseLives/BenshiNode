import 'dart:typed_data';

class AudioFraming {
  static const int delimiter = 0x7E;
  static const int escape = 0x7D;
  static const int escapeXor = 0x20;

  static const int cmdTxAudio = 0x00;
  static const int cmdAudioEnd = 0x01;
  static const int cmdAudioAck = 0x02;
  static const int cmdRxAudioAlt = 0x03;
  static const int cmdTxEcho = 0x09;

  static Uint8List frame(int command, Uint8List payload) {
    final out = <int>[delimiter, command];
    for (final value in payload) {
      if (value == escape || value == delimiter) {
        out.add(escape);
        out.add(value ^ escapeXor);
      } else {
        out.add(value);
      }
    }
    out.add(delimiter);
    return Uint8List.fromList(out);
  }

  static Uint8List endFrame() {
    return Uint8List.fromList(
      [0x7E, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E],
    );
  }

  static Uint8List unescape(Uint8List payload) {
    final out = <int>[];
    for (var i = 0; i < payload.length; i++) {
      if (payload[i] == escape) {
        if (i + 1 < payload.length) {
          out.add(payload[i + 1] ^ escapeXor);
          i++;
        }
      } else {
        out.add(payload[i]);
      }
    }
    return Uint8List.fromList(out);
  }
}

class AudioFrameParser {
  final List<int> _buffer = [];

  void clear() => _buffer.clear();

  void push(Uint8List data) {
    _buffer.addAll(data);
    if (_buffer.length > 256 * 1024) {
      _buffer.removeRange(0, _buffer.length - 4096);
    }
  }

  Uint8List? next() {
    while (true) {
      if (_buffer.length < 2) {
        return null;
      }
      var start = _buffer.indexOf(AudioFraming.delimiter);
      if (start < 0) {
        _buffer.clear();
        return null;
      }
      if (start > 0) {
        _buffer.removeRange(0, start);
      }
      if (_buffer.length < 2) {
        return null;
      }
      final end = _buffer.indexOf(AudioFraming.delimiter, 1);
      if (end < 0) {
        return null;
      }
      if (end == 1) {
        _buffer.removeAt(0);
        continue;
      }
      final payload = Uint8List.fromList(_buffer.sublist(1, end));
      _buffer.removeRange(0, end + 1);
      final frame = AudioFraming.unescape(payload);
      if (frame.isNotEmpty) {
        return frame;
      }
    }
  }
}
