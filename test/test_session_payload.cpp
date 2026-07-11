// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#include "session_internal.hpp"
#include "eternaltermlib.h"
#include <gtest/gtest.h>

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

// Security regression (report finding #1): the session's inbound decode must
// NOT route untrusted server payloads through ET's stringToProto<>(), which
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
