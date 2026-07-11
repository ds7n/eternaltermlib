# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0
#
# eternaltermlib — portable C client library for the Eternal Terminal transport.

cmake_minimum_required(VERSION 3.16)
project(eternaltermlib VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
# ET's vendored cpp-httplib (Headers.hpp) unconditionally builds with
# CPPHTTPLIB_OPENSSL_SUPPORT, so src/base transitively needs OpenSSL + zlib.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

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
target_include_directories(et_base PUBLIC
    ${ET_BASE}
    ${ET_DIR}/src
    ${ET_EXTERNAL}/easyloggingpp/src
    ${ET_EXTERNAL}/PlatformFolders
    ${ET_EXTERNAL}/UniversalStacktrace/ust
    ${ET_EXTERNAL}/json/single_include
    ${ET_EXTERNAL}/base64
    ${ET_EXTERNAL}/cpp-httplib
    ${ET_EXTERNAL}/ThreadPool
    ${ET_EXTERNAL}/sole
    ${CMAKE_CURRENT_BINARY_DIR}          # generated *.pb.h
    ${Protobuf_INCLUDE_DIRS}
)
# ELPP flags ET's own base compiles with (from ET CMakeLists.txt).
target_compile_definitions(et_base PUBLIC
    ELPP_NO_DEFAULT_LOG_FILE ELPP_THREAD_SAFE ELPP_FORCE_USE_STD_THREAD)
target_link_libraries(et_base PUBLIC ${Protobuf_LIBRARIES} ${SODIUM_LIB} OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)

# --- Public library --------------------------------------------------------
add_library(eternaltermlib STATIC src/shim.cpp src/transport.cpp src/session.cpp)
target_include_directories(eternaltermlib PUBLIC include PRIVATE src)
target_link_libraries(eternaltermlib PRIVATE et_base)

enable_testing()
add_subdirectory(test)
