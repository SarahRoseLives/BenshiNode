import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:flutter_tts/flutter_tts.dart';
import 'package:path_provider/path_provider.dart';

import 'logger.dart';
import 'wav_clip.dart';

class TtsService {
  TtsService(this.logger, {this.rate = 0.4});

  final NodeLogger logger;
  final double rate;
  final FlutterTts _tts = FlutterTts();
  final Map<String, Int16List> _memory = {};
  bool _initialized = false;

  Future<void> _init() async {
    if (_initialized) {
      return;
    }
    _initialized = true;
    try {
      await _tts.setLanguage('en-US');
      await _tts.setSpeechRate(rate);
      await _tts.setPitch(1.0);
      await _tts.setVolume(1.0);
      await _tts.awaitSynthCompletion(true);
    } catch (error) {
      logger.warning('TTS initialization failed: $error');
    }
  }

  Future<Int16List?> synthesize(String text) async {
    final trimmed = text.trim();
    if (trimmed.isEmpty) {
      return null;
    }
    final key = sha256
        .convert('${rate.toStringAsFixed(2)}|$trimmed'.codeUnits)
        .toString();
    final cached = _memory[key];
    if (cached != null) {
      return cached;
    }
    await _init();
    try {
      final directory = await getTemporaryDirectory();
      final path = '${directory.path}${Platform.pathSeparator}tts_$key.wav';
      final file = File(path);
      if (!file.existsSync()) {
        final result = await _tts.synthesizeToFile(trimmed, path, true);
        if (result != 1 || !file.existsSync()) {
          logger.warning('TTS synthesis failed for "$trimmed"');
          return null;
        }
      }
      final pcm = WavClip.loadFile(path);
      if (pcm != null) {
        _memory[key] = pcm;
      } else {
        logger.warning('TTS WAV decode failed for "$trimmed"');
      }
      return pcm;
    } catch (error) {
      logger.warning('TTS synthesis failed: $error');
      return null;
    }
  }
}
