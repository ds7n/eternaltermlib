// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
//
// Integration-test harness: captures et_callbacks into thread-safe buffers
// so tests can deterministically wait for bytes/state/end instead of
// sleeping or polling with a busy loop. Callbacks fire on eternaltermlib's
// own transport thread, so every access to shared state is mutex-guarded;
// the wait*() methods use a condition variable and always take a timeout,
// returning false (never hanging) if the awaited condition doesn't happen
// in time -- callers should ASSERT_TRUE/EXPECT_TRUE on the result so a
// fixture/network hang fails the test instead of wedging the run.
#ifndef ETERNALTERMLIB_TEST_IT_HELPERS_HPP
#define ETERNALTERMLIB_TEST_IT_HELPERS_HPP

#include "eternaltermlib.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

struct Harness {
  std::mutex m;
  std::condition_variable cv;
  std::string receivedBuf;
  std::vector<et_state> states;
  bool ended = false;
  std::string endReason;

  static void onBytes(void *ctx, const uint8_t *buf, size_t len) {
    auto *h = static_cast<Harness *>(ctx);
    std::lock_guard<std::mutex> g(h->m);
    h->receivedBuf.append(reinterpret_cast<const char *>(buf), len);
    h->cv.notify_all();
  }

  static void onState(void *ctx, et_state state) {
    auto *h = static_cast<Harness *>(ctx);
    std::lock_guard<std::mutex> g(h->m);
    h->states.push_back(state);
    h->cv.notify_all();
  }

  static void onEnd(void *ctx, const char *reason) {
    auto *h = static_cast<Harness *>(ctx);
    std::lock_guard<std::mutex> g(h->m);
    h->ended = true;
    h->endReason = reason ? reason : "";
    h->cv.notify_all();
  }

  et_callbacks cbs() const { return et_callbacks{onBytes, onState, onEnd}; }

  // Blocks until at least `n` bytes have been received via on_bytes, or
  // `timeout` elapses. Returns false on timeout (never hangs the test).
  template <typename Rep, typename Period>
  bool waitBytes(size_t n, std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, timeout,
                        [&] { return receivedBuf.size() >= n; });
  }

  // Blocks until the received buffer contains `needle` as a substring, or
  // `timeout` elapses. Returns false on timeout (never hangs the test). Used
  // by the window-size test, where the remote PTY echoes its geometry as a
  // known substring somewhere in a larger stream (shell prompt noise etc.).
  template <typename Rep, typename Period>
  bool waitBytesContaining(const std::string &needle,
                           std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, timeout, [&] {
      return receivedBuf.find(needle) != std::string::npos;
    });
  }

  // Blocks until `state` has been reported at least once via on_state, or
  // `timeout` elapses. Returns false on timeout.
  template <typename Rep, typename Period>
  bool waitState(et_state state, std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, timeout, [&] {
      return std::find(states.begin(), states.end(), state) != states.end();
    });
  }

  // Blocks until on_end has fired, or `timeout` elapses. Returns false on
  // timeout.
  template <typename Rep, typename Period>
  bool waitEnd(std::chrono::duration<Rep, Period> timeout) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, timeout, [&] { return ended; });
  }

  // Snapshot accessor (mutex-guarded read of the captured bytes). Tests call
  // this AFTER waitBytes()/waitEnd() has already synchronized with the
  // transport thread, so a plain lock-and-copy is sufficient here.
  std::string received() {
    std::lock_guard<std::mutex> g(m);
    return receivedBuf;
  }

  bool reachedState(et_state state) {
    std::lock_guard<std::mutex> g(m);
    return std::find(states.begin(), states.end(), state) != states.end();
  }

  bool endReasonContainsAny(std::initializer_list<const char *> needles) {
    std::lock_guard<std::mutex> g(m);
    for (const char *needle : needles)
      if (endReason.find(needle) != std::string::npos) return true;
    return false;
  }

  // Exact snapshot of the on_end reason (mutex-guarded read). Callers should
  // waitEnd() first to synchronize with the transport thread, same as
  // received() above. Use this for exact-equality assertions on the reason
  // string, rather than the substring-based endReasonContainsAny() above.
  std::string endReasonExact() {
    std::lock_guard<std::mutex> g(m);
    return endReason;
  }
};

#endif  // ETERNALTERMLIB_TEST_IT_HELPERS_HPP
