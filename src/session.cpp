// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
//
// ETerminal-protocol session: the pure payload builders + the internal
// transport thread that drives handshake, byte streaming, keepalive, and
// roaming. This is the ONLY first-party translation unit that touches ET's
// generated proto types; shim.cpp stays proto-free.
//
// The loop is a transcription of upstream TerminalClient::run, with ET's
// console fd replaced by a self-pipe + outbound queue: et_send/et_resize/
// et_close enqueue work and write one byte to the pipe to wake select().
#include "session_internal.hpp"

namespace et {

std::string buildInitialPayload(const char *const *keys,
                                const char *const *vals, size_t count) {
  InitialPayload p;
  p.set_jumphost(false);
  for (size_t i = 0; i < count; i++)
    (*p.mutable_environmentvariables())[keys[i]] = vals[i];
  return protoToString(p);
}

std::string buildTerminalBuffer(const uint8_t *buf, size_t len) {
  TerminalBuffer tb;
  tb.set_buffer(std::string(reinterpret_cast<const char *>(buf), len));
  return protoToString(tb);
}

std::string buildTerminalInfo(uint16_t cols, uint16_t rows,
                              uint16_t width, uint16_t height) {
  TerminalInfo ti;
  ti.set_column(cols);
  ti.set_row(rows);
  ti.set_width(width);
  ti.set_height(height);
  return protoToString(ti);
}

}  // namespace et

#include "session.hpp"
#include "transport.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <sys/select.h>

namespace {
struct OutItem {
  uint8_t header;
  std::string payload;
};
}  // namespace

struct et_session {
  et_callbacks cbs;
  void *ctx;
  std::string host, id, passkey;
  uint16_t port, cols, rows, width, height;
  int keepalive_secs;
  std::string initialPayload;  // prebuilt env map
  std::unique_ptr<et::Transport> transport;
  std::thread thr;
  std::mutex qmutex;
  std::deque<OutItem> outq;
  int wake_r = -1, wake_w = -1;  // self-pipe
  std::atomic<bool> stopping{false};
  et_state lastState = ET_STATE_CONNECTING;

  void emitState(et_state s) {
    if (s != lastState) {
      lastState = s;
      cbs.on_state(ctx, s);
    }
  }
  void wake() {
    char b = 1;
    ssize_t n = ::write(wake_w, &b, 1);
    (void)n;
  }
  void enqueue(uint8_t h, std::string p) {
    {
      std::lock_guard<std::mutex> g(qmutex);
      outq.push_back({h, std::move(p)});
    }
    wake();
  }
  void run();  // the loop
};

using namespace et;

namespace {
// Live-handle registry: the idempotency latch for et_session_close.
//
// The teardown latch CANNOT live inside et_session, because et_session_close's
// last act is `delete s` -- a second et_session_close(s) call on the same
// (now-dangling) pointer would then read/write the freed latch, a
// heap-use-after-free (caught by TSan/ASan on the CloseIsIdempotent path).
// Keeping the set of still-live handles OUTSIDE the freed object lets the
// second call observe "already torn down" by pointer identity alone, without
// ever dereferencing freed memory. A plain mutex-guarded set (not a lock-free
// structure) is sufficient: close is a rare, non-hot-path operation.
std::mutex g_liveMutex;
std::unordered_set<et_session *> g_liveSessions;

// Register a freshly-started session as live. Called from et_session_start.
void registerLive(et_session *s) {
  std::lock_guard<std::mutex> g(g_liveMutex);
  g_liveSessions.insert(s);
}

// Atomically claim `s` for teardown: returns true exactly once (for the first
// caller), false for every later call on the same pointer. Removing from the
// set under the lock is the sole synchronization point that makes et_close
// idempotent by pointer identity without touching freed memory.
bool claimForClose(et_session *s) {
  std::lock_guard<std::mutex> g(g_liveMutex);
  return g_liveSessions.erase(s) == 1;
}
}  // namespace

void et_session::run() {
  emitState(ET_STATE_CONNECTING);
  if (!transport->connect()) {
    cbs.on_end(ctx, "connect failed");
    emitState(ET_STATE_DISCONNECTED);
    return;
  }

  // Handshake: send INITIAL_PAYLOAD, await INITIAL_RESPONSE (3 tries, 1s each).
  transport->writePacket(EtPacketType::INITIAL_PAYLOAD, initialPayload);
  bool ok = false;
  for (int a = 0; a < 3 && !ok; a++) {
    int fd = transport->socketFd();
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(fd, &rfd);
    timeval tv{1, 0};
    select(fd + 1, &rfd, nullptr, nullptr, &tv);
    if (FD_ISSET(fd, &rfd) && transport->hasData()) {
      uint8_t h;
      std::string pl;
      if (transport->read(&h, &pl)) {
        if (h != EtPacketType::INITIAL_RESPONSE) {
          cbs.on_end(ctx, "missing initial response");
          emitState(ET_STATE_DISCONNECTED);
          return;
        }
        auto resp = stringToProto<InitialResponse>(pl);
        if (resp.has_error()) {
          cbs.on_end(ctx, ("handshake rejected: " + resp.error()).c_str());
          emitState(ET_STATE_DISCONNECTED);
          return;
        }
        ok = true;
      }
    }
  }
  if (!ok) {
    cbs.on_end(ctx, "handshake timeout");
    emitState(ET_STATE_DISCONNECTED);
    return;
  }

  // Initial window size, then CONNECTED.
  transport->writePacket(TerminalPacketType::TERMINAL_INFO,
                         buildTerminalInfo(cols, rows, width, height));
  emitState(ET_STATE_CONNECTED);

  time_t keepaliveTime = time(nullptr) + keepalive_secs;
  bool waitingOnKeepalive = false;

  while (!stopping.load() && !transport->isShuttingDown()) {
    int fd = transport->socketFd();
    fd_set rfd;
    FD_ZERO(&rfd);
    FD_SET(wake_r, &rfd);
    int maxfd = wake_r;
    if (fd >= 0) {
      FD_SET(fd, &rfd);
      if (fd > maxfd) maxfd = fd;
    } else if (lastState == ET_STATE_CONNECTED) {
      emitState(ET_STATE_ROAMING);
    }
    timeval tv{0, 10000};
    select(maxfd + 1, &rfd, nullptr, nullptr, &tv);

    if (FD_ISSET(wake_r, &rfd)) {
      char drain[64];
      while (::read(wake_r, drain, sizeof drain) > 0) {
      }
    }

    // Reconnected? ET replays the backed buffer transparently.
    if (fd >= 0 && lastState == ET_STATE_ROAMING) emitState(ET_STATE_CONNECTED);

    // Inbound: coalesce all available TERMINAL_BUFFER into one on_bytes.
    if (fd >= 0 && FD_ISSET(fd, &rfd)) {
      std::string coalesced;
      while (transport->hasData()) {
        uint8_t h;
        std::string pl;
        if (!transport->read(&h, &pl)) break;
        keepaliveTime = time(nullptr) + keepalive_secs;
        if (h == TerminalPacketType::TERMINAL_BUFFER)
          coalesced += stringToProto<TerminalBuffer>(pl).buffer();
        else if (h == TerminalPacketType::KEEP_ALIVE)
          waitingOnKeepalive = false;
        // else: PORT_FORWARD_* / unknown -> drop (out of scope).
      }
      if (!coalesced.empty())
        cbs.on_bytes(ctx, reinterpret_cast<const uint8_t *>(coalesced.data()),
                     coalesced.size());
    }

    // Outbound queue.
    for (;;) {
      OutItem item;
      {
        std::lock_guard<std::mutex> g(qmutex);
        if (outq.empty()) break;
        item = std::move(outq.front());
        outq.pop_front();
      }
      transport->writePacket(item.header, item.payload);
      keepaliveTime = time(nullptr) + keepalive_secs;
    }

    // Keepalive / roaming trigger.
    if (fd >= 0 && keepaliveTime < time(nullptr)) {
      keepaliveTime = time(nullptr) + keepalive_secs;
      if (waitingOnKeepalive) {
        transport->closeAndMaybeReconnect();
        waitingOnKeepalive = false;
        emitState(ET_STATE_ROAMING);
      } else {
        transport->writePacket(TerminalPacketType::KEEP_ALIVE, "");
        waitingOnKeepalive = true;
      }
    }
    if (fd < 0) waitingOnKeepalive = false;
  }
  cbs.on_end(ctx, stopping.load() ? "closed" : "session ended");
  emitState(ET_STATE_DISCONNECTED);
}

et_session *et_session_start(const et_config *cfg, const et_callbacks *cbs,
                             void *ctx) {
  auto *s = new et_session();
  s->cbs = *cbs;
  s->ctx = ctx;
  s->host = cfg->host;
  s->id = cfg->id;
  s->passkey = cfg->passkey;
  s->port = cfg->port ? cfg->port : 2022;
  s->cols = cfg->cols;
  s->rows = cfg->rows;
  s->width = cfg->width;
  s->height = cfg->height;
  s->keepalive_secs = cfg->keepalive_secs > 0 ? cfg->keepalive_secs : 5;
  s->initialPayload =
      buildInitialPayload(cfg->env_keys, cfg->env_vals, cfg->env_count);
  int p[2];
  if (pipe(p) != 0) {
    delete s;
    return nullptr;
  }
  s->wake_r = p[0];
  s->wake_w = p[1];
  // wake_r MUST be non-blocking: the main loop's drain (`while (read(wake_r,
  // ...) > 0)`) keeps reading until it sees a non-positive return to know
  // it's caught up. On a blocking pipe with the write end (wake_w) still
  // open, that last read() would instead block forever waiting for more
  // data (a pipe only returns 0/EOF once ALL writers close) -- wedging the
  // transport thread, and therefore et_close()'s thr.join(), permanently.
  // Reproduced against the real etserver fixture: a real CONNECTED session
  // followed by et_close() hung indefinitely until this fd was made
  // non-blocking.
  fcntl(s->wake_r, F_SETFL, O_NONBLOCK);
  s->transport.reset(new Transport(s->host, s->port, s->id, s->passkey));
  registerLive(s);
  s->thr = std::thread([s] { s->run(); });
  return s;
}

int et_session_send(et_session *s, const uint8_t *buf, size_t len) {
  if (s->stopping.load()) return ET_ERR_CLOSED;
  s->enqueue(TerminalPacketType::TERMINAL_BUFFER, buildTerminalBuffer(buf, len));
  return (int)len;
}

int et_session_resize(et_session *s, uint16_t cols, uint16_t rows,
                      uint16_t width, uint16_t height) {
  if (s->stopping.load()) return ET_ERR_CLOSED;
  s->enqueue(TerminalPacketType::TERMINAL_INFO,
             buildTerminalInfo(cols, rows, width, height));
  return 0;
}

void et_session_close(et_session *s) {
  if (!s) return;
  if (!claimForClose(s)) return;  // second+ call on this handle: no-op
  s->stopping.store(true);
  s->wake();
  if (s->transport) s->transport->shutdown();
  if (s->thr.joinable()) s->thr.join();
  if (s->wake_r >= 0) close(s->wake_r);
  if (s->wake_w >= 0) close(s->wake_w);
  delete s;
}
