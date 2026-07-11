<!--
SPDX-FileCopyrightText: 2026 True Positive LLC
SPDX-License-Identifier: Apache-2.0
-->

# Porting eternaltermlib to iOS (for semicolyn)

Status of the iOS cross-compile, what is proven on Linux, and the remaining
work that needs a macOS host. Per the layering in the README, semicolyn
**vendors this repo as source and compiles it in its own iOS build** (like
`extern/mosh`); this repo produces a portable C core + a CMake target, not a
prebuilt iOS artifact. The Swift wrapper (`libetios`) lives in semicolyn.

## What is DONE and verified (on Linux)

### The OpenSSL blocker is removed — `-DET_HTTP_TLS=OFF`

The base transport pulled in OpenSSL + zlib only because ET's `Headers.hpp`
includes `cpp-httplib` with TLS support. **The transport never calls any
`httplib::` symbol** (zero use sites in the base `.cpp` we compile) — it was
pure dead weight, and OpenSSL is the one dependency that is genuinely painful to
cross-compile for iOS.

`ET_HTTP_TLS` (CMake option, default `ON` to preserve the Linux build) controls
this:

- `ON`  — link OpenSSL/zlib (historical Linux behavior).
- `OFF` — suppress `httplib.h` entirely (by pre-defining its include guard
  `CPPHTTPLIB_HTTPLIB_H`, so the `#include` becomes a no-op — **no vendored
  edit**), dropping OpenSSL + zlib. This is the mobile config.

> Why not `CPPHTTPLIB_OPENSSL_SUPPORT=0`? httplib gates its SSL code with
> `#ifdef` (defined-ness, not value) and `Headers.hpp` force-`#define`s it, so
> `=0` still emits the SSL code. Suppressing the whole header is the clean lever.

**Verified with `-DET_HTTP_TLS=OFF` on Linux:**
- Full library + tests compile and link with **zero OpenSSL symbols**
  (`nm -u libeternaltermlib.a | grep SSL_` is empty).
- **Unit suite 3/3** and **integration + roaming 2/2** pass — the real ET
  handshake, streaming, and roaming/replay all work without httplib/OpenSSL.

So the remaining link-time deps for the mobile config are **libsodium** +
**protobuf-lite** only — both of which cross-compile to iOS/Android.

### An iOS CMake toolchain file exists

`cmake/ios.toolchain.cmake` — a lean toolchain for `iOS` / `iOS Simulator`
(device arm64, sim arm64, sim x86_64). Committed so a macOS runner can use it;
it cannot be exercised on Linux.

## What REMAINS (needs a macOS host with Xcode — no Mac available in this repo's CI yet)

1. **Actually run the iOS cross-compile.** On macOS:
   ```sh
   cmake -B build-ios -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
     -DIOS_PLATFORM=OS64 \
     -DET_HTTP_TLS=OFF \
     -DCMAKE_PREFIX_PATH="<path to iOS-built libsodium & protobuf>"
   cmake --build build-ios
   ```
   This will surface any remaining POSIX assumptions in ET's `src/base` that
   don't hold on iOS (it targets Linux/macOS/Windows; iOS is close to macOS but
   sandboxed — watch for `fork`/PTY/filesystem calls, though the transport
   subset we compile should be clean).

2. **iOS-built libsodium + protobuf-lite.** Provide these for the target arch
   (e.g. via a package manager that supports iOS, a prebuilt, or building them
   with the same toolchain). protobuf-lite is the runtime we link; the `protoc`
   compiler still runs on the **host**, not the target.

3. **A consumable artifact / install target.** Currently `eternaltermlib` is an
   in-tree `STATIC` target with no `install()`/`export()`. Decide the contract
   semicolyn uses: either it `add_subdirectory()`s this repo directly (simplest,
   matches the mosh pattern), or we add an install/export + optionally bundle an
   `.xcframework` (device + sim slices) in a macOS CI job.

4. **macOS CI to keep it green.** A GitHub Actions `macos-latest` job
   (free for public repos) running step 1 for each platform slice — so the iOS
   cross-compile is enforced, not a one-time check. This is the payoff of making
   the repo public: free Apple runners.

## Summary

| Piece | Status |
|---|---|
| Drop OpenSSL (the hard iOS dep) | ✅ done, verified on Linux (`-DET_HTTP_TLS=OFF`) |
| iOS toolchain file | ✅ committed (`cmake/ios.toolchain.cmake`) |
| Run the arm64-apple-ios build | ⏳ needs macOS + Xcode |
| iOS-built libsodium/protobuf | ⏳ needs macOS |
| Install/export / xcframework | ⏳ decide contract with semicolyn |
| macOS CI gate | ⏳ set up when repo goes public (free runners) |

The Linux-side blocker (OpenSSL) is retired; the rest is macOS-host work that
this environment cannot perform, staged and documented so it can be picked up on
a Mac or a macOS CI runner.
