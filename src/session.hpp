// SPDX-FileCopyrightText: 2026 True Positive LLC
// SPDX-License-Identifier: Apache-2.0
#ifndef ETERNALTERMLIB_SESSION_HPP
#define ETERNALTERMLIB_SESSION_HPP
#include "eternaltermlib.h"
typedef struct et_session et_session;
et_session *et_session_start(const et_config *cfg, const et_callbacks *cbs, void *ctx);
int  et_session_send(et_session *s, const uint8_t *buf, size_t len);
int  et_session_resize(et_session *s, uint16_t cols, uint16_t rows, uint16_t width, uint16_t height);
void et_session_close(et_session *s);
#endif
