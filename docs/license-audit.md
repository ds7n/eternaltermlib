<!--
SPDX-FileCopyrightText: 2026 True Positive LLC
SPDX-License-Identifier: Apache-2.0
-->

# License audit — eternaltermlib

**Verdict: CLEAN.** All 14 linked/vendored dependencies are permissive
(Apache-2.0 / MIT / BSD-3-Clause / ISC / Zlib). No copyleft (GPL/LGPL/AGPL/
MPL/EPL) is compiled or linked into any shipped artifact. 0 blockers,
0 requires-attention.

This satisfies the CLAUDE.md **license gate** — run before distributing any
combined binary. Re-run when the vendored ET submodule is bumped or a new
linked dependency is added.

- **Project license:** Apache-2.0, © True Positive LLC.
- **Method:** C/CMake project — no language package manifest. Dependencies are
  system libraries (dev image), the vendored ET submodule, and one FetchContent
  test-only dep. Licenses resolved via Debian `copyright` files in the dev image
  plus submodule `LICENSE` files.
- **Audited at:** vendored ET pinned to `dfc75d663`; system deps per the
  `eternaltermlib-dev` image (Ubuntu 24.04).

## Classified dependencies

| Dependency | Version | License | Class | Linked into shipped artifact? |
|---|---|---|---|---|
| eternalterminal (vendored submodule) | @dfc75d663 | Apache-2.0 | compatible | yes (transport core) |
| libsodium | 1.0.18 | ISC | compatible | yes |
| protobuf (libprotobuf) | 3.21.12 | BSD-3-Clause | compatible\* | yes |
| openssl (libssl) | 3.0.13 | Apache-2.0 | compatible | yes (transitive via ET `cpp-httplib`) |
| zlib | 1.3 | Zlib | compatible | yes (transitive via ET `cpp-httplib`) |
| googletest | v1.15.2 | BSD-3-Clause | compatible | no (test-only) |
| ET vendored: easyloggingpp, json, cpp-httplib, PlatformFolders, base64 | vendored | MIT | compatible | partial (header-only, as used) |
| ET vendored: ThreadPool, sole | vendored | Zlib | compatible | partial (header-only, as used) |
| ET vendored: UniversalStacktrace | vendored | Apache-2.0 | compatible | partial (header-only, as used) |

**Counts: compatible 14 · requires-attention 0 · blocker 0.**

## Notes

- **\* protobuf GPL entries are build-only.** protobuf's Debian `copyright`
  lists two GPL-licensed files — `m4/ax_pthread.m4` (`GPLWithACException`, an
  autoconf macro carrying the configure-output exception) and `debian/*`
  (`GPL-3`, packaging scripts). Both are build tooling; neither is compiled or
  linked into any eternaltermlib artifact. The linked library code
  (`Files: *`) is BSD-3-Clause (Google).
- **OpenSSL is 3.x → Apache-2.0**, not the historical dual OpenSSL/SSLeay
  license with its advertising-style clause. No advertising obligation applies.
- **OpenSSL + zlib** entered the dependency set in the build spike (linking ET's
  `src/base`, which transitively pulls `cpp-httplib` with OpenSSL support enabled
  by ET's `Headers.hpp`). Both are permissive and cross-compilable.
- **Only attribution obligation:** Apache-2.0 §4 NOTICE propagation for the
  vendored ET source, satisfied by the repository-root `NOTICE` file. Preserve
  `NOTICE` in any redistributed combined work.
- **Cross-compile note:** OpenSSL is the one dependency that is non-trivial to
  cross-compile for iOS/Android. A future task may disable
  `CPPHTTPLIB_OPENSSL_SUPPORT` (its `#ifndef` guard permits an override without
  modifying vendored code) if the client transport never needs cpp-httplib's
  server features — which would drop OpenSSL from the link entirely.
