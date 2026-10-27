#include "DtmfCommands.h"

#include <algorithm>
#include <cctype>

namespace vr76 {

const char *nodeCommandName(NodeCommand command) {
  switch (command) {
  case NodeCommand::ParrotToggle:
    return "PARROT";
  case NodeCommand::Help:
    return "HELP";
  case NodeCommand::Ident:
    return "IDENT";
  case NodeCommand::Reset:
    return "RESET";
  case NodeCommand::EchoLinkConnect:
    return "ECHOLINK_CONNECT";
  case NodeCommand::EchoLinkDisconnect:
    return "ECHOLINK_DISCONNECT";
  case NodeCommand::EchoLinkDial:
    return "ECHOLINK_DIAL";
  case NodeCommand::DiscordJoin:
    return "DISCORD_JOIN";
  case NodeCommand::DiscordLeave:
    return "DISCORD_LEAVE";
  case NodeCommand::LinkConnect:
    return "LINK_CONNECT";
  case NodeCommand::AllStarConnect:
    return "ALLSTAR_CONNECT";
  case NodeCommand::AllStarDisconnect:
    return "ALLSTAR_DISCONNECT";
  case NodeCommand::DisconnectAll:
    return "DISCONNECT_ALL";
  case NodeCommand::None:
    break;
  }
  return "NONE";
}

bool parseNodeCommand(const std::string &text, NodeCommand &command) {
  std::string value;
  value.reserve(text.size());
  for (char c : text) {
    value.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  if (value == "PARROT" || value == "PARROT_TOGGLE") {
    command = NodeCommand::ParrotToggle;
    return true;
  }
  if (value == "HELP") {
    command = NodeCommand::Help;
    return true;
  }
  if (value == "IDENT" || value == "ID") {
    command = NodeCommand::Ident;
    return true;
  }
  if (value == "RESET" || value == "CANCEL") {
    command = NodeCommand::Reset;
    return true;
  }
  if (value == "ECHOLINK" || value == "ELINK" || value == "CONNECT") {
    command = NodeCommand::EchoLinkConnect;
    return true;
  }
  if (value == "ECHOLINK_DISCONNECT" || value == "UNLINK" ||
      value == "DISCONNECT") {
    command = NodeCommand::EchoLinkDisconnect;
    return true;
  }
  if (value == "DISCORD" || value == "DISCORD_JOIN") {
    command = NodeCommand::DiscordJoin;
    return true;
  }
  if (value == "DISCORD_LEAVE") {
    command = NodeCommand::DiscordLeave;
    return true;
  }
  if (value == "LINK" || value == "LINK_CONNECT" || value == "NETWORK") {
    command = NodeCommand::LinkConnect;
    return true;
  }
  if (value == "ALLSTAR" || value == "ALLSTAR_CONNECT" ||
      value == "ALLSTAR_LINK") {
    command = NodeCommand::AllStarConnect;
    return true;
  }
  if (value == "ALLSTAR_DISCONNECT" || value == "ASL_UNLINK") {
    command = NodeCommand::AllStarDisconnect;
    return true;
  }
  if (value == "DISCONNECT_ALL" || value == "ALL_LINKS" ||
      value == "ALLLINK" || value == "UNLINK_ALL") {
    command = NodeCommand::DisconnectAll;
    return true;
  }
  return false;
}

bool DtmfCommandParser::feed(char digit, NodeCommand &command,
                             std::string &completedCode) {
  command = NodeCommand::None;
  completedCode.clear();

  if (buffer_.empty()) {
    if (prefix_ != '\0' && digit != prefix_) {
      return false;
    }
    if (prefix_ == '\0' && digit == suffix_) {
      return false;
    }
    buffer_.push_back(digit);
    return false;
  }

  if (prefix_ != '\0' && digit == prefix_) {
    buffer_.assign(1, prefix_);
    return false;
  }

  buffer_.push_back(digit);
  if (buffer_.size() > 32) {
    buffer_.clear();
    return false;
  }

  if (digit != suffix_) {
    return false;
  }

  completedCode = buffer_;
  buffer_.clear();
  auto it = commands_.find(completedCode);
  if (it != commands_.end()) {
    command = it->second;
    return true;
  }

  // Variable-length EchoLink dial: <dialPrefix><digits><suffix>.
  if (!dialPrefix_.empty() && completedCode.size() > dialPrefix_.size() + 1 &&
      completedCode.compare(0, dialPrefix_.size(), dialPrefix_) == 0 &&
      completedCode.back() == suffix_) {
    const std::string digits =
        completedCode.substr(dialPrefix_.size(),
                             completedCode.size() - dialPrefix_.size() - 1);
    const bool allDigits =
        !digits.empty() &&
        std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        });
    if (allDigits) {
      command = NodeCommand::EchoLinkDial;
      completedCode = digits;
    }
  }
  return true;
}

} // namespace vr76
