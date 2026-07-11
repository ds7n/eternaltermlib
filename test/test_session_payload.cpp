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
