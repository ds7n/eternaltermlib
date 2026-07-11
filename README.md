<!--
SPDX-FileCopyrightText: 2026 True Positive LLC
SPDX-License-Identifier: Apache-2.0
-->

# eternaltermlib

[![ci](https://github.com/ds7n/eternaltermlib/actions/workflows/ci.yml/badge.svg)](https://github.com/ds7n/eternaltermlib/actions/workflows/ci.yml)
[![ios-cross](https://github.com/ds7n/eternaltermlib/actions/workflows/ios-cross.yml/badge.svg)](https://github.com/ds7n/eternaltermlib/actions/workflows/ios-cross.yml)

A **portable C client library for the [Eternal Terminal](https://github.com/MisterTea/EternalTerminal) (ET) transport** — the re-connectable, roaming-tolerant secure byte stream, exposed behind a small `extern "C"` callback API with **no PTY, no terminal, and no platform assumptions**.

It wraps upstream ET's transport core (`src/base/`) so the resumable connection can be embedded anywhere a plain byte stream is wanted — an iOS/Android app, a Rust FFI consumer, a test harness — without shelling out to `ssh` or emulating a terminal.

## Why this exists

ET is shipped as an *application*, not a library: upstream builds executables only (`et`, `etserver`, `etterminal`), with no exported headers or embeddable API. Embedders (see [ET issue #452](https://github.com/MisterTea/EternalTerminal/issues/452)) have had to reach into internal C++ classes. `eternaltermlib` provides the missing clean boundary: one stable C ABI over the (PTY-free, ~2,000 LOC) ET transport.

It was extracted for [semicolyn](https://github.com/ds7n/semicolyn) (an iOS SSH/mosh terminal), which needs native `tmux -CC` panes **and** network roaming at once — the one thing mosh structurally can't do. But nothing here is semicolyn- or iOS-specific.

## Scope

**In scope:** connect to an `etserver` (given an already-planted bootstrap credential), send/receive an encrypted byte stream, survive roaming via ET's backed-buffer replay, report connection state — all behind a C ABI.

**Out of scope (deliberately):**
- **The SSH bootstrap.** ET self-generates an `id`+`passkey` and normally shells out to the system `ssh` to plant them on the host (`echo '<id>/<passkey>_<TERM>' | etterminal`). `eternaltermlib` takes an *already-planted* credential — the caller runs that command over whatever SSH it already has (semicolyn uses russh). This is the clean seam that removes ET's one non-portable dependency.
- PTY/terminal handling, port forwarding beyond the shell session, jumphost mode.

## Layering (for the semicolyn use case)

```
eternaltermlib (this repo, portable C)  ──vendored+linked──▶  libetios (thin iOS wrapper, in semicolyn)  ──▶  semicolyn app
```

`libetios` (which lives in the semicolyn repo, like `extern/mosh`) is the only iOS-specific layer: it adapts this C ABI to Swift + the app lifecycle and packages an xcframework.

## C ABI (see `include/eternaltermlib.h` for the full, documented surface)

Connection parameters go through a growable `et_config` struct; the three callbacks (`on_bytes`/`on_state`/`on_end`) all fire on the library's internal transport thread.

```c
#include "eternaltermlib.h"
#include <string.h>

static void on_bytes(void *ctx, const uint8_t *buf, size_t len) {
    /* Decrypted shell output. `buf` is valid ONLY for this call — copy if kept. */
    fwrite(buf, 1, len, stdout);
}
static void on_state(void *ctx, et_state st)      { /* CONNECTING/CONNECTED/ROAMING/DISCONNECTED */ }
static void on_end(void *ctx, const char *reason) { /* session over (reason may be NULL); call et_close */ }

int main(void) {
    /* InitialPayload env map — parallel key/value arrays; include "TERM". */
    const char *keys[] = { "TERM" };
    const char *vals[] = { "xterm-256color" };

    et_config cfg = {0};
    cfg.host      = "host.example";
    cfg.port      = 2022;                 /* 0 -> default 2022 */
    cfg.id        = "0123456789abcdef";   /* already-planted bootstrap id */
    cfg.passkey   = "0123456789abcdef0123456789abcdef";
    cfg.env_keys  = keys;
    cfg.env_vals  = vals;
    cfg.env_count = 1;
    cfg.cols = 80; cfg.rows = 24;         /* initial window, char cells */

    et_callbacks cbs = { on_bytes, on_state, on_end };

    et_client *c = et_connect(&cfg, &cbs, /*ctx=*/NULL);   /* NULL only on bad args */
    if (!c) return 1;

    const char cmd[] = "ls -la\n";
    et_send(c, (const uint8_t *)cmd, strlen(cmd));         /* keystrokes -> TERMINAL_BUFFER */
    et_set_window_size(c, 120, 40, 0, 0);                  /* resize -> TERMINAL_INFO */

    /* ... run until on_end / app teardown ... */
    et_close(c);                          /* joins the transport thread, frees; idempotent */
    return 0;
}
```

Callback-based (not blocking `read`/`write`) so no consumer owns a read loop, and the same ABI works identically on Linux (CI test harness) and iOS. `cfg` strings are deep-copied, so the caller may free them once `et_connect` returns.

## Build & test

Builds with CMake against **libsodium** + **protobuf-lite** (both cross-compile to iOS/Android). Tested on Linux CI against a real `etserver` fixture (Docker Compose), including a **roaming/reconnect-replay** test (drop the fd mid-stream, reconnect, assert the backed buffer replays exactly the missed bytes).

> Status: **implemented.** The full C ABI (`et_connect`/`et_send`/`et_set_window_size`/`et_close` + `on_bytes`/`on_state`/`on_end`) is in place and integration-tested on Linux CI against a real `etserver` fixture — including the roaming/reconnect-replay crown-jewel test (sever the link mid-stream via toxiproxy, reconnect, assert the backed buffer replays **exactly** the missed bytes). The suite additionally runs clean under ThreadSanitizer (transport thread + send queue + self-pipe) and AddressSanitizer+UBSan (`et_close` frees cleanly, idempotent double-close is memory-safe). Build/test in the `eternaltermlib-dev` Docker image; see `docs/specs/`.

## License gate before shipping

Both this library (**Apache-2.0**) and upstream ET (**Apache-2.0**) are permissive and match, so the combined work is cleanly Apache-2.0 — you still must **preserve ET's attribution** (see `NOTICE`). Run a license audit before distributing any vendored/combined binary as a matter of hygiene (confirm no transitively-linked dep — libsodium/protobuf — imposes stricter terms).

## License

**Apache-2.0**, © True Positive LLC. Matches upstream ET's Apache-2.0 for the cleanest combined-work and upstreaming story, and lets any consumer (including closed-source terminals) embed it. Vendored ET source retains its own Apache-2.0 + attribution (see `NOTICE`). REUSE-compliant (SPDX headers on all first-party files).
