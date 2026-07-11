// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#include "eternaltermlib.h"
#include "session.hpp"

extern "C" et_client *et_connect(const et_config *cfg,
                                 const et_callbacks *cbs, void *ctx) {
  if (!cfg || !cbs || !cfg->host || !cfg->id || !cfg->passkey) return nullptr;
  // The three callbacks are the sole delivery mechanism and are all invoked
  // unconditionally on the transport thread (on_state fires on the very first
  // emitState); a NULL among them would be a NULL-function-pointer call ->
  // crash. Reject here so bad config fails safe with a NULL return, per this
  // ABI's documented contract, rather than crashing the internal thread.
  if (!cbs->on_bytes || !cbs->on_state || !cbs->on_end) return nullptr;
  if (cfg->env_count && (!cfg->env_keys || !cfg->env_vals)) return nullptr;
  for (size_t i = 0; i < cfg->env_count; i++)
    if (!cfg->env_keys[i] || !cfg->env_vals[i]) return nullptr;
  return reinterpret_cast<et_client *>(et_session_start(cfg, cbs, ctx));
}

extern "C" int et_send(et_client *c, const uint8_t *buf, size_t len) {
  if (!c) return ET_ERR_INVALID;
  if (len && !buf) return ET_ERR_INVALID;
  return et_session_send(reinterpret_cast<et_session *>(c), buf, len);
}

extern "C" int et_set_window_size(et_client *c, uint16_t cols, uint16_t rows,
                                  uint16_t width, uint16_t height) {
  if (!c) return ET_ERR_INVALID;
  return et_session_resize(reinterpret_cast<et_session *>(c),
                           cols, rows, width, height);
}

extern "C" void et_close(et_client *c) {
  if (!c) return;
  et_session_close(reinterpret_cast<et_session *>(c));
}
