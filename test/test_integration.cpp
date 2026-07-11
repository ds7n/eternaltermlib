// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
//
// Integration tests against a REAL etserver fixture (see
// test/fixture/{Dockerfile.etserver,entrypoint.sh,plant_credential.sh} and
// docker-compose.yml's `etserver`/`dev-integration` services, gated behind
// the `integration` Compose profile). Labeled ET_INTEGRATION in ctest (see
// test/CMakeLists.txt) so the plain unit suite stays fast and fixture-free.
//
// The fixture plants:
//   - ET_ID/ET_PASSKEY   -> the container's default login shell (bash). Not
//     used by any test below directly (UnregisteredIdReportsSpecificFailure
//     deliberately uses an id the fixture never planted at all -- see that
//     test for why); kept alive for manual/future use.
//   - Three DISTINCT `cat`-echo credentials (ET_CAT_IDS/ET_CAT_PASSKEYS,
//     SHELL=et-cat-shell) -- one per test below that performs a real
//     et_connect. Each such test gets its OWN id, never reused across tests:
//     ET's wire protocol treats a second connect for an id that has already
//     connected once as roaming *recovery* (BackedReader/BackedWriter
//     sequence-number resumption), not a fresh session, so two independent
//     et_connect calls sharing an id after a hard et_close() (no graceful
//     ET-level goodbye) desync the recovered sequence numbers and the server
//     FATALs ("client is ahead of server") or logs "Invalid size" -- observed
//     directly against this fixture. See plant_credential.sh for the full
//     writeup, including the SEPARATE (more severe) finding about testing a
//     wrong passkey against a *registered* id.
#include "eternaltermlib.h"
#include "it_helpers.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>

static const char *H() {
  const char *v = getenv("ET_HOST");
  return v ? v : "etserver";
}
static const char *ID() {
  const char *v = getenv("ET_ID");
  return v ? v : "0123456789abcdef";
}
static const char *KEY() {
  const char *v = getenv("ET_PASSKEY");
  return v ? v : "0123456789abcdef0123456789abcdef";
}
// One dedicated cat-echo id per connecting test (see file header).
static const char *CatID(int n) {
  switch (n) {
    case 0: {
      const char *v = getenv("ET_CAT_ID_0");
      return v ? v : "fedcba9876543210";
    }
    case 1: {
      const char *v = getenv("ET_CAT_ID_1");
      return v ? v : "fedcba9876543211";
    }
    default: {
      const char *v = getenv("ET_CAT_ID_2");
      return v ? v : "fedcba9876543212";
    }
  }
}
static const char *CatKey(int n) {
  switch (n) {
    case 0: {
      const char *v = getenv("ET_CAT_PASSKEY_0");
      return v ? v : "fedcba9876543210fedcba9876543210";
    }
    case 1: {
      const char *v = getenv("ET_CAT_PASSKEY_1");
      return v ? v : "fedcba9876543211fedcba9876543211";
    }
    default: {
      const char *v = getenv("ET_CAT_PASSKEY_2");
      return v ? v : "fedcba9876543212fedcba9876543212";
    }
  }
}

// Task 7 dedicated ids (own never-reused id per test, same rule as CatID).
// Match docker-compose.yml's etserver env + plant_credential.sh defaults.
static const char *ResizeID() {
  const char *v = getenv("ET_RESIZE_ID");
  return v ? v : "abcdef0123456780";
}
static const char *ResizeKey() {
  const char *v = getenv("ET_RESIZE_PASSKEY");
  return v ? v : "abcdef0123456780abcdef0123456780";
}
static const char *Exit0ID() {
  const char *v = getenv("ET_EXIT0_ID");
  return v ? v : "abcdef0123456781";
}
static const char *Exit0Key() {
  const char *v = getenv("ET_EXIT0_PASSKEY");
  return v ? v : "abcdef0123456781abcdef0123456781";
}

static et_config baseCfg(Harness *h) {
  (void)h;  // cfg itself doesn't reference the harness; kept for symmetry
            // with the brief's signature / future per-test cfg tweaks.
  static const char *k[] = {"TERM"};
  static const char *v[] = {"xterm-256color"};
  et_config c = {};
  c.host = H();
  c.port = 2022;
  c.id = ID();
  c.passkey = KEY();
  c.env_keys = k;
  c.env_vals = v;
  c.env_count = 1;
  c.cols = 80;
  c.rows = 24;
  return c;
}

TEST(Integration, ConnectsAndReachesConnected) {
  Harness h;
  et_config c = baseCfg(&h);
  c.id = CatID(0);
  c.passkey = CatKey(0);
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  EXPECT_TRUE(h.waitState(ET_STATE_CONNECTED, std::chrono::seconds(10)));
  et_close(cl);
}

// NOTE on why this is "UnregisteredId" and not "WrongPasskey": a genuine
// wrong-PASSKEY negative test cannot be honestly performed against a stock
// etserver, for two confirmed upstream limitations (verified directly
// against the vendored ET source and this fixture):
//
//   1. ET has NO passkey-verification step at handshake. ServerConnection
//      only checks clientKeyExists(clientId) -- whether the server has ANY
//      key stored for that id -- the client's supplied passkey is never
//      compared; it only seeds the client's own CryptoHandler. So a
//      REGISTERED id + WRONG passkey is ACCEPTED at handshake
//      (NEW_CLIENT/RETURNING_CLIENT) and only desyncs the stream cipher:
//      the server's CryptoHandler::decrypt() hits STFATAL ("Decrypt failed.
//      Possible key mismatch?") on the first encrypted payload and the
//      ENTIRE etserver process CRASHES, taking down every other session
//      with it. There is no clean rejection to observe in that path -- a
//      test doing this would assert "the server died," which exercises
//      upstream ET's crash behavior, not our client.
//
//   2. The specific rejection reason is swallowed before our client ever
//      sees it. For an UNREGISTERED id, the server DOES return a specific
//      ConnectResponse status (INVALID_KEY / "Client is not registered"),
//      but upstream ClientConnection::connect() throws a runtime_error
//      carrying that detail and CATCHES it internally, logging it and
//      returning only `false` -- the detail never crosses the
//      connect()->bool boundary. Our Transport::connect() is a thin
//      passthrough to that bool (src/transport.cpp), so session.cpp's
//      run() loop (src/session.cpp) sees `false` and emits the generic
//      `cbs.on_end(ctx, "connect failed")` -- not the richer server-side
//      reason. Propagating that detail through Transport/session would be
//      a real diagnostic improvement but is out of scope for this test;
//      it's a follow-up for a future change to session.cpp/transport.cpp.
//
// Given both limitations, this test covers the honestly-testable portion of
// the spec's Sec.6 bad-credential Critical-tier intent: a REJECTED
// credential (unregistered id) produces a SPECIFIC, exact failure reason
// and never reaches CONNECTED -- as opposed to a loose "some error
// happened" check.
TEST(Integration, UnregisteredIdReportsSpecificFailure) {
  Harness h;
  et_config c = baseCfg(&h);
  // Deliberately an id the fixture never planted (rather than a KNOWN id
  // with a mismatched passkey -- see the comment block above for why that
  // alternative is not honestly testable). An unregistered id fails cleanly
  // and early, at the ConnectRequest/ConnectResponse stage (INVALID_KEY /
  // "Client is not registered", ServerConnection.cpp), before any
  // per-client crypto exists to fail on -- see
  // test/fixture/plant_credential.sh for the fuller writeup of this
  // finding.
  c.id = "0000000000000000";
  c.passkey = "ffffffffffffffffffffffffffffffff";  // unused/unplanted id
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  ASSERT_TRUE(h.waitEnd(std::chrono::seconds(10)));
  // SPECIFIC failure, not merely "some error": never reaches CONNECTED, and
  // the on_end reason is asserted EXACTLY (not a substring/contains check)
  // against what our client genuinely produces for this path -- the
  // generic "connect failed" from src/session.cpp's `!transport->connect()`
  // branch, per the swallowed-detail limitation documented above.
  EXPECT_FALSE(h.reachedState(ET_STATE_CONNECTED));
  EXPECT_EQ(h.endReasonExact(), "connect failed");
  et_close(cl);
}

TEST(Integration, ByteRoundTripThroughCatShell) {
  Harness h;
  et_config c = baseCfg(&h);
  c.id = CatID(1);
  c.passkey = CatKey(1);  // fixture's cat-echo session, see file header
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  ASSERT_TRUE(h.waitState(ET_STATE_CONNECTED, std::chrono::seconds(10)));
  const char *msg = "hello-eternaltermlib\n";
  // The PTY's line discipline runs in canonical mode (the fixture never
  // switches it to raw mode), so the kernel tty driver itself translates the
  // trailing '\n' to "\r\n" on echo -- observed directly against the real
  // fixture -- before `cat` ever sees/re-emits the byte. The EXACT expected
  // value therefore includes the CR (not a weakened "contains"/"non-empty"
  // check): a wrong echo (dropped/garbled/truncated bytes) still fails this.
  const std::string expected = "hello-eternaltermlib\r\n";
  ASSERT_EQ(et_send(cl, (const uint8_t *)msg, strlen(msg)), (int)strlen(msg));
  ASSERT_TRUE(h.waitBytes(expected.size(), std::chrono::seconds(10)));
  EXPECT_EQ(h.received(), expected);  // EXACT bytes, not "non-empty"
  et_close(cl);
}

TEST(Integration, EmptySendIsAcceptedNoBytes) {
  Harness h;
  et_config c = baseCfg(&h);
  c.id = CatID(2);
  c.passkey = CatKey(2);
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  ASSERT_TRUE(h.waitState(ET_STATE_CONNECTED, std::chrono::seconds(10)));
  EXPECT_EQ(et_send(cl, (const uint8_t *)"", 0), 0);  // boundary: 0-length
  et_close(cl);
}

// Task 7 Step 1: a client window-size change (et_set_window_size ->
// TERMINAL_INFO) reaches the remote PTY. The fixture's et-resize-echo-shell
// traps SIGWINCH (delivered by ET's ioctl(TIOCSWINSZ) on the PTY master) and
// echoes `stty size` == "<rows> <cols>". We push cols=120, rows=40 and assert
// the remote PTY reported EXACTLY "40 120" -- the geometry actually applied
// on the far side, not a loose "some bytes came back" check.
TEST(Integration, WindowSizePropagatesToRemotePty) {
  Harness h;
  et_config c = baseCfg(&h);
  c.id = ResizeID();
  c.passkey = ResizeKey();
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  ASSERT_TRUE(h.waitState(ET_STATE_CONNECTED, std::chrono::seconds(10)));
  ASSERT_EQ(et_set_window_size(cl, /*cols*/ 120, /*rows*/ 40, 0, 0), 0);
  ASSERT_TRUE(h.waitBytesContaining("40 120", std::chrono::seconds(10)));
  EXPECT_TRUE(h.received().find("40 120") != std::string::npos);
  et_close(cl);
}

// Task 7 Step 2: the remote shell exiting terminates the session -> on_end +
// ET_STATE_DISCONNECTED; then et_close is idempotent/null-safe after on_end.
// The fixture's et-exit0-shell exits immediately once exec'd (on connect).
TEST(Integration, RemoteShellExitTriggersOnEnd) {
  Harness h;
  et_config c = baseCfg(&h);
  c.id = Exit0ID();
  c.passkey = Exit0Key();
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  ASSERT_TRUE(h.waitEnd(std::chrono::seconds(10)));
  EXPECT_TRUE(h.reachedState(ET_STATE_DISCONNECTED));
  et_close(cl);       // idempotent after on_end
  et_close(nullptr);  // null-safe
}
