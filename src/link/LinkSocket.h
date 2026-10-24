#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace vr76::link {

bool socketStartup(std::string &error);
void socketCleanup();

SOCKET createTcpListener(uint16_t port, std::string &error);
SOCKET createUdpSocket(uint16_t port, std::string &error);
SOCKET connectTcp(const std::string &host, uint16_t port, int timeoutMs,
                  std::string &error);

bool setNonBlocking(SOCKET socket, bool enabled);
bool setReceiveTimeout(SOCKET socket, int timeoutMs);
bool sendAll(SOCKET socket, const void *data, size_t size);
bool recvAll(SOCKET socket, void *data, size_t size, int timeoutMs);
bool sendTo(SOCKET socket, const sockaddr *address, int addressLength,
            const void *data, size_t size);

void closeSocket(SOCKET socket);
std::string endpointToString(const sockaddr_storage &address);

} // namespace vr76::link
