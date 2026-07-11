// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#ifndef ETERNALTERMLIB_SESSION_INTERNAL_HPP
#define ETERNALTERMLIB_SESSION_INTERNAL_HPP
#include <cstddef>
#include <cstdint>
#include <string>
#include "ETerminal.pb.h"
#include "ET.pb.h"
#include "Headers.hpp"  // protoToString / stringToProto

namespace et {

// Pure protocol-payload builders, factored out so they can be unit-tested
// without standing up a Transport or the session thread.

// InitialPayload with jumphost=false and env_keys[i] -> env_vals[i] (incl.
// TERM). Returns the protobuf-lite wire encoding.
std::string buildInitialPayload(const char *const *keys,
                                const char *const *vals, size_t count);

// TerminalBuffer wrapping `len` raw bytes at `buf`.
std::string buildTerminalBuffer(const uint8_t *buf, size_t len);

// TerminalInfo mapping cols -> column, rows -> row (plus width/height px).
std::string buildTerminalInfo(uint16_t cols, uint16_t rows,
                              uint16_t width, uint16_t height);

}  // namespace et
#endif
