#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
ET_ID="${ET_ID:-0123456789abcdef}"
ET_PASSKEY="${ET_PASSKEY:-0123456789abcdef0123456789abcdef}"
# etterminal reads '<id>/<passkey>_<TERM>' on stdin, registers the id/passkey
# with the running etserver over the router fifo (default
# /var/run/etserver.idpasskey.fifo, since both binaries run as root here and
# use no --serverfifo override -- see ServerFifoPath::getPathForCreation /
# detectAndConnect), then daemonizes (DaemonCreator::createSessionLeader ->
# daemon(0,0)): this invocation's own process exits immediately once the
# fork succeeds, but the detached child keeps running UserTerminalHandler::run
# forever, holding the PTY ($SHELL, read fresh from this process's env at
# fork time -- see PseudoUserTerminal::runTerminal) open.
#
# Credential lifecycle finding (see task-5-report.md for the router-map
# evidence -- still correct as far as it goes): UserTerminalRouter::idInfoMap
# is populated once and never erased by the router, so a NEW client process
# can always resolve an id it has never connected with before.
#
# Task 6 correction (found empirically, see task-6-report.md): that is NOT
# the whole lifecycle story for a SINGLE id across *repeat* independent
# client connections. ET's wire protocol treats a second connect for an
# id that has already connected once as a RETURNING_CLIENT / roaming
# *recovery* (BackedReader/BackedWriter sequence-number resumption,
# ServerConnection.cpp's `recoverClient` path) rather than a fresh session --
# it is designed for the SAME logical client reconnecting after a dropped
# link, not for two unrelated `et_connect` calls (e.g. two separate test
# cases) sharing an id after a hard `et_close()` (which sends no graceful
# ET-level goodbye). Reusing an id this way desyncs the recovered
# reader/writer sequence numbers and the server FATALs
# ("Something went really wrong, client is ahead of server",
# BackedWriter.cpp) or logs "Invalid size" -- observed directly against this
# fixture while developing the byte-round-trip test.
#
# So: every integration test that performs a REAL et_connect gets its OWN
# never-reused id, each running the et-cat-shell echo wrapper (see
# et-cat-shell / the -l argv issue documented there). Adding a new such test
# means adding one more id to CAT_IDS below, not reusing an existing one.
#
# Second, SEPARATE and more severe finding: testing "wrong passkey" by
# reusing a *registered* id (e.g. the primary ET_ID above) with a mismatched
# passkey does NOT get a graceful rejection from real upstream ET --
# ServerConnection::clientHandler's id-registration check
# (clientKeyExists()) only looks at the id, so it passes, and the server
# constructs a ServerClientConnection keyed on the id using the SERVER's own
# cached (correct) passkey (ServerConnection.cpp:84,
# `clientKeys.at(clientId)`). The first real payload the client sends is
# then encrypted with the CLIENT's (wrong) passkey, so the server's decrypt
# attempt (using its own correct key) fails --
# CryptoHandler::decrypt() (src/base/CryptoHandler.cpp) unconditionally
# STFATALs ("Decrypt failed.  Possible key mismatch?") on ANY decrypt
# failure, which aborts the ENTIRE etserver PROCESS -- taking down every
# other id's in-flight session with it. Verified directly against this
# fixture: reusing ET_ID with a bad passkey crashes the container
# (`docker ps` shows Exited(134) immediately after). A wrong-passkey test
# MUST instead use an id the fixture never planted at all -- that fails
# early and cleanly at the ConnectRequest/ConnectResponse stage
# (`INVALID_KEY` / "Client is not registered", ServerConnection.cpp:88-98),
# before any per-client CryptoHandler exists to crash on. See
# test/test_integration.cpp's UnregisteredIdReportsSpecificFailure.
echo "${ET_ID}/${ET_PASSKEY}_xterm" | /src/et/build/etterminal
# Sentinel for the compose healthcheck: only created once the fifo handshake
# above has actually completed (etterminal has registered the id/passkey with
# the router and daemonized). `set -euo pipefail` aborts this script before
# reaching this line if the pipeline above fails, so the sentinel is only ever
# created on genuine plant success -- closing the healthcheck/plant race where
# `nc -z 127.0.0.1 2022` alone could report (healthy) before the credential
# was registered.
touch /tmp/.et-credential-planted

# plant_group SHELL "<space-separated ids>" "<space-separated passkeys>"
# Registers each id/passkey pair with the router, running the given $SHELL for
# that session's PTY. etterminal's run() blocks on the router TERMINAL_INIT
# until a client actually connects, so the $SHELL is only exec'd on connect --
# registration itself is durable and cheap (see the lifecycle note above). This
# is the SAME per-id-registration mechanism the cat-echo plants use, factored
# out so Task 7's window/lifecycle/roaming shells can reuse it verbatim.
plant_group() {
  local shell="$1"
  local -a ids passkeys
  read -r -a ids <<<"$2"
  read -r -a passkeys <<<"$3"
  local i
  for i in "${!ids[@]}"; do
    SHELL="$shell" \
      bash -c "echo '${ids[i]}/${passkeys[i]}_xterm' | /src/et/build/etterminal"
  done
}

# One dedicated `cat`-echo credential PER integration test that actually
# connects (see the lifecycle note above for why sharing one id across
# multiple independent et_connect calls is unsafe). Env-overridable
# (ET_CAT_IDS/ET_CAT_PASSKEYS, space-separated, parallel arrays) purely for
# flexibility; the defaults are what docker-compose.yml's `etserver` service
# sets and what test/test_integration.cpp falls back to.
plant_group /usr/local/bin/et-cat-shell \
  "${ET_CAT_IDS:-fedcba9876543210 fedcba9876543211 fedcba9876543212}" \
  "${ET_CAT_PASSKEYS:-fedcba9876543210fedcba9876543210 fedcba9876543211fedcba9876543211 fedcba9876543212fedcba9876543212}"

# Task 7 window-size test: a shell that traps SIGWINCH and echoes `stty size`
# (see et-resize-echo-shell). Its OWN never-reused id, same rule as above.
plant_group /usr/local/bin/et-resize-echo-shell \
  "${ET_RESIZE_ID:-abcdef0123456780}" \
  "${ET_RESIZE_PASSKEY:-abcdef0123456780abcdef0123456780}"

# Task 7 lifecycle test: a shell that exits immediately (see et-exit0-shell),
# to drive the remote-exit -> on_end path. Exec'd only on connect, so it exits
# right after the test connects (not at plant time).
plant_group /usr/local/bin/et-exit0-shell \
  "${ET_EXIT0_ID:-abcdef0123456781}" \
  "${ET_EXIT0_PASSKEY:-abcdef0123456781abcdef0123456781}"

# Task 7 roaming/replay CROWN JEWEL test: a cat-echo shell reached THROUGH the
# breakable toxiproxy relay. Unlike the plain cat ids above, the roaming test
# reuses THIS single id across the sever/restore -- which is exactly ET's
# roaming *recovery* path (the SAME logical client reconnecting after a dropped
# link), not two unrelated connects, so it is the correct/safe use of id-reuse
# (see the lifecycle note above). Its own dedicated id so it never collides
# with the plain cat tests.
# et-cat-raw-shell (not the plain et-cat-shell): raw + -echo so `cat` is the
# SOLE verbatim echo path, making the crown-jewel EXACT-bytes assertion
# (received() == expected) byte-for-byte checkable across the reconnect+replay
# (canonical mode would duplicate + '\n'->'\r\n'-mutate the stream).
plant_group /usr/local/bin/et-cat-raw-shell \
  "${ET_ROAM_ID:-abcdef0123456782}" \
  "${ET_ROAM_PASSKEY:-abcdef0123456782abcdef0123456782}"

touch /tmp/.et-credential-planted
