import 'dart:math' as math;
import 'dart:typed_data';

class Downsampler32to8 {
  int _sum = 0;
  int _count = 0;

  void reset() {
    _sum = 0;
    _count = 0;
  }

  Int16List process(Int16List samples) {
    final out = <int>[];
    for (final sample in samples) {
      _sum += sample;
      if (++_count == 4) {
        out.add(_sum ~/ 4);
        _sum = 0;
        _count = 0;
      }
    }
    return Int16List.fromList(out);
  }
}

class Upsampler8to32 {
  int _previous = 0;
  bool _havePrevious = false;

  void reset() {
    _previous = 0;
    _havePrevious = false;
  }

  Int16List process(Int16List samples) {
    final out = Int16List(samples.length * 4);
    var index = 0;
    for (final current in samples) {
      final start = _havePrevious ? _previous : current;
      for (var k = 0; k < 4; k++) {
        out[index++] = start + ((current - start) * k ~/ 4);
      }
      _previous = current;
      _havePrevious = true;
    }
    return out;
  }
}

int ulawToLinear(int ulaw) {
  const bias = 0x84;
  final value = (~ulaw) & 0xFF;
  final sign = value & 0x80;
  final exponent = (value >> 4) & 0x07;
  final mantissa = value & 0x0F;
  var magnitude = ((mantissa << 3) + bias) << exponent;
  magnitude -= bias;
  return sign != 0 ? -magnitude : magnitude;
}

int linearToUlaw(int pcm) {
  const bias = 0x84;
  const clip = 32635;
  var sample = pcm;
  var sign = 0;
  if (sample < 0) {
    sample = -sample;
    sign = 0x80;
  }
  if (sample > clip) {
    sample = clip;
  }
  sample += bias;
  var exponent = 7;
  for (var mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
    exponent--;
  }
  final mantissa = (sample >> (exponent + 3)) & 0x0F;
  return (~(sign | (exponent << 4) | mantissa)) & 0xFF;
}

Int16List resampleLinear(Int16List input, int inputRate, int outputRate) {
  if (inputRate == outputRate || input.isEmpty) {
    return input;
  }
  final outputLength =
      (input.length * outputRate / inputRate).round().clamp(0, 1 << 30);
  final out = Int16List(outputLength);
  final ratio = inputRate / outputRate;
  for (var i = 0; i < out.length; i++) {
    final position = i * ratio;
    final index = position.floor();
    final fraction = position - index;
    final a = input[index.clamp(0, input.length - 1)];
    final b = input[(index + 1).clamp(0, input.length - 1)];
    out[i] = (a + (b - a) * fraction).round();
  }
  return out;
}

Int16List toMono16(Int16List samples, int channels) {
  if (channels <= 1) {
    return samples;
  }
  final frames = samples.length ~/ channels;
  final out = Int16List(frames);
  for (var i = 0; i < frames; i++) {
    var sum = 0;
    for (var c = 0; c < channels; c++) {
      sum += samples[i * channels + c];
    }
    out[i] = sum ~/ channels;
  }
  return out;
}

double rmsDb(Int16List samples) {
  if (samples.isEmpty) {
    return -120;
  }
  var sum = 0.0;
  for (final sample in samples) {
    final value = sample / 32768.0;
    sum += value * value;
  }
  final rms = math.sqrt(sum / samples.length);
  return rms <= 0 ? -120 : 20 * (math.log(rms) / math.ln10);
}

Int16List applyGain(Int16List samples, double gainDb) {
  if (gainDb == 0) {
    return samples;
  }
  final gain = math.pow(10, gainDb / 20).toDouble();
  final out = Int16List(samples.length);
  for (var i = 0; i < samples.length; i++) {
    out[i] = (samples[i] * gain).round().clamp(-32768, 32767);
  }
  return out;
}
