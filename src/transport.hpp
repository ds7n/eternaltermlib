// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#ifndef ETERNALTERMLIB_TRANSPORT_HPP
#define ETERNALTERMLIB_TRANSPORT_HPP
#include <cstdint>
#include <memory>
#include <string>
namespace et {
class ClientConnection;
class SocketHandler;
class Transport {
 public:
  Transport(std::string host, uint16_t port, std::string id, std::string passkey);
  ~Transport();
  bool connect();
  void writePacket(uint8_t header, const std::string &payload);
  bool hasData();
  bool read(uint8_t *headerOut, std::string *payloadOut);
  int  socketFd();
  void closeAndMaybeReconnect();
  void shutdown();
  bool isShuttingDown();
 private:
  std::string host_, id_, passkey_;
  uint16_t port_;
  std::shared_ptr<SocketHandler> socketHandler_;
  std::shared_ptr<ClientConnection> conn_;
};
}  // namespace et
#endif
