# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0

# Under -DET_SANITIZE=thread, hand every ctest process a TSAN_OPTIONS that (a)
# points at the static llvm-symbolizer (unsuffixed name is absent on Ubuntu 24.04
# -- only llvm-symbolizer-18 exists -- so unsymbolized `<null>` frames would make
# it impossible to tell OUR races from vendored-ET ones) and (b) loads the
# documented vendored-ET race suppressions. Applied as a per-test ENV property so
# a non-sanitized `ctest` run is unaffected. See ../.tsan-suppressions.
if(ET_SANITIZE STREQUAL "thread")
  find_program(ET_LLVM_SYMBOLIZER
    NAMES llvm-symbolizer llvm-symbolizer-18 llvm-symbolizer-17)
  set(_et_tsan_opts "suppressions=${CMAKE_SOURCE_DIR}/.tsan-suppressions")
  if(ET_LLVM_SYMBOLIZER)
    set(_et_tsan_opts "${_et_tsan_opts} external_symbolizer_path=${ET_LLVM_SYMBOLIZER}")
  endif()
  set(ET_TSAN_TEST_ENV "TSAN_OPTIONS=${_et_tsan_opts}")
endif()

# Attach ET_TSAN_TEST_ENV (if set) to a test's ENV property. No-op off TSan.
function(et_apply_tsan_env test_name)
  if(ET_TSAN_TEST_ENV)
    set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT "${ET_TSAN_TEST_ENV}")
  endif()
endfunction()

add_executable(test_abi_smoke test_abi_smoke.c)
target_link_libraries(test_abi_smoke PRIVATE eternaltermlib)
add_test(NAME abi_smoke COMMAND test_abi_smoke)
et_apply_tsan_env(abi_smoke)

# --- GoogleTest (unit tests that need C++ assertions) ----------------------
# Fetched at configure time (dev container has network access); if a future
# environment lacks network, vendor googletest as a git submodule under
# extern/googletest and add_subdirectory() it instead.
include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)

add_executable(test_transport_unit test_transport_unit.cpp)
target_include_directories(test_transport_unit PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_transport_unit PRIVATE eternaltermlib et_base gtest_main)
add_test(NAME transport_unit COMMAND test_transport_unit)
et_apply_tsan_env(transport_unit)

# session_internal.hpp pulls in ET proto + Headers.hpp; et_base exposes both its
# own include dirs and the generated *.pb.h (binary dir) transitively.
add_executable(test_session_payload test_session_payload.cpp)
target_include_directories(test_session_payload PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_session_payload PRIVATE eternaltermlib et_base gtest_main)
add_test(NAME SessionPayload COMMAND test_session_payload)
et_apply_tsan_env(SessionPayload)

# --- Integration tests (require a live etserver fixture) -------------------
# Labeled ET_INTEGRATION so a plain `ctest` run (no fixture) can still be
# filtered to skip these; see docker-compose.yml's `integration` profile /
# `dev-integration` service for how the fixture is brought up before this
# suite runs.
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE eternaltermlib gtest_main)
target_include_directories(test_integration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME integration COMMAND test_integration)
set_tests_properties(integration PROPERTIES LABELS "ET_INTEGRATION")
et_apply_tsan_env(integration)

# Roaming/replay crown-jewel test: connects THROUGH the toxiproxy relay and
# severs/restores the link mid-stream (see test_roaming.cpp + proxy_control.hpp
# + docker-compose.yml's `toxiproxy` service). Same ET_INTEGRATION label so it
# runs only under an integration fixture, never the plain unit suite.
add_executable(test_roaming test_roaming.cpp)
target_link_libraries(test_roaming PRIVATE eternaltermlib gtest_main)
target_include_directories(test_roaming PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME Roaming COMMAND test_roaming)
set_tests_properties(Roaming PROPERTIES LABELS "ET_INTEGRATION")
et_apply_tsan_env(Roaming)
