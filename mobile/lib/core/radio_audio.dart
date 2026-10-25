import 'dart:async';
import 'dart:typed_data';

import '../sbc/sbc_decoder.dart';
import '../sbc/sbc_encoder.dart';
import '../sbc/sbc_enums.dart';
import '../sbc/sbc_frame.dart';
import '../services/bluetooth_classic.dart';
import 'audio_framing.dart';
import 'logger.dart';

class RadioAudio {
  RadioAudio(this.logger, {int bitpool = 18}) {
    _frame.bitpool = bitpool.clamp(16, 53);
  }

  final NodeLogger logger;
  final BluetoothClassic _bt = BluetoothClassic.instance;
  final AudioFrameParser _parser = AudioFrameParser();
  final SbcDecoder _decoder = SbcDecoder();
  final SbcEncoder _encoder = SbcEncoder();
  final SbcFrame _frame = SbcFrame()
    ..frequency = SbcFrequency.freq32K
    ..mode = SbcMode.mono
    ..allocationMethod = SbcBitAllocationMethod.loudness
    ..blocks = 16
    ..subbands = 8
    ..bitpool = 18;
  String? _address;
  StreamSubscription<Uint8List>? _subscription;
  final List<int> _remainder = [];
  bool _txActive = false;
  Future<void> _writeChain = Future.value();

  void Function(Int16List pcm, bool transmit)? onPcm;
  void Function()? onEnd;

  bool get connected => _address != null;

  Future<void> connect(String address) async {
    await disconnect();
    _parser.clear();
    _decoder.reset();
    _encoder.reset();
    _subscription = _bt.audioData.listen((data) {
      _onData(data);
    });
    final ok = await _bt.connectAudio(address);
    if (!ok) {
      await _subscription?.cancel();
      _subscription = null;
      await _bt.disconnectAudio(address);
      return;
    }
    _address = address;
  }

  Future<void> disconnect() async {
    _txActive = false;
    _remainder.clear();
    _parser.clear();
    _subscription?.cancel();
    _subscription = null;
    final address = _address;
    _address = null;
    if (address != null) {
      await _bt.disconnectAudio(address);
    }
  }

  bool beginTx() {
    if (_address == null) {
      return false;
    }
    _remainder.clear();
    _txActive = true;
    return true;
  }

  bool sendPcm(Int16List samples) {
    final address = _address;
    if (address == null || !_txActive || samples.isEmpty) {
      return false;
    }
    _remainder.addAll(samples);
    final samplesPerFrame = _frame.blocks * _frame.subbands;
    while (_remainder.length >= samplesPerFrame) {
      final chunk = Int16List.fromList(_remainder.sublist(0, samplesPerFrame));
      _remainder.removeRange(0, samplesPerFrame);
      final encoded = _encoder.encode(chunk, null, _frame);
      if (encoded == null) {
        logger.warning('SBC encode failed');
        return false;
      }
      _send(address, AudioFraming.frame(AudioFraming.cmdTxAudio, encoded));
    }
    return true;
  }

  Future<bool> endTx() async {
    final address = _address;
    if (address == null) {
      return false;
    }
    if (!_txActive) {
      return true;
    }
    _txActive = false;
    if (_remainder.isNotEmpty) {
      final samplesPerFrame = _frame.blocks * _frame.subbands;
      final count =
          _remainder.length < samplesPerFrame ? _remainder.length : samplesPerFrame;
      final padded = Int16List(samplesPerFrame);
      padded.setRange(0, count, _remainder.sublist(0, count));
      final encoded = _encoder.encode(padded, null, _frame);
      if (encoded != null) {
        _send(address, AudioFraming.frame(AudioFraming.cmdTxAudio, encoded));
      }
      _remainder.clear();
    }
    final end = AudioFraming.endFrame();
    _send(address, end);
    await Future<void>.delayed(const Duration(milliseconds: 80));
    _send(address, end);
    return true;
  }

  void cancelTx() {
    _txActive = false;
    _remainder.clear();
  }

  void _send(String address, Uint8List data) {
    _writeChain = _writeChain.then((_) async {
      final ok = await _bt.sendAudio(address, data);
      if (!ok) {
        logger.warning('Failed to write audio frame');
      }
    });
  }

  void _onData(Uint8List data) {
    _parser.push(data);
    while (true) {
      final frame = _parser.next();
      if (frame == null) {
        return;
      }
      if (frame.isEmpty) {
        continue;
      }
      final command = frame[0];
      switch (command) {
        case AudioFraming.cmdRxAudioAlt:
        case AudioFraming.cmdTxAudio:
          _decode(frame, false);
        case AudioFraming.cmdTxEcho:
          _decode(frame, true);
        case AudioFraming.cmdAudioEnd:
          onEnd?.call();
        case AudioFraming.cmdAudioAck:
          break;
        default:
          logger.debug('Unknown audio command 0x${command.toRadixString(16)}');
      }
    }
  }

  void _decode(Uint8List frame, bool transmit) {
    var offset = 1;
    while (offset + SbcFrame.headerSize <= frame.length) {
      final header = Uint8List.sublistView(
        frame,
        offset,
        frame.length,
      );
      final probe = _decoder.probe(header);
      if (probe == null) {
        return;
      }
      final size = probe.getFrameSize();
      if (size <= 0 || offset + size > frame.length) {
        return;
      }
      final result = _decoder.decode(
        Uint8List.sublistView(frame, offset, offset + size),
      );
      if (!result.success) {
        return;
      }
      final pcm = result.pcmLeft;
      if (pcm.isNotEmpty) {
        onPcm?.call(pcm, transmit);
      }
      offset += size;
    }
  }
}
