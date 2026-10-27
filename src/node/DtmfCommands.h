#pragma once

#include <map>
#include <string>

namespace vr76 {

enum class NodeCommand {
  None,
  ParrotToggle,
  Help,
  Ident,
  Reset,
  EchoLinkConnect,
  EchoLinkDisconnect,
  EchoLinkDial,
  DiscordJoin,
  DiscordLeave,
  LinkConnect,
  AllStarConnect,
  AllStarDisconnect,
  DisconnectAll
};

const char *nodeCommandName(NodeCommand command);
bool parseNodeCommand(const std::string &text, NodeCommand &command);

class DtmfCommandParser {
public:
  void setCommands(const std::map<std::string, NodeCommand> &commands) {
    commands_ = commands;
  }
  void setPrefix(char prefix) { prefix_ = prefix; }
  void setSuffix(char suffix) { suffix_ = suffix; }
  void setDialPrefix(const std::string &prefix) { dialPrefix_ = prefix; }
  char prefix() const { return prefix_; }
  char suffix() const { return suffix_; }

  bool feed(char digit, NodeCommand &command, std::string &completedCode);
  void reset() { buffer_.clear(); }
  const std::string &buffer() const { return buffer_; }

private:
  std::map<std::string, NodeCommand> commands_;
  std::string buffer_;
  std::string dialPrefix_ = "4";
  char prefix_ = '*';
  char suffix_ = '#';
};

} // namespace vr76
