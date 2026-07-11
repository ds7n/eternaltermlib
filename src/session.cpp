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
#include <sodium.h>  // sodium_memzero — scrub the passkey before free

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
  // Set by run() when the session terminates on its own (remote exit, fatal
  // error) -- distinct from `stopping`, which only et_session_close sets. A
  // post-on_end et_send/et_resize on a not-yet-closed handle must report
  // ET_ERR_CLOSED, not a false success for bytes that can never transmit.
  std::atomic<bool> ended{false};
  et_state lastState = ET_STATE_CONNECTING;

  // Destructor owns final cleanup so both the normal et_session_close path and
  // the exception-unwind path in et_session_start release resources:
  //  - scrub the secretbox key (std::string's dtor does not zero its buffer;
  //    sodium_memzero is a barrier-guarded wipe that won't be optimized away),
  //  - close the self-pipe fds (guarded, idempotent against -1).
  ~et_session() {
    if (!passkey.empty()) sodium_memzero(&passkey[0], passkey.size());
    if (wake_r >= 0) close(wake_r);
    if (wake_w >= 0) close(wake_w);
  }

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
  // Terminal exit: mark the session ended (so a post-on_end et_send returns
  // ET_ERR_CLOSED rather than a false success), fire on_end with `reason`,
  // then transition to DISCONNECTED. Every path that ends run() goes through
  // here so `ended` is always set before on_end.
  void terminate(const char *reason) {
    ended.store(true);
    cbs.on_end(ctx, reason);
    emitState(ET_STATE_DISCONNECTED);
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
    terminate("connect failed");
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
          terminate("missing initial response");
          return;
        }
        // Parse the server's INITIAL_RESPONSE directly rather than via ET's
        // stringToProto<>(): that helper calls LOG(FATAL) -> abort() on a
        // parse failure, so an untrusted server sending a malformed payload
        // would crash the whole embedding process. Handle failure ourselves
        // and fail CLOSED -- a response we cannot parse is not a valid
        // handshake, so we reject rather than proceed to CONNECTED.
        InitialResponse resp;
        if (!resp.ParseFromString(pl)) {
          terminate("malformed initial response");
          return;
        }
        if (resp.has_error()) {
          terminate(("handshake rejected: " + resp.error()).c_str());
          return;
        }
        ok = true;
      }
    }
  }
  if (!ok) {
    terminate("handshake timeout");
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
        if (h == TerminalPacketType::TERMINAL_BUFFER) {
          // Parse directly instead of via ET's stringToProto<>(), which
          // aborts the process (LOG(FATAL)) on a parse failure. An untrusted
          // server could otherwise crash the app with one malformed frame.
          // A frame we cannot parse is dropped (the session continues), the
          // same disposition as an unknown packet type below.
          TerminalBuffer tb;
          if (tb.ParseFromString(pl)) coalesced += tb.buffer();
        } else if (h == TerminalPacketType::KEEP_ALIVE)
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
  terminate(stopping.load() ? "closed" : "session ended");
}

et_session *et_session_start(const et_config *cfg, const et_callbacks *cbs,
                             void *ctx) try {
  // Own `s` via unique_ptr through the fallible setup: if buildInitialPayload,
  // the Transport ctor, or the thread spawn throws, the unique_ptr frees the
  // session (its destructor closes any opened pipe fds -- see below) instead
  // of leaking, and the catch clause below stops the exception from crossing
  // the extern "C" boundary (UB for a C ABI), returning NULL per the contract.
  auto s = std::make_unique<et_session>();
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
  // Register + spawn last. If the thread spawn throws (EAGAIN), the session is
  // not yet registered, so unique_ptr cleanup here is race-free (no other
  // thread can reach it). registerLive precedes the spawn so the started
  // thread's handle is already claimable by et_close.
  et_session *raw = s.get();
  registerLive(raw);
  try {
    raw->thr = std::thread([raw] { raw->run(); });
  } catch (...) {
    claimForClose(raw);  // undo registerLive so the set stays consistent
    throw;               // unique_ptr frees s (dtor closes fds); caught below
  }
  return s.release();  // ownership transfers to the caller's handle
} catch (...) {
  // No C++ exception may cross the extern "C" boundary. Bad config / OOM /
  // resource exhaustion during setup -> fail safe with NULL, same as the
  // documented bad-config contract.
  return nullptr;
}

int et_session_send(et_session *s, const uint8_t *buf, size_t len) {
  // Closed by the caller (stopping) OR self-terminated (ended): either way the
  // transport thread is gone/going, so enqueuing would silently drop the bytes.
  if (s->stopping.load() || s->ended.load()) return ET_ERR_CLOSED;
  s->enqueue(TerminalPacketType::TERMINAL_BUFFER, buildTerminalBuffer(buf, len));
  return (int)len;
}

int et_session_resize(et_session *s, uint16_t cols, uint16_t rows,
                      uint16_t width, uint16_t height) {
  if (s->stopping.load() || s->ended.load()) return ET_ERR_CLOSED;
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
  // ~et_session (runs on delete) closes the self-pipe fds and scrubs the
  // passkey; keep that cleanup in one place so the exception-unwind path in
  // et_session_start frees the same resources.
  delete s;
}
