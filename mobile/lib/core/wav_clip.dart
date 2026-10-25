import 'dart:io';
import 'dart:typed_data';

import 'dsp.dart';

class WavClip {
  static Int16List? loadFile(String path, {int targetRate = 32000}) {
    try {
      final file = File(path);
      if (!file.existsSync()) {
        return null;
      }
      return load(file.readAsBytesSync(), targetRate: targetRate);
    } catch (_) {
      return null;
    }
  }

  static Int16List? load(Uint8List bytes, {int targetRate = 32000}) {
    if (bytes.length < 44) {
      return null;
    }
    if (String.fromCharCodes(bytes.sublist(0, 4)) != 'RIFF' ||
        String.fromCharCodes(bytes.sublist(8, 12)) != 'WAVE') {
      return null;
    }

    var offset = 12;
    int? format;
    int? channels;
    int? sampleRate;
    int? bitsPerSample;
    Uint8List? data;

    while (offset + 8 <= bytes.length) {
      final id = String.fromCharCodes(bytes.sublist(offset, offset + 4));
      final size = _readUint32(bytes, offset + 4);
      final body = offset + 8;
      if (size < 0 || body + size > bytes.length) {
        break;
      }
      if (id == 'fmt ' && size >= 16) {
        format = _readUint16(bytes, body);
        channels = _readUint16(bytes, body + 2);
        sampleRate = _readUint32(bytes, body + 4);
        bitsPerSample = _readUint16(bytes, body + 14);
      } else if (id == 'data') {
        data = Uint8List.sublistView(bytes, body, body + size);
      }
      offset = body + size + (size & 1);
    }

    if (format != 1 ||
        channels == null ||
        sampleRate == null ||
        bitsPerSample == null ||
        data == null ||
        channels < 1) {
      return null;
    }

    final sampleCount = bitsPerSample == 8
        ? data.length
        : data.length ~/ 2;
    var samples = Int16List(sampleCount);
    if (bitsPerSample == 8) {
      for (var i = 0; i < data.length; i++) {
        samples[i] = (data[i] - 128) << 8;
      }
    } else if (bitsPerSample == 16) {
      for (var i = 0; i < sampleCount; i++) {
        samples[i] = _readInt16(data, i * 2);
      }
    } else {
      return null;
    }

    samples = toMono16(samples, channels);
    return resampleLinear(samples, sampleRate, targetRate);
  }

  static int _readUint16(Uint8List data, int offset) {
    return data[offset] | (data[offset + 1] << 8);
  }

  static int _readInt16(Uint8List data, int offset) {
    final value = _readUint16(data, offset);
    return value >= 0x8000 ? value - 0x10000 : value;
  }

  static int _readUint32(Uint8List data, int offset) {
    return data[offset] |
        (data[offset + 1] << 8) |
        (data[offset + 2] << 16) |
        (data[offset + 3] << 24);
  }
}
