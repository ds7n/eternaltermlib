// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#include "transport.hpp"
#include "ClientConnection.hpp"
#include "TcpSocketHandler.hpp"
#include "Headers.hpp"
#include <sodium.h>  // sodium_memzero — scrub the passkey copy before free

namespace et {

Transport::Transport(std::string host, uint16_t port,
                     std::string id, std::string passkey)
    : host_(std::move(host)), id_(std::move(id)),
      passkey_(std::move(passkey)), port_(port) {
  socketHandler_ = std::make_shared<TcpSocketHandler>();
  SocketEndpoint endpoint;
  endpoint.set_name(host_);
  endpoint.set_port(port_);
  conn_ = std::make_shared<ClientConnection>(socketHandler_, endpoint,
                                             id_, passkey_);
}

Transport::~Transport() {
  if (conn_) conn_->shutdown();
  conn_.reset();
  socketHandler_.reset();
  // Scrub this second in-memory copy of the secretbox key (the session holds
  // the other). std::string's destructor won't zero the buffer on its own.
  if (!passkey_.empty()) sodium_memzero(&passkey_[0], passkey_.size());
}

bool Transport::connect() { return conn_->connect(); }

void Transport::writePacket(uint8_t header, const std::string &payload) {
  conn_->writePacket(Packet(header, payload));
}

bool Transport::hasData() { return conn_->hasData(); }

bool Transport::read(uint8_t *headerOut, std::string *payloadOut) {
  Packet p;
  if (!conn_->read(&p)) return false;
  *headerOut = p.getHeader();
  *payloadOut = p.getPayload();
  return true;
}

int  Transport::socketFd() { return conn_->getSocketFd(); }
void Transport::closeAndMaybeReconnect() { conn_->closeSocketAndMaybeReconnect(); }
void Transport::shutdown() { if (conn_) conn_->shutdown(); }
bool Transport::isShuttingDown() { return conn_ && conn_->isShuttingDown(); }

}  // namespace et
