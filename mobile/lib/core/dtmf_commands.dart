enum NodeCommand {
  none,
  parrotToggle,
  help,
  ident,
  reset,
  echoLinkConnect,
  echoLinkDisconnect,
  echoLinkDial,
  discordJoin,
  discordLeave,
  linkConnect,
  disconnectAll,
}

String nodeCommandName(NodeCommand command) {
  switch (command) {
    case NodeCommand.parrotToggle:
      return 'PARROT';
    case NodeCommand.help:
      return 'HELP';
    case NodeCommand.ident:
      return 'IDENT';
    case NodeCommand.reset:
      return 'RESET';
    case NodeCommand.echoLinkConnect:
      return 'ECHOLINK_CONNECT';
    case NodeCommand.echoLinkDisconnect:
      return 'ECHOLINK_DISCONNECT';
    case NodeCommand.echoLinkDial:
      return 'ECHOLINK_DIAL';
    case NodeCommand.discordJoin:
      return 'DISCORD_JOIN';
    case NodeCommand.discordLeave:
      return 'DISCORD_LEAVE';
    case NodeCommand.linkConnect:
      return 'LINK_CONNECT';
    case NodeCommand.disconnectAll:
      return 'DISCONNECT_ALL';
    case NodeCommand.none:
      return 'NONE';
  }
}

NodeCommand? parseNodeCommand(String text) {
  switch (text.trim().toUpperCase()) {
    case 'PARROT':
    case 'PARROT_TOGGLE':
      return NodeCommand.parrotToggle;
    case 'HELP':
      return NodeCommand.help;
    case 'IDENT':
    case 'ID':
      return NodeCommand.ident;
    case 'RESET':
    case 'CANCEL':
      return NodeCommand.reset;
    case 'ECHOLINK':
    case 'ELINK':
    case 'CONNECT':
      return NodeCommand.echoLinkConnect;
    case 'ECHOLINK_DISCONNECT':
    case 'UNLINK':
    case 'DISCONNECT':
      return NodeCommand.echoLinkDisconnect;
    case 'DISCORD':
    case 'DISCORD_JOIN':
      return NodeCommand.discordJoin;
    case 'DISCORD_LEAVE':
      return NodeCommand.discordLeave;
    case 'LINK':
    case 'LINK_CONNECT':
    case 'NETWORK':
      return NodeCommand.linkConnect;
    case 'DISCONNECT_ALL':
    case 'ALL_LINKS':
    case 'ALLLINK':
    case 'UNLINK_ALL':
      return NodeCommand.disconnectAll;
    default:
      return null;
  }
}

class DtmfCommandResult {
  const DtmfCommandResult(this.command, this.arg, this.completed);

  final NodeCommand command;
  final String arg;
  final bool completed;
}

class DtmfCommandParser {
  final Map<String, NodeCommand> _commands = {};
  String _buffer = '';
  String dialPrefix = '4';

  String prefix = '';
  String suffix = '#';

  String get buffer => _buffer;

  void setCommands(Map<String, NodeCommand> commands) {
    _commands
      ..clear()
      ..addAll(commands);
  }

  void reset() {
    _buffer = '';
  }

  DtmfCommandResult feed(String digit) {
    if (_buffer.isEmpty) {
      if (prefix.isNotEmpty && digit != prefix) {
        return const DtmfCommandResult(NodeCommand.none, '', false);
      }
      if (prefix.isEmpty && digit == suffix) {
        return const DtmfCommandResult(NodeCommand.none, '', false);
      }
      _buffer = digit;
      return const DtmfCommandResult(NodeCommand.none, '', false);
    }

    if (prefix.isNotEmpty && digit == prefix) {
      _buffer = prefix;
      return const DtmfCommandResult(NodeCommand.none, '', false);
    }

    _buffer += digit;
    if (_buffer.length > 32) {
      _buffer = '';
      return const DtmfCommandResult(NodeCommand.none, '', false);
    }

    if (digit != suffix) {
      return const DtmfCommandResult(NodeCommand.none, '', false);
    }

    final completed = _buffer;
    _buffer = '';
    final direct = _commands[completed];
    if (direct != null) {
      return DtmfCommandResult(direct, '', true);
    }

    if (dialPrefix.isNotEmpty &&
        completed.length > dialPrefix.length + 1 &&
        completed.startsWith(dialPrefix) &&
        completed.endsWith(suffix)) {
      final digits =
          completed.substring(dialPrefix.length, completed.length - 1);
      if (digits.isNotEmpty && digits.codeUnits.every(_isDigit)) {
        return DtmfCommandResult(NodeCommand.echoLinkDial, digits, true);
      }
    }
    return const DtmfCommandResult(NodeCommand.none, '', true);
  }

  static bool _isDigit(int codeUnit) => codeUnit >= 0x30 && codeUnit <= 0x39;
}
