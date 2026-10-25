import 'package:flutter/foundation.dart';
import 'package:intl/intl.dart';

enum LogLevel { debug, info, warning, error }

class LogEntry {
  LogEntry(this.time, this.level, this.message);

  final DateTime time;
  final LogLevel level;
  final String message;

  String get formatted {
    final stamp = DateFormat('HH:mm:ss.SSS').format(time);
    return '$stamp [${level.name.toUpperCase()}] $message';
  }
}

class NodeLogger extends ChangeNotifier {
  final List<LogEntry> _entries = [];
  final _timeFormat = DateFormat('HH:mm:ss.SSS');

  List<LogEntry> get entries => List.unmodifiable(_entries);

  List<String> recent([int count = 200]) {
    final start = _entries.length > count ? _entries.length - count : 0;
    return _entries
        .sublist(start)
        .map((entry) => entry.formatted)
        .toList(growable: false);
  }

  void clear() {
    _entries.clear();
    notifyListeners();
  }

  void debug(String message) => _add(LogLevel.debug, message);

  void info(String message) => _add(LogLevel.info, message);

  void warning(String message) => _add(LogLevel.warning, message);

  void error(String message) => _add(LogLevel.error, message);

  void _add(LogLevel level, String message) {
    final entry = LogEntry(DateTime.now(), level, message);
    _entries.add(entry);
    if (_entries.length > 500) {
      _entries.removeRange(0, _entries.length - 500);
    }
    debugPrint('${_timeFormat.format(entry.time)} [${level.name}] $message');
    notifyListeners();
  }
}
