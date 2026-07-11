// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
//
// Control shim for the breakable TCP relay used by the roaming/replay test.
//
// The relay is `shopify/toxiproxy` (a compose service under the `integration`
// profile, see docker-compose.yml). Toxiproxy is purpose-built for exactly
// this scenario -- injecting a hard connection cut between a client and an
// upstream -- so the roaming test can sever the TCP path mid-stream and later
// restore it, forcing ET's reconnect+replay to kick in.
//
// Wiring: the client (baseCfgThroughProxy in test_roaming.cpp) connects to
// `toxiproxy:2022`; toxiproxy forwards to `etserver:2022`. This shim drives
// toxiproxy's HTTP control API (default `toxiproxy:8474`):
//   - setup():  (re)create the proxy `et`, listen 0.0.0.0:2022 -> etserver:2022
//   - pause():  disable the proxy -- CLOSES the listener AND drops the live
//               connection, so the client's socket fd goes dead (our client
//               then emits ET_STATE_ROAMING and ET spawns its reconnect thread)
//   - resume(): re-enable the proxy -- the reconnect thread re-establishes and
//               ET replays the backed buffer, so the missed bytes arrive
//               exactly once, in order.
//
// The control transport is a minimal raw-socket HTTP/1.1 client (no external
// HTTP dependency added to the test binary); it POSTs tiny JSON bodies and
// parses only the status line. Any control-plane failure throws
// std::runtime_error so a test that cannot actually sever/restore the link
// fails loudly rather than silently asserting against a link that was never
// cut.
#ifndef ETERNALTERMLIB_TEST_PROXY_CONTROL_HPP
#define ETERNALTERMLIB_TEST_PROXY_CONTROL_HPP

#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace proxy {

namespace detail {

// host:port of toxiproxy's HTTP control API. Overridable for local runs.
inline std::string controlHostPort() {
  const char *v = getenv("TOXIPROXY_URL");
  return v ? v : "toxiproxy:8474";
}

// Upstream the proxy forwards to (the real etserver on the compose network).
inline std::string upstream() {
  const char *v = getenv("ET_UPSTREAM");
  return v ? v : "etserver:2022";
}

// The address the proxy itself listens on inside the compose network. The
// client connects here (host = toxiproxy, port = 2022). 0.0.0.0 so it accepts
// from the dev-integration container.
inline std::string listen() {
  const char *v = getenv("ET_PROXY_LISTEN");
  return v ? v : "0.0.0.0:2022";
}

inline void split(const std::string &hp, std::string *host, std::string *port) {
  auto c = hp.rfind(':');
  if (c == std::string::npos)
    throw std::runtime_error("proxy: malformed host:port '" + hp + "'");
  *host = hp.substr(0, c);
  *port = hp.substr(c + 1);
}

// Open a TCP connection to host:port, resolving via getaddrinfo. Throws on
// failure. Returns a connected fd the caller must close().
inline int dial(const std::string &host, const std::string &port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (gai != 0)
    throw std::runtime_error("proxy: getaddrinfo(" + host + ":" + port +
                             ") failed: " + gai_strerror(gai));
  int fd = -1;
  for (addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0)
    throw std::runtime_error("proxy: connect to control API " + host + ":" +
                             port + " failed");
  return fd;
}

// Read the full HTTP response off `fd` until the peer closes (toxiproxy sends
// Connection: close for these small control requests) or the buffer is done.
inline std::string readAll(int fd) {
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof buf)) > 0) out.append(buf, (size_t)n);
  return out;
}

// Parse the numeric status code out of an HTTP/1.x status line
// ("HTTP/1.1 200 OK\r\n..."). Returns -1 if it can't be parsed.
inline int statusCode(const std::string &resp) {
  // Expect "HTTP/1.x NNN ..."
  auto sp = resp.find(' ');
  if (sp == std::string::npos) return -1;
  auto sp2 = resp.find(' ', sp + 1);
  std::string code = resp.substr(sp + 1, (sp2 == std::string::npos)
                                             ? std::string::npos
                                             : sp2 - sp - 1);
  try {
    return std::stoi(code);
  } catch (...) {
    return -1;
  }
}

// Perform one HTTP request against the control API. `method` is "POST"/"GET"/
// "DELETE"; `path` like "/proxies"; `body` is the JSON payload (may be empty).
// Returns the numeric HTTP status code. Throws on transport failure.
inline int request(const std::string &method, const std::string &path,
                   const std::string &body) {
  std::string host, port;
  split(controlHostPort(), &host, &port);
  int fd = dial(host, port);

  std::string req = method + " " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "Content-Type: application/json\r\n";
  req += "Connection: close\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n";
  req += body;

  size_t sent = 0;
  while (sent < req.size()) {
    ssize_t w = ::write(fd, req.data() + sent, req.size() - sent);
    if (w <= 0) {
      close(fd);
      throw std::runtime_error("proxy: write to control API failed");
    }
    sent += (size_t)w;
  }
  std::string resp = readAll(fd);
  close(fd);
  int code = statusCode(resp);
  if (code < 0)
    throw std::runtime_error("proxy: unparseable control-API response: " +
                             resp.substr(0, 80));
  return code;
}

// Flip the proxy's `enabled` flag. enabled=false severs the link (drops the
// live connection + closes the listener); enabled=true restores it.
inline void setEnabled(bool enabled) {
  std::string body = std::string("{\"enabled\":") +
                     (enabled ? "true" : "false") + "}";
  int code = request("POST", "/proxies/et", body);
  if (code != 200)
    throw std::runtime_error("proxy: setEnabled(" +
                             std::string(enabled ? "true" : "false") +
                             ") got HTTP " + std::to_string(code));
}

}  // namespace detail

// (Re)create the `et` proxy: listen -> upstream, enabled. Idempotent: if a
// proxy of that name already exists (e.g. a prior run), it is deleted first so
// the roaming test always starts from a clean, enabled relay. Throws on any
// control-plane failure so a test can never proceed against a proxy that was
// never actually established.
inline void setup() {
  // Best-effort delete of a stale proxy from a previous run. 404 (not found)
  // is fine; only a transport failure throws (out of request()).
  detail::request("DELETE", "/proxies/et", "");

  std::string body = std::string("{\"name\":\"et\",\"listen\":\"") +
                     detail::listen() + "\",\"upstream\":\"" +
                     detail::upstream() + "\",\"enabled\":true}";
  int code = detail::request("POST", "/proxies", body);
  if (code != 201 && code != 200)
    throw std::runtime_error("proxy: setup() create got HTTP " +
                             std::to_string(code));
}

// Sever the TCP path: disable the proxy. Drops the live connection so the
// client's socket dies and ET begins roaming.
inline void pause() { detail::setEnabled(false); }

// Restore the TCP path: re-enable the proxy so ET's reconnect thread can
// re-establish and replay the backed buffer.
inline void resume() { detail::setEnabled(true); }

}  // namespace proxy

#endif  // ETERNALTERMLIB_TEST_PROXY_CONTROL_HPP
