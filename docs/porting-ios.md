<!--
SPDX-FileCopyrightText: 2026 True Positive LLC
SPDX-License-Identifier: Apache-2.0
-->

# Building eternaltermlib for iOS (guidance for consumers)

eternaltermlib is a **portable C library**. It does not build an iOS artifact
itself: a consumer vendors it as source and compiles it inside its own iOS
build (the [semicolyn](https://github.com/ds7n/semicolyn) app does this, like
`extern/mosh`). This repo's job is to be correct and portable; the iOS
packaging belongs to the consumer that already owns an iOS toolchain.

This doc captures what a consumer needs to build it for iOS, including the
findings that make it work.

## The one thing that makes iOS viable: drop OpenSSL

Configure with **`-DET_HTTP_TLS=OFF`**. ET's `Headers.hpp` pulls in cpp-httplib
with TLS support, which drags in OpenSSL + zlib, and OpenSSL is the one
dependency that is painful to cross-compile for iOS. The transport never calls
cpp-httplib, so `ET_HTTP_TLS=OFF` suppresses the whole header (no vendored
edit) and drops OpenSSL + zlib.

Verified on Linux: with `-DET_HTTP_TLS=OFF` the full library + tests build and
link with zero OpenSSL symbols, and the real transport (handshake, streaming,
roaming/replay) works. The only remaining link deps are **libsodium** and
**protobuf-lite**, both of which cross-compile to iOS.

## Remaining link deps for the iOS target

- **libsodium** for iOS. libsodium ships `dist-build/apple-xcframework.sh`
  (its only iOS build path; it builds all Apple slices and is slow, so cache
  it). Note it produces an `.xcframework`, not a plain `lib/`+`include/` prefix
  that `find_library(sodium)` expects, so you either point the build at the
  extracted per-arch static lib inside the xcframework, or reuse an
  iOS-libsodium your app already builds.
- **protobuf-lite** for iOS. There is no official iOS build script; established
  community build scripts exist (search "protobuf iOS static library"). The
  `protoc` compiler runs on the **host** (for codegen), only the protobuf-lite
  *runtime* needs the iOS target build.

## CMake toolchain: use a mature one, do not hand-roll

Use **[leetal/ios-cmake](https://github.com/leetal/ios-cmake)** (a widely-used
iOS CMake toolchain). It supports the platform values this project needs
(`OS64` device arm64, `SIMULATORARM64`, and `OS64COMBINED` for a device+sim FAT
library) and handles the `find_library` / simulator-vs-device switching that a
hand-rolled toolchain gets wrong. Point `CMAKE_TOOLCHAIN_FILE` at it.

Two settings a consumer's toolchain must get right (learned the hard way):

- **`CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH`** (not `ONLY`). This project calls
  `find_package(Protobuf REQUIRED)` for the **host** protoc + codegen wiring;
  `ONLY` restricts package search to the iOS SDK and configure fails because
  the host Protobuf is invisible. `BOTH` lets host packages resolve while
  target link libs still come from the SDK/prefix (`LIBRARY`/`INCLUDE ONLY`).
- Make the iOS-built libsodium/protobuf prefixes searchable by the `ONLY`
  library mode (append them to `CMAKE_FIND_ROOT_PATH`, not just
  `CMAKE_PREFIX_PATH`).

## Sketch

```sh
cmake -B build-ios -G Xcode \
  -DCMAKE_TOOLCHAIN_FILE=<path>/leetal-ios.toolchain.cmake \
  -DPLATFORM=OS64COMBINED \
  -DET_HTTP_TLS=OFF \
  -DCMAKE_PREFIX_PATH="<iOS libsodium>;<iOS protobuf-lite>"
cmake --build build-ios --config Release
```

## Building it in CI without a Mac

If the consumer (like semicolyn) has no Mac and relies on GitHub Actions
`macos-latest` runners, see the companion CI recipe handed off separately: it
encodes the toolchain choice, the dep sourcing, and the `PACKAGE BOTH` fix so
the iOS build can be developed in CI without burning many blind ~20-minute
iterations.
