import 'dart:math' as math;
import 'dart:typed_data';

class DtmfDecoder {
  static const int sampleRate = 32000;
  static const int blockSize = 512;

  static const _lowFrequencies = [697.0, 770.0, 852.0, 941.0];
  static const _highFrequencies = [1209.0, 1336.0, 1477.0, 1633.0];
  static const _twoPi = 2 * math.pi;

  final List<int> _buffer = [];
  final Float64List _window = Float64List(blockSize);
  final Float64List _lowCoeff = Float64List(4);
  final Float64List _highCoeff = Float64List(4);

  String _activeDigit = '';
  int _activeBlocks = 0;
  int _silenceBlocks = 0;
  bool _reported = false;

  DtmfDecoder() {
    for (var i = 0; i < blockSize; i++) {
      _window[i] =
          0.5 - 0.5 * math.cos(_twoPi * i / (blockSize - 1));
    }
    for (var i = 0; i < 4; i++) {
      _lowCoeff[i] =
          2 * math.cos(_twoPi * _lowFrequencies[i] / sampleRate);
      _highCoeff[i] =
          2 * math.cos(_twoPi * _highFrequencies[i] / sampleRate);
    }
  }

  void reset() {
    _buffer.clear();
    _activeDigit = '';
    _activeBlocks = 0;
    _silenceBlocks = 0;
    _reported = false;
  }

  static String digitForFrequencies(int lowIndex, int highIndex) {
    const table = [
      ['1', '2', '3', 'A'],
      ['4', '5', '6', 'B'],
      ['7', '8', '9', 'C'],
      ['*', '0', '#', 'D'],
    ];
    if (lowIndex < 0 || lowIndex > 3 || highIndex < 0 || highIndex > 3) {
      return '';
    }
    return table[lowIndex][highIndex];
  }

  void process(Int16List samples, void Function(String digit) onDigit) {
    if (samples.isEmpty) {
      return;
    }
    _buffer.addAll(samples);
    while (_buffer.length >= blockSize) {
      final lowAmps = List<double>.filled(4, 0);
      final highAmps = List<double>.filled(4, 0);
      for (var i = 0; i < 4; i++) {
        lowAmps[i] = _goertzel(_buffer, _lowCoeff[i]);
        highAmps[i] = _goertzel(_buffer, _highCoeff[i]);
      }

      var lowIndex = 0;
      var highIndex = 0;
      for (var i = 1; i < 4; i++) {
        if (lowAmps[i] > lowAmps[lowIndex]) lowIndex = i;
        if (highAmps[i] > highAmps[highIndex]) highIndex = i;
      }

      var lowSecond = 0.0;
      var highSecond = 0.0;
      for (var i = 0; i < 4; i++) {
        if (i != lowIndex) lowSecond = math.max(lowSecond, lowAmps[i]);
        if (i != highIndex) highSecond = math.max(highSecond, highAmps[i]);
      }

      final lowPeak = lowAmps[lowIndex];
      final highPeak = highAmps[highIndex];
      final tonePresent = lowPeak > 900 &&
          highPeak > 900 &&
          lowPeak > lowSecond * 1.8 &&
          highPeak > highSecond * 1.8;
      final digit =
          tonePresent ? digitForFrequencies(lowIndex, highIndex) : '';

      if (digit.isNotEmpty) {
        _silenceBlocks = 0;
        if (digit == _activeDigit) {
          _activeBlocks++;
        } else {
          _activeDigit = digit;
          _activeBlocks = 1;
          _reported = false;
        }
        final durationMs = _activeBlocks * blockSize * 1000 ~/ sampleRate;
        if (!_reported && durationMs >= 45) {
          _reported = true;
          onDigit(digit);
        }
      } else {
        _silenceBlocks++;
        if (_silenceBlocks >= 2) {
          _activeDigit = '';
          _activeBlocks = 0;
          _reported = false;
        }
      }

      _buffer.removeRange(0, blockSize);
    }
  }

  double _goertzel(List<int> samples, double coeff) {
    var q0 = 0.0;
    var q1 = 0.0;
    var q2 = 0.0;
    var windowSum = 0.0;
    for (var i = 0; i < blockSize; i++) {
      final window = _window[i];
      windowSum += window;
      q0 = coeff * q1 - q2 + samples[i] * window;
      q2 = q1;
      q1 = q0;
    }
    final power = q1 * q1 + q2 * q2 - coeff * q1 * q2;
    if (power <= 0 || windowSum <= 0) {
      return 0;
    }
    return 2 * math.sqrt(power) / windowSum;
  }
}
