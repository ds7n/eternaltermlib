// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
//
// The crown-jewel test: prove ET's roaming/replay survives OUR C ABI. We
// stream a known, ordered, numbered byte sequence through a raw-mode cat shell
// on a REAL etserver, SEVER the TCP path mid-stream (toxiproxy disable), send
// the rest while the link is down, RESTORE the path (toxiproxy enable), and
// assert every byte comes back EXACTLY once, in order -- nothing lost, nothing
// duplicated. This is the property mosh structurally cannot provide and ET can;
// if this passes, eternaltermlib's reason to exist is proven.
//
// Wiring: the client connects to toxiproxy:2022 (not etserver:2022 directly);
// toxiproxy forwards to etserver:2022. proxy::pause()/resume() drive
// toxiproxy's control API to drop/restore the relay. See test/proxy_control.hpp
// and docker-compose.yml's `toxiproxy` service (integration profile).
//
// The roaming id reuses ONE credential across the sever/restore -- that is
// exactly ET's roaming *recovery* path (the SAME logical client reconnecting
// after a dropped link), i.e. the correct/intended use of id-reuse, not the
// two-unrelated-connects footgun documented in plant_credential.sh.
#include "eternaltermlib.h"
#include "it_helpers.hpp"
#include "proxy_control.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>

namespace {

const char *RoamHost() {
  // The client talks to the proxy, NOT etserver directly.
  const char *v = getenv("ET_PROXY_HOST");
  return v ? v : "toxiproxy";
}
const char *RoamID() {
  const char *v = getenv("ET_ROAM_ID");
  return v ? v : "abcdef0123456782";
}
const char *RoamKey() {
  const char *v = getenv("ET_ROAM_PASSKEY");
  return v ? v : "abcdef0123456782abcdef0123456782";
}

// Config pointing the client at the breakable proxy (host = toxiproxy:2022).
et_config baseCfgThroughProxy() {
  static const char *k[] = {"TERM"};
  static const char *v[] = {"xterm-256color"};
  et_config c = {};
  c.host = RoamHost();
  c.port = 2022;
  c.id = RoamID();
  c.passkey = RoamKey();
  c.env_keys = k;
  c.env_vals = v;
  c.env_count = 1;
  c.cols = 80;
  c.rows = 24;
  return c;
}

}  // namespace

// THE crown-jewel assertion: a known ordered sequence streamed through a cut
// and restored link arrives byte-for-byte, once each, in order.
TEST(Roaming, ReplaysExactlyTheMissedBytesNoLossNoDup) {
  // Establish the proxy relay (toxiproxy:2022 -> etserver:2022), enabled.
  // Throws (failing the test loudly) if the control plane is unreachable, so a
  // green result can never come from a link that was never actually proxied.
  proxy::setup();

  Harness h;
  et_config c = baseCfgThroughProxy();
  et_callbacks cbs = h.cbs();
  et_client *cl = et_connect(&c, &cbs, &h);
  ASSERT_NE(cl, nullptr);
  ASSERT_TRUE(h.waitState(ET_STATE_CONNECTED, std::chrono::seconds(10)));

  // The raw-mode shell applies `stty raw -echo` asynchronously at startup, then
  // emits this READY marker (see et-cat-raw-shell). Wait for it before sending
  // anything so every payload byte is echoed under raw mode -- otherwise a
  // startup race echoes the first bytes in canonical mode ('\r\n'). The marker
  // is a known, distinct prefix we strip before the exact-bytes comparison.
  const std::string kReady = "__RAW_READY__\n";
  ASSERT_TRUE(h.waitBytesContaining(kReady, std::chrono::seconds(10)));

  // A numbered sequence the raw-mode cat shell echoes back VERBATIM (raw +
  // -echo => cat is the sole, byte-exact echo path; no line-discipline
  // duplication or '\n'->'\r\n' mutation). 200 lines "L0\n".."L199\n".
  std::string expected;
  for (int i = 0; i < 200; i++) expected += "L" + std::to_string(i) + "\n";
  ASSERT_GT(expected.size(), 500u);  // guard: the split below assumes >500B
  const std::string chunk1 = expected.substr(0, 500);
  const std::string chunk2 = expected.substr(500);

  // Send chunk1 over the LIVE link and WAIT until it has fully echoed back.
  // This is load-bearing for the "no duplication" property: ET's recover()
  // replays only writer bytes the server has not ACKED (Connection::recover ->
  // writer->recover(remoteSeq)). By confirming chunk1 round-tripped BEFORE the
  // cut, we guarantee its bytes are acked and therefore NOT replayed -- so the
  // post-reconnect replay carries ONLY chunk2 (the bytes sent while the link
  // was down), and the far-side cat echoes each byte exactly once. (Severing
  // with un-acked in-flight bytes would legitimately replay them and the echo
  // app would re-emit them -- an at-least-once artifact of a stateful echo at
  // the cut boundary, not an ET transport defect; we avoid that confound so
  // the assertion isolates ET's exactly-once replay of the missed bytes.)
  ASSERT_EQ(et_send(cl, (const uint8_t *)chunk1.data(), chunk1.size()),
            (int)chunk1.size());
  ASSERT_TRUE(
      h.waitBytes(kReady.size() + chunk1.size(), std::chrono::seconds(15)));

  // SEVER the TCP path mid-stream. ET's write/read on the dead socket (and, as
  // a backstop, our keepalive-miss) trip closeSocketAndMaybeReconnect -> fd<0,
  // and our client emits ET_STATE_ROAMING. Generous timeout covers the
  // keepalive backstop (~2 keepalive intervals).
  proxy::pause();
  EXPECT_TRUE(h.waitState(ET_STATE_ROAMING, std::chrono::seconds(15)));

  // Send chunk2 WHILE THE LINK IS DOWN. BackedWriter buffers it (fd<0 =>
  // BUFFERED_ONLY); it is exactly the "missed" data ET must replay on recover.
  ASSERT_EQ(et_send(cl, (const uint8_t *)chunk2.data(), chunk2.size()),
            (int)chunk2.size());

  // RESTORE the path. ET's reconnect thread re-establishes and recover()
  // replays the backed (chunk2) bytes from the last acked sequence number.
  proxy::resume();
  EXPECT_TRUE(h.waitState(ET_STATE_CONNECTED, std::chrono::seconds(20)));

  // THE crown-jewel assertion: the full echoed stream after the READY marker
  // equals the sent sequence byte-for-byte -- every byte once, in order,
  // none lost (chunk2 replayed) and none duplicated (chunk1 already acked).
  ASSERT_TRUE(h.waitBytes(kReady.size() + expected.size(),
                          std::chrono::seconds(20)));
  std::string got = h.received();
  auto pos = got.find(kReady);
  ASSERT_NE(pos, std::string::npos);
  std::string payload = got.substr(pos + kReady.size());
  EXPECT_EQ(payload, expected);  // exact -- no loss, no dup, in order

  et_close(cl);
}
