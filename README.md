<!--
SPDX-FileCopyrightText: 2026 True Positive LLC
SPDX-License-Identifier: Apache-2.0
-->

# eternaltermlib

[![ci](https://github.com/ds7n/eternaltermlib/actions/workflows/ci.yml/badge.svg)](https://github.com/ds7n/eternaltermlib/actions/workflows/ci.yml)

A portable C client library for the [Eternal Terminal](https://github.com/MisterTea/EternalTerminal) (ET) transport: its re-connectable, roaming-tolerant encrypted byte stream, behind a small `extern "C"` callback API. No PTY, no terminal, no platform assumptions.

ET ships as executables only, with no embeddable API. This wraps ET's transport core (`src/base/`) behind one stable C ABI so the resumable connection embeds anywhere a byte stream is wanted (iOS/Android, Rust FFI, a test harness) without shelling out to `ssh`.

## Scope

**In:** connect to an `etserver` with an already-planted credential, send/receive the encrypted stream, survive roaming via ET's backed-buffer replay, report connection state.

**Out:** the SSH bootstrap (the caller plants `id`/`passkey` over its own SSH; this library takes them already-planted), PTY/terminal handling, port forwarding, jumphost mode.

## API

The full, documented surface is `include/eternaltermlib.h`. Callback-based, so no consumer owns a read loop; the three callbacks fire on the library's internal transport thread.

```c
et_client *et_connect(const et_config *cfg, const et_callbacks *cbs, void *ctx);
int  et_send(et_client *c, const uint8_t *buf, size_t len);
int  et_set_window_size(et_client *c, uint16_t cols, uint16_t rows, uint16_t w, uint16_t h);
void et_close(et_client *c);   /* joins the transport thread, frees; idempotent */

/* callbacks: on_bytes (decrypted output), on_state, on_end */
```

`et_config` carries host/port, the planted `id`/`passkey`, an env map (include `TERM`), and the initial window. Config strings are deep-copied, so the caller may free them once `et_connect` returns.

## Build & test

CMake against **libsodium** + **protobuf-lite**. Build and test in the `eternaltermlib-dev` Docker image:

```sh
docker compose build dev
HOST_UID=$(id -u) HOST_GID=$(id -g) docker compose run --rm dev cmake -B build -G Ninja
HOST_UID=$(id -u) HOST_GID=$(id -g) docker compose run --rm dev cmake --build build
HOST_UID=$(id -u) HOST_GID=$(id -g) docker compose run --rm dev ctest --test-dir build -LE ET_INTEGRATION
```

CI runs the unit suite, the integration + roaming/replay tests against a real `etserver` fixture, and the full suite under ThreadSanitizer and AddressSanitizer+UBSan. For iOS, build with `-DET_HTTP_TLS=OFF` (drops OpenSSL); see [`docs/porting-ios.md`](docs/porting-ios.md).

## License

**Apache-2.0**, © True Positive LLC. Matches upstream ET's Apache-2.0. Vendored ET keeps its own attribution (see [`NOTICE`](NOTICE)). REUSE-compliant. Run a license audit before shipping a combined binary (see [`docs/license-audit.md`](docs/license-audit.md)).
