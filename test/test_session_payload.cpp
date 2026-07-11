// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#include "session_internal.hpp"
#include "eternaltermlib.h"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>

TEST(SessionPayload, EnvMapRoundTrips) {
  const char *k[] = {"TERM", "LANG"};
  const char *v[] = {"xterm-256color", "en_US.UTF-8"};
  std::string wire = et::buildInitialPayload(k, v, 2);
  et::InitialPayload p = et::stringToProto<et::InitialPayload>(wire);
  EXPECT_EQ(p.environmentvariables().at("TERM"), "xterm-256color");
  EXPECT_EQ(p.environmentvariables().at("LANG"), "en_US.UTF-8");
  EXPECT_FALSE(p.jumphost());
}

TEST(SessionPayload, TerminalInfoMapsColsRowsToColumnRow) {
  std::string wire = et::buildTerminalInfo(/*cols*/ 80, /*rows*/ 24,
                                           /*w*/ 640, /*h*/ 480);
  et::TerminalInfo ti = et::stringToProto<et::TerminalInfo>(wire);
  EXPECT_EQ(ti.column(), 80);
  EXPECT_EQ(ti.row(), 24);
  EXPECT_EQ(ti.width(), 640);
  EXPECT_EQ(ti.height(), 480);
}

TEST(SessionPayload, TerminalBufferPreservesBytes) {
  const uint8_t bytes[] = {0x00, 0x1b, 0x5b, 0x41, 0xff};
  std::string wire = et::buildTerminalBuffer(bytes, sizeof bytes);
  et::TerminalBuffer tb = et::stringToProto<et::TerminalBuffer>(wire);
  EXPECT_EQ(tb.buffer(), std::string(reinterpret_cast<const char *>(bytes),
                                     sizeof bytes));
}

// Security regression: the session's inbound decode must NOT route untrusted
// server payloads through ET's stringToProto<>(), which
// calls LOG(FATAL) -> abort() on a parse failure -- a malformed frame from a
// hostile/compromised server would otherwise crash the whole embedding app.
// session.cpp was changed to use ParseFromString() directly and handle the
// false return (drop the TERMINAL_BUFFER frame; fail-closed on a malformed
// INITIAL_RESPONSE). This test pins the contract that decode relies on: a
// malformed wire payload makes ParseFromString() return false (a value we can
// branch on) rather than aborting, and leaves the message empty.
//
// The bytes below are deliberately invalid protobuf: field 1 tagged as
// wire-type 6 (0x0e = (1<<3)|6), which is not a defined wire type, so the
// lite parser rejects the message.
TEST(SessionPayload, MalformedProtoParsesFalseNotAbort) {
  const std::string malformed("\x0e\xff\xff\xff\xff", 5);

  et::TerminalBuffer tb;
  EXPECT_FALSE(tb.ParseFromString(malformed));  // rejected, not aborted
  EXPECT_EQ(tb.buffer(), "");                   // safe empty on failure

  et::InitialResponse resp;
  EXPECT_FALSE(resp.ParseFromString(malformed));  // handshake would fail-closed

  // Sanity: a WELL-formed TerminalBuffer still round-trips (guards against a
  // fix that broke the happy path by over-rejecting).
  const uint8_t good[] = {'h', 'i'};
  et::TerminalBuffer ok;
  ASSERT_TRUE(ok.ParseFromString(et::buildTerminalBuffer(good, sizeof good)));
  EXPECT_EQ(ok.buffer(), "hi");
}

namespace {
void noopBytes(void *, const uint8_t *, size_t) {}
void noopState(void *, et_state) {}
void noopEnd(void *, const char *) {}
}  // namespace

// Regression for the et_close double-free: et_close must be idempotent so a
// consumer that calls it once from an on_end handler and again from its own
// cleanup path (a sequence the public header explicitly blesses) does not
// double-free the handle. This test is most meaningful run under ASan/valgrind
// (Task 8 sanitizer runs) — without a sanitizer a double-free on freshly freed
// memory may not crash deterministically, but the test still documents and
// exercises the contract, and will catch a regression under sanitized builds.
TEST(SessionPayload, CloseIsIdempotent) {
  et_callbacks cbs = {noopBytes, noopState, noopEnd};
  et_config cfg = {};
  cfg.host = "127.0.0.1";
  cfg.port = 1;  // unlikely-to-be-listening port -> connect fails fast
  cfg.id = "0123456789abcdef";
  cfg.passkey = "01234567890123456789012345678901";
  cfg.cols = 80;
  cfg.rows = 24;

  et_client *c = et_connect(&cfg, &cbs, nullptr);
  ASSERT_NE(c, nullptr);

  et_close(c);  // first call: tears down and frees
  et_close(c);  // second call on the same (now-dangling) pointer: must be a no-op
}

namespace {
struct EndLatch {
  std::mutex m;
  std::condition_variable cv;
  bool ended = false;
  static void onEnd(void *ctx, const char *) {
    auto *l = static_cast<EndLatch *>(ctx);
    std::lock_guard<std::mutex> g(l->m);
    l->ended = true;
    l->cv.notify_all();
  }
  bool wait() {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, std::chrono::seconds(10), [&] { return ended; });
  }
};
}  // namespace

// Security regression: after a session self-terminates (natural on_end,
// NOT et_close), a subsequent et_send must report ET_ERR_CLOSED rather than a
// false success -- the transport thread is gone, so the bytes could never be
// sent. Connect to an unlikely-listening port so the session ends on its own;
// then et_send AFTER on_end must be ET_ERR_CLOSED.
TEST(SessionPayload, SendAfterNaturalEndReportsClosed) {
  EndLatch latch;
  et_callbacks cbs = {noopBytes, noopState, EndLatch::onEnd};
  et_config cfg = {};
  cfg.host = "127.0.0.1";
  cfg.port = 1;  // unlikely-to-be-listening -> connect fails -> natural on_end
  cfg.id = "0123456789abcdef";
  cfg.passkey = "01234567890123456789012345678901";
  cfg.cols = 80;
  cfg.rows = 24;

  et_client *c = et_connect(&cfg, &cbs, &latch);
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(latch.wait());  // session has self-terminated (on_end fired)

  // The handle is still allocated (not yet et_close'd) but ended: send/resize
  // must fail closed, not report a false success.
  EXPECT_EQ(et_send(c, (const uint8_t *)"x", 1), ET_ERR_CLOSED);
  EXPECT_EQ(et_set_window_size(c, 100, 40, 0, 0), ET_ERR_CLOSED);

  et_close(c);
}
