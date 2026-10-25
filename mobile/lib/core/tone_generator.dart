import 'dart:math' as math;
import 'dart:typed_data';

class ToneGenerator {
  static const int sampleRate = 32000;

  static Int16List silence(int milliseconds) {
    return Int16List((sampleRate * milliseconds) ~/ 1000);
  }

  static Int16List beep(double frequencyHz, int milliseconds,
      {double levelDb = -16}) {
    final count = (sampleRate * milliseconds) ~/ 1000;
    final out = Int16List(count);
    final amplitude = math.pow(10, levelDb / 20) * 32767;
    for (var i = 0; i < count; i++) {
      final t = i / sampleRate;
      out[i] = (math.sin(2 * math.pi * frequencyHz * t) * amplitude).round();
    }
    return out;
  }

  static Int16List cw(String text, int wpm, double toneHz,
      {double levelDb = -12}) {
    const wpmUnitMs = 1200;
    final unitMs = wpm > 0 ? wpmUnitMs ~/ wpm : 60;
    final builder = <int>[];
    builder.addAll(silence(unitMs));

    final units = text.toUpperCase().split('');
    for (final raw in units) {
      final code = _morse[raw];
      if (code == null) {
        builder.addAll(silence(unitMs));
        continue;
      }
      for (var i = 0; i < code.length; i++) {
        final isDash = code[i] == '-';
        builder.addAll(beep(toneHz, unitMs * (isDash ? 3 : 1),
                levelDb: levelDb));
        if (i != code.length - 1) {
          builder.addAll(silence(unitMs));
        }
      }
      builder.addAll(silence(unitMs * 3));
    }
    return Int16List.fromList(builder);
  }

  static const Map<String, String> _morse = {
    'A': '.-',
    'B': '-...',
    'C': '-.-.',
    'D': '-..',
    'E': '.',
    'F': '..-.',
    'G': '--.',
    'H': '....',
    'I': '..',
    'J': '.---',
    'K': '-.-',
    'L': '.-..',
    'M': '--',
    'N': '-.',
    'O': '---',
    'P': '.--.',
    'Q': '--.-',
    'R': '.-.',
    'S': '...',
    'T': '-',
    'U': '..-',
    'V': '...-',
    'W': '.--',
    'X': '-..-',
    'Y': '-.--',
    'Z': '--..',
    '0': '-----',
    '1': '.----',
    '2': '..---',
    '3': '...--',
    '4': '....-',
    '5': '.....',
    '6': '-....',
    '7': '--...',
    '8': '---..',
    '9': '----.',
    '/': '-..-.',
    '?': '..--..',
    '.': '.-.-.-',
    ',': '--..--',
  };
}
