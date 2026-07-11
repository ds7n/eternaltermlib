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
  /* et_send on NULL handle -> ET_ERR_INVALID (not a crash, not 0). */
  assert(et_send(NULL, (const uint8_t*)"x", 1) == ET_ERR_INVALID);
  /* et_close(NULL) is a safe no-op. */
  et_close(NULL);
  return 0;
}
