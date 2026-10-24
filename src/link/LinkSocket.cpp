#include "LinkSocket.h"

#include <algorithm>
#include <cstring>

namespace vr76::link {

namespace {

bool startup(std::string &error) {
  static bool started = false;
  if (started) {
    return true;
  }
  WSADATA data{};
  const int result = WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    error = "WSAStartup failed: " + std::to_string(result);
    return false;
  }
  started = true;
  return true;
}

} // namespace

bool socketStartup(std::string &error) { return startup(error); }

void socketCleanup() { WSACleanup(); }

SOCKET createTcpListener(uint16_t port, std::string &error) {
  if (!startup(error)) {
    return INVALID_SOCKET;
  }
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    error = "socket(TCP) failed: " + std::to_string(WSAGetLastError());
    return INVALID_SOCKET;
  }

  BOOL reuse = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
      SOCKET_ERROR) {
    error = "bind(TCP " + std::to_string(port) +
            ") failed: " + std::to_string(WSAGetLastError());
    closeSocket(listener);
    return INVALID_SOCKET;
  }
  if (listen(listener, SOMAXCONN) == SOCKET_ERROR) {
    error = "listen failed: " + std::to_string(WSAGetLastError());
    closeSocket(listener);
    return INVALID_SOCKET;
  }
  u_long nonBlocking = 1;
  ioctlsocket(listener, FIONBIO, &nonBlocking);
  return listener;
}

SOCKET createUdpSocket(uint16_t port, std::string &error) {
  if (!startup(error)) {
    return INVALID_SOCKET;
  }
  SOCKET udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udp == INVALID_SOCKET) {
    error = "socket(UDP) failed: " + std::to_string(WSAGetLastError());
    return INVALID_SOCKET;
  }
  BOOL reuse = TRUE;
  setsockopt(udp, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char *>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (bind(udp, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
      SOCKET_ERROR) {
    error = "bind(UDP " + std::to_string(port) +
            ") failed: " + std::to_string(WSAGetLastError());
    closeSocket(udp);
    return INVALID_SOCKET;
  }
  u_long nonBlocking = 1;
  ioctlsocket(udp, FIONBIO, &nonBlocking);
  return udp;
}

SOCKET connectTcp(const std::string &host, uint16_t port, int timeoutMs,
                  std::string &error) {
  if (!startup(error)) {
    return INVALID_SOCKET;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *result = nullptr;
  const std::string portText = std::to_string(port);
  if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 ||
      result == nullptr) {
    error = "Could not resolve " + host;
    return INVALID_SOCKET;
  }

  SOCKET socketHandle = socket(result->ai_family, result->ai_socktype,
                               result->ai_protocol);
  if (socketHandle == INVALID_SOCKET) {
    freeaddrinfo(result);
    error = "socket(TCP) failed: " + std::to_string(WSAGetLastError());
    return INVALID_SOCKET;
  }

  u_long nonBlocking = 1;
  ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
  const int connectResult =
      connect(socketHandle, result->ai_addr, static_cast<int>(result->ai_addrlen));
  freeaddrinfo(result);

  if (connectResult == SOCKET_ERROR) {
    const int code = WSAGetLastError();
    if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
      closeSocket(socketHandle);
      error = "connect failed: " + std::to_string(code);
      return INVALID_SOCKET;
    }

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(socketHandle, &writeSet);
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    const int selected =
        select(0, nullptr, &writeSet, nullptr, &timeout);
    if (selected <= 0) {
      closeSocket(socketHandle);
      error = "connect timed out";
      return INVALID_SOCKET;
    }
    int socketError = 0;
    int length = sizeof(socketError);
    if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char *>(&socketError), &length) ==
            SOCKET_ERROR ||
        socketError != 0) {
      closeSocket(socketHandle);
      error = "connect failed: " + std::to_string(socketError);
      return INVALID_SOCKET;
    }
  }

  nonBlocking = 0;
  ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
  return socketHandle;
}

bool setNonBlocking(SOCKET socketHandle, bool enabled) {
  u_long value = enabled ? 1 : 0;
  return ioctlsocket(socketHandle, FIONBIO, &value) == 0;
}

bool setReceiveTimeout(SOCKET socketHandle, int timeoutMs) {
  DWORD timeout = static_cast<DWORD>(std::max(0, timeoutMs));
  return setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char *>(&timeout),
                    sizeof(timeout)) == 0;
}

bool sendAll(SOCKET socketHandle, const void *data, size_t size) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  size_t sent = 0;
  while (sent < size) {
    const int result =
        send(socketHandle, reinterpret_cast<const char *>(bytes + sent),
             static_cast<int>(size - sent), 0);
    if (result <= 0) {
      return false;
    }
    sent += static_cast<size_t>(result);
  }
  return true;
}

bool recvAll(SOCKET socketHandle, void *data, size_t size, int timeoutMs) {
  setReceiveTimeout(socketHandle, timeoutMs);
  uint8_t *bytes = static_cast<uint8_t *>(data);
  size_t received = 0;
  while (received < size) {
    const int result =
        recv(socketHandle, reinterpret_cast<char *>(bytes + received),
             static_cast<int>(size - received), 0);
    if (result <= 0) {
      return false;
    }
    received += static_cast<size_t>(result);
  }
  return true;
}

bool sendTo(SOCKET socketHandle, const sockaddr *address, int addressLength,
            const void *data, size_t size) {
  return sendto(socketHandle, reinterpret_cast<const char *>(data),
                static_cast<int>(size), 0, address, addressLength) ==
         static_cast<int>(size);
}

void closeSocket(SOCKET socketHandle) {
  if (socketHandle != INVALID_SOCKET) {
    closesocket(socketHandle);
  }
}

std::string endpointToString(const sockaddr_storage &address) {
  char host[INET6_ADDRSTRLEN] = {};
  uint16_t port = 0;
  if (address.ss_family == AF_INET) {
    const auto *ipv4 = reinterpret_cast<const sockaddr_in *>(&address);
    inet_ntop(AF_INET, &ipv4->sin_addr, host, sizeof(host));
    port = ntohs(ipv4->sin_port);
  } else if (address.ss_family == AF_INET6) {
    const auto *ipv6 = reinterpret_cast<const sockaddr_in6 *>(&address);
    inet_ntop(AF_INET6, &ipv6->sin6_addr, host, sizeof(host));
    port = ntohs(ipv6->sin6_port);
  }
  return std::string(host) + ":" + std::to_string(port);
}

} // namespace vr76::link
