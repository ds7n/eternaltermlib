# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0
#
# eternaltermlib: portable C client library for the Eternal Terminal transport.

cmake_minimum_required(VERSION 3.16)
project(eternaltermlib VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)  # CMAKE_INSTALL_INCLUDEDIR/LIBDIR, used below + at install

# --- Sanitizer build option ------------------------------------------------
# -DET_SANITIZE=thread          -> ThreadSanitizer (validates the transport
#                                  thread / send queue / self-pipe model).
# -DET_SANITIZE=ON (or address) -> AddressSanitizer + UndefinedBehaviorSanitizer
#                                  (validates et_close frees cleanly, no leak/UB).
# Applied globally (compile + link) BEFORE any target is declared so it also
# instruments the vendored et_base and the gtest fetch. Requires the static
# sanitizer runtimes (libclang-rt-<ver>-dev in Dockerfile.dev). Off by default.
option(ET_SANITIZE "Build with a sanitizer: 'thread' (TSan) or ON/'address' (ASan+UBSan)" OFF)
if(ET_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
elseif(ET_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -g)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Protobuf REQUIRED)
find_library(SODIUM_LIB sodium REQUIRED)

# ET's vendored Headers.hpp includes cpp-httplib and, by default, #defines
# CPPHTTPLIB_OPENSSL_SUPPORT=1 -- which drags in OpenSSL + zlib. But the client
# TRANSPORT we compile never calls cpp-httplib's HTTP/TLS code (grep the base
# .cpp we build: zero SSL_/httplib:: call sites; the only crypto used is
# libsodium's secretbox). The OpenSSL link is dead weight from an include-only
# header path. OpenSSL is also the one dependency that is painful to
# cross-compile for iOS/Android.
#
# ET_HTTP_TLS=ON  (default) keeps the historical Linux behavior (link OpenSSL).
# ET_HTTP_TLS=OFF defines CPPHTTPLIB_OPENSSL_SUPPORT=0 via the header's own
# #ifndef guard (no vendored edit) and drops the OpenSSL/zlib link -- the
# configuration a mobile/embedded consumer wants. See docs/porting-ios.md.
option(ET_HTTP_TLS "Link OpenSSL/zlib for ET's cpp-httplib TLS support (unused by the transport; off for mobile)" ON)
if(ET_HTTP_TLS)
  find_package(OpenSSL REQUIRED)
  find_package(ZLIB REQUIRED)
endif()

set(ET_DIR      ${CMAKE_CURRENT_SOURCE_DIR}/extern/eternalterminal)
set(ET_EXTERNAL ${ET_DIR}/external)
set(ET_BASE     ${ET_DIR}/src/base)

# --- Generated ET protobuf (lite) -----------------------------------------
set(ET_PROTO_DIR ${ET_DIR}/proto)
protobuf_generate_cpp(ET_PB_SRCS ET_PB_HDRS
    ${ET_PROTO_DIR}/ET.proto
    ${ET_PROTO_DIR}/ETerminal.proto)

# --- Vendored ET base transport (client subset) ---------------------------
# Only the files the CLIENT transport reaches. Server/daemon/subprocess/tunnel
# units from upstream's et-lib are omitted; if the linker reports an undefined
# ET symbol, add that base .cpp here (do NOT modify files under extern/).
set(ET_BASE_SRCS
    ${ET_EXTERNAL}/easyloggingpp/src/easylogging++.cc
    ${ET_EXTERNAL}/PlatformFolders/sago/platform_folders.cpp
    ${ET_BASE}/BackedReader.cpp
    ${ET_BASE}/BackedWriter.cpp
    ${ET_BASE}/ClientConnection.cpp
    ${ET_BASE}/Connection.cpp
    ${ET_BASE}/CryptoHandler.cpp
    ${ET_BASE}/SocketHandler.cpp
    ${ET_BASE}/TcpSocketHandler.cpp
    ${ET_BASE}/PipeSocketHandler.cpp
    ${ET_BASE}/UnixSocketHandler.cpp
    ${ET_BASE}/LogHandler.cpp
    ${ET_BASE}/RawSocketUtils.cpp
)

add_library(et_base STATIC ${ET_BASE_SRCS} ${ET_PB_SRCS})
# These are ET-internal build-tree includes; a consumer of the installed lib
# never sees them (only include/eternaltermlib.h). Wrap in BUILD_INTERFACE so
# they apply while WE compile but are NOT exported to the install tree (an
# exported absolute build path fails install(EXPORT)).
target_include_directories(et_base PUBLIC
    $<BUILD_INTERFACE:${ET_BASE}>
    $<BUILD_INTERFACE:${ET_DIR}/src>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/easyloggingpp/src>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/PlatformFolders>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/UniversalStacktrace/ust>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/json/single_include>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/base64>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/cpp-httplib>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/ThreadPool>
    $<BUILD_INTERFACE:${ET_EXTERNAL}/sole>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>   # generated *.pb.h
    $<BUILD_INTERFACE:${Protobuf_INCLUDE_DIRS}>
)
# ELPP flags ET's own base compiles with (from ET CMakeLists.txt).
target_compile_definitions(et_base PUBLIC
    ELPP_NO_DEFAULT_LOG_FILE ELPP_THREAD_SAFE ELPP_FORCE_USE_STD_THREAD)
target_link_libraries(et_base PUBLIC ${Protobuf_LIBRARIES} ${SODIUM_LIB})
if(ET_HTTP_TLS)
  target_link_libraries(et_base PUBLIC OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
else()
  # Suppress cpp-httplib ENTIRELY (and with it OpenSSL + zlib), without editing
  # vendored source. Why not just CPPHTTPLIB_OPENSSL_SUPPORT=0? httplib gates
  # its SSL code with `#ifdef CPPHTTPLIB_OPENSSL_SUPPORT` (defined-ness, not
  # value), and ET's Headers.hpp force-#defines it via its own #ifndef guard --
  # so =0 still emits the SSL code and needs OpenSSL to link. Instead we
  # pre-define httplib.h's own include guard (CPPHTTPLIB_HTTPLIB_H), which makes
  # Headers.hpp's `#include "httplib.h"` a no-op. The base TRANSPORT never calls
  # any httplib:: symbol (verified: zero use sites in the base .cpp we compile),
  # so dropping the header is safe. This is the mobile/iOS/Android config.
  target_compile_definitions(et_base PUBLIC CPPHTTPLIB_HTTPLIB_H=1)
endif()

# --- Public library --------------------------------------------------------
add_library(eternaltermlib STATIC src/shim.cpp src/transport.cpp src/session.cpp)
target_include_directories(eternaltermlib
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE src)
target_link_libraries(eternaltermlib PRIVATE et_base)
# session.cpp/transport.cpp include ET's Headers.hpp directly, so they must
# suppress httplib.h the same way et_base does when TLS is dropped -- linking
# et_base PRIVATE does not carry its compile-defs to our own TUs. Setting this
# PUBLIC also propagates to the test targets that include session_internal.hpp,
# so every first-party TU that pulls in Headers.hpp agrees (a mismatch would
# link the SSL code paths back in -> undefined SSL_* at link time).
if(NOT ET_HTTP_TLS)
  target_compile_definitions(eternaltermlib PUBLIC CPPHTTPLIB_HTTPLIB_H=1)
endif()

# --- Install / export (for consumers that don't add_subdirectory) -----------
# The recommended consumption is still to vendor this repo and add_subdirectory()
# it (that is how semicolyn embeds it, like extern/mosh). This install/export is
# for consumers who prefer to `find_package(eternaltermlib)` against an install
# tree. It exports the target eternaltermlib::eternaltermlib.
#
# eternaltermlib links et_base PRIVATE, but et_base is a separate STATIC archive
# holding the ET transport objects, so a consumer linking the *installed* static
# lib needs both archives on its link line. We install both and put et_base in
# eternaltermlib's INTERFACE link libraries via the export set.
include(CMakePackageConfigHelpers)

# Promote the et_base dependency into the public link interface for the export
# (a static consumer must link et_base too; PRIVATE would drop it from the
# installed usage requirements).
target_link_libraries(eternaltermlib INTERFACE et_base)

install(TARGETS eternaltermlib et_base
        EXPORT eternaltermlib-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES include/eternaltermlib.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT eternaltermlib-targets
        FILE eternaltermlib-targets.cmake
        NAMESPACE eternaltermlib::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eternaltermlib)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/eternaltermlib-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

# Config: re-find every dependency whose IMPORTED target name appears in the
# exported et_base link interface, BEFORE including the targets file. If any is
# missing, set_target_properties() on the imported et_base fails with an unknown
# target (e.g. OpenSSL::SSL). Protobuf is always needed; OpenSSL + ZLIB only when
# this was built with ET_HTTP_TLS=ON (they are dropped otherwise). sodium is
# linked by absolute path, so it needs no find_dependency.
set(_etl_tls_config "")
if(ET_HTTP_TLS)
  set(_etl_tls_config "find_dependency(OpenSSL)\nfind_dependency(ZLIB)\n")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/eternaltermlib-config.cmake"
"include(CMakeFindDependencyMacro)\n\
find_dependency(Protobuf)\n\
${_etl_tls_config}\
include(\"\${CMAKE_CURRENT_LIST_DIR}/eternaltermlib-targets.cmake\")\n")

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/eternaltermlib-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/eternaltermlib-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/eternaltermlib)

enable_testing()
add_subdirectory(test)
