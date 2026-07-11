# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0
#
# Minimal iOS / iOS-Simulator CMake toolchain for eternaltermlib.
#
# Cross-compiles the portable C core (src/ + the vendored ET src/base subset)
# for Apple platforms. REQUIRES a macOS host with Xcode + the iOS SDK -- it
# cannot be exercised on Linux (no iOS SDK). It is committed so a macOS CI
# runner (e.g. GitHub Actions `macos-latest`, free for public repos) can build
# and keep the cross-compile green.
#
# Pair it with -DET_HTTP_TLS=OFF so no OpenSSL/zlib is required (see
# docs/porting-ios.md): libsodium + protobuf-lite are the only remaining
# link-time deps, and both cross-compile to iOS.
#
# Usage (on macOS):
#   cmake -B build-ios -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#     -DIOS_PLATFORM=OS64 \        # OS64 = device arm64; SIMULATOR64 = x86_64 sim; SIMULATORARM64 = arm64 sim
#     -DET_HTTP_TLS=OFF \
#     -DCMAKE_INSTALL_PREFIX=... \
#     <plus paths to iOS-built libsodium/protobuf, e.g. via -DCMAKE_PREFIX_PATH>
#   cmake --build build-ios
#
# NOTE: this is a lean, hand-rolled toolchain covering what eternaltermlib
# needs (a static lib, no bundle/signing). For a full-featured iOS toolchain
# (bitcode, catalyst, combined xcframework) consider leetal/ios-cmake; this
# file intentionally stays small and dependency-free.

set(CMAKE_SYSTEM_NAME iOS)

# Deployment target -- match semicolyn's when it vendors this; 15.0 is a safe
# floor for a modern app. Override with -DDEPLOYMENT_TARGET=<v>.
if(NOT DEFINED DEPLOYMENT_TARGET)
  set(DEPLOYMENT_TARGET "15.0")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET "${DEPLOYMENT_TARGET}" CACHE STRING "iOS deployment target")

# Platform selection -> SDK + architecture.
if(NOT DEFINED IOS_PLATFORM)
  set(IOS_PLATFORM "OS64")
endif()

if(IOS_PLATFORM STREQUAL "OS64")
  set(CMAKE_OSX_SYSROOT "iphoneos")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
elseif(IOS_PLATFORM STREQUAL "SIMULATORARM64")
  set(CMAKE_OSX_SYSROOT "iphonesimulator")
  set(CMAKE_OSX_ARCHITECTURES "arm64")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR64")
  set(CMAKE_OSX_SYSROOT "iphonesimulator")
  set(CMAKE_OSX_ARCHITECTURES "x86_64")
else()
  message(FATAL_ERROR "IOS_PLATFORM must be OS64, SIMULATORARM64, or SIMULATOR64 (got '${IOS_PLATFORM}')")
endif()

# We build a static library only -- no code signing, no app bundle.
set(CMAKE_MACOSX_BUNDLE OFF)

# Standard cross-compile search-mode: find headers/libs in the SDK, run host
# tools from the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
