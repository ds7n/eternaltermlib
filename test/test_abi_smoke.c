/* SPDX-FileCopyrightText: 2026 True Positive LLC
 * SPDX-License-Identifier: Apache-2.0 */
#include "eternaltermlib.h"
#include <assert.h>
#include <stddef.h>

static void on_bytes(void *c, const uint8_t *b, size_t n){(void)c;(void)b;(void)n;}
static void on_state(void *c, et_state s){(void)c;(void)s;}
static void on_end(void *c, const char *r){(void)c;(void)r;}

int main(void) {
  et_callbacks cbs = { on_bytes, on_state, on_end };
  /* NULL config -> NULL handle (typed failure at boundary). */
  assert(et_connect(NULL, &cbs, NULL) == NULL);
  /* NULL host -> NULL handle. */
  et_config bad = {0};
  bad.id = "id"; bad.passkey = "key";
  assert(et_connect(&bad, &cbs, NULL) == NULL);

  /* A NULL callback pointer must fail safe with a NULL return, NOT a crash on
   * the transport thread. Use an otherwise-valid config so the callback guard
   * (not the config guard) is what rejects it. Each of the three callbacks is
   * required; test each NULL independently. */
  et_config ok = {0};
  ok.host = "h"; ok.id = "id"; ok.passkey = "key";
  et_callbacks no_bytes = { NULL,     on_state, on_end };
  et_callbacks no_state = { on_bytes, NULL,     on_end };
  et_callbacks no_end   = { on_bytes, on_state, NULL    };
  assert(et_connect(&ok, &no_bytes, NULL) == NULL);
  assert(et_connect(&ok, &no_state, NULL) == NULL);
  assert(et_connect(&ok, &no_end,   NULL) == NULL);

  /* et_send on NULL handle -> ET_ERR_INVALID (not a crash, not 0). */
  assert(et_send(NULL, (const uint8_t*)"x", 1) == ET_ERR_INVALID);
  /* et_close(NULL) is a safe no-op. */
  et_close(NULL);
  return 0;
}
