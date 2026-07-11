// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#include "transport.hpp"
#include <gtest/gtest.h>

TEST(Transport, ConstructAndShutdownWithoutServer) {
  et::Transport t("127.0.0.1", 1 /* bogus port */,
                  "0123456789abcdef", "0123456789abcdef0123456789abcdef");
  EXPECT_LT(t.socketFd(), 0);         // no connection yet
  EXPECT_FALSE(t.isShuttingDown());
  t.shutdown();
  EXPECT_TRUE(t.isShuttingDown());    // shutdown is observable, idempotent
  t.shutdown();                       // second call must not crash
}
