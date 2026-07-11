#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 True Positive LLC
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
/src/et/build/etserver --port 2022 &
SRV=$!
for _ in $(seq 1 30); do
  if nc -z 127.0.0.1 2022; then break; fi
  sleep 0.2
done
# Plant the known credential ONCE: registration is durable for the container's
# lifetime (see plant_credential.sh for the upstream-source-backed lifecycle
# finding). etterminal daemonizes and keeps the PTY session alive in the
# background, so this call returns quickly once the daemon fork completes.
/usr/local/bin/plant_credential.sh
wait "$SRV"
