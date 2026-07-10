# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0
#
# eternaltermlib — portable C client library for the Eternal Terminal transport.
# STATUS: scaffolding. Targets are stubbed; sources under src/ are placeholders
# until implementation begins (see docs/specs/).

cmake_minimum_required(VERSION 3.16)
project(eternaltermlib VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Always emit compile_commands.json so clangd (and the C LSP MCP) can index.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Dependencies (ET transport needs these) -------------------------------
# libsodium — ET's session crypto (secretbox). protobuf-lite — ET wire messages.
# find_package wiring is deferred until the vendored src/base build is added.
# find_package(unofficial-sodium CONFIG)   # or PkgConfig libsodium
# find_package(Protobuf CONFIG)

# --- Vendored Eternal Terminal transport core ------------------------------
# We compile ONLY src/base/ (the PTY-free ~2k-LOC transport) from the pinned
# submodule at extern/eternalterminal, never modified in-tree. The exact file
# list + generated ET.pb.* wiring lands with the implementation.
# set(ET_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/eternalterminal/src/base)

# --- The library ------------------------------------------------------------
# add_library(eternaltermlib
#     src/ETClient.cpp        # wraps ET ClientConnection/Backed*/CryptoHandler
#     src/shim.cpp            # the extern "C" C ABI (include/eternaltermlib.h)
#     ${ET_BASE_SOURCES}
# )
# target_include_directories(eternaltermlib PUBLIC include)
# target_link_libraries(eternaltermlib PRIVATE sodium protobuf-lite)

# --- Tests (Linux, against a Docker etserver fixture) -----------------------
# enable_testing()
# add_subdirectory(test)

message(STATUS "eternaltermlib: scaffolding build — targets stubbed pending implementation")
