import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:benshinode/core/wav_clip.dart';
import 'package:benshinode/sbc/sbc_decoder.dart';
import 'package:benshinode/sbc/sbc_encoder.dart';
import 'package:benshinode/sbc/sbc_enums.dart';
import 'package:benshinode/sbc/sbc_frame.dart';

void main(List<String> args) {
  if (args.length < 2) {
    stderr.writeln('usage: codec_check <in.wav> <out.wav> [bitpool]');
    exit(2);
  }
  final bitpool = args.length > 2 ? int.parse(args[2]) : 40;
  final input = WavClip.loadFile(args[0]);
  if (input == null) {
    stderr.writeln('could not load ${args[0]}');
    exit(1);
  }
  final frame = SbcFrame()
    ..frequency = SbcFrequency.freq32K
    ..mode = SbcMode.mono
    ..allocationMethod = SbcBitAllocationMethod.loudness
    ..blocks = 16
    ..subbands = 8
    ..bitpool = bitpool.clamp(16, 53);
  final encoder = SbcEncoder();
  final decoder = SbcDecoder();
  final decoded = <int>[];
  final samplesPerFrame = frame.blocks * frame.subbands;
  for (var offset = 0; offset + samplesPerFrame <= input.length; offset += samplesPerFrame) {
    final chunk = Int16List.fromList(
      input.sublist(offset, offset + samplesPerFrame),
    );
    final encoded = encoder.encode(chunk, null, frame);
    if (offset == 0 && encoded != null) {
      stdout.writeln(
        'header=${encoded.sublist(0, 4).map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ')}',
      );
    }
    if (encoded == null) {
      stderr.writeln('encode failed at $offset');
      exit(1);
    }
    final result = decoder.decode(encoded);
    if (!result.success) {
      stderr.writeln('decode failed at $offset');
      exit(1);
    }
    decoded.addAll(result.pcmLeft);
  }
  writeWav(args[1], Int16List.fromList(decoded));
  final snr = bestSnr(input, Int16List.fromList(decoded));
  stdout.writeln(
    'bitpool=$bitpool input=${input.length} decoded=${decoded.length} '
    'bestSnr=${snr.toStringAsFixed(2)} dB',
  );
}

double bestSnr(Int16List a, Int16List b) {
  var best = -999.0;
  for (var delay = 0; delay <= 200; delay++) {
    final length = math.min(a.length, b.length - delay);
    if (length < 1000) {
      continue;
    }
    var signal = 0.0;
    var noise = 0.0;
    for (var i = 0; i < length; i++) {
      final x = a[i].toDouble();
      final y = b[i + delay].toDouble();
      signal += x * x;
      final e = x - y;
      noise += e * e;
    }
    if (noise <= 0) {
      return 99.0;
    }
    final snr = 10 * math.log(signal / noise) / math.ln10;
    if (snr > best) {
      best = snr;
    }
  }
  return best;
}

void writeWav(String path, Int16List samples) {
  const rate = 32000;
  final dataLength = samples.length * 2;
  final bytes = BytesBuilder(copy: false);
  void ascii(String value) => bytes.add(value.codeUnits);
  void u16(int value) => bytes.add([value & 0xFF, (value >> 8) & 0xFF]);
  void u32(int value) => bytes.add([
        value & 0xFF,
        (value >> 8) & 0xFF,
        (value >> 16) & 0xFF,
        (value >> 24) & 0xFF,
      ]);
  ascii('RIFF');
  u32(36 + dataLength);
  ascii('WAVE');
  ascii('fmt ');
  u32(16);
  u16(1);
  u16(1);
  u32(rate);
  u32(rate * 2);
  u16(2);
  u16(16);
  ascii('data');
  u32(dataLength);
  for (final sample in samples) {
    bytes.add([sample & 0xFF, (sample >> 8) & 0xFF]);
  }
  File(path).writeAsBytesSync(bytes.takeBytes());
}
