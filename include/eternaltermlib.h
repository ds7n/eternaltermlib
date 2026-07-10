/* SPDX-FileCopyrightText: 2026 True Positive LLC
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * eternaltermlib — portable C client library for the Eternal Terminal (ET)
 * transport. This header is the ENTIRE public surface: a small callback-based
 * C ABI over ET's re-connectable byte stream. No PTY, no terminal, no platform
 * assumptions — usable from Swift/Obj-C (iOS), a Linux test harness, or any FFI.
 *
 * STATUS: scaffolding. Signatures below are the design sketch from
 * docs/specs/ and WILL be refined against ET's real ClientConnection API when
 * implementation begins. Nothing here is implemented yet.
 */

#ifndef ETERNALTERMLIB_H
#define ETERNALTERMLIB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque connection handle. */
typedef struct et_client et_client;

/* Connection lifecycle states reported via et_callbacks.on_state. */
typedef enum {
    ET_STATE_CONNECTING = 0,
    ET_STATE_CONNECTED = 1,
    ET_STATE_ROAMING = 2,     /* link lost; ET is re-establishing + will replay the gap */
    ET_STATE_DISCONNECTED = 3
} et_state;

/*
 * Caller-supplied callbacks. All are invoked on eternaltermlib's internal
 * transport thread; the caller is responsible for hopping to its own
 * queue/actor if needed. `ctx` is the opaque pointer passed to et_connect.
 */
typedef struct {
    /* Received plaintext stream bytes (already decrypted). Feed straight to the
     * consumer's terminal/tmux layer. `buf` is owned by the callee; copy if you
     * need it past the callback. */
    void (*on_bytes)(void *ctx, const uint8_t *buf, size_t len);

    /* Connection state transition. */
    void (*on_state)(void *ctx, et_state state);

    /* Terminal teardown (non-resumable). `reason` may be NULL. After on_end the
     * handle is dead; call et_close to free it. */
    void (*on_end)(void *ctx, const char *reason);
} et_callbacks;

/*
 * Open an ET connection to an already-bootstrapped endpoint.
 *
 *   host, port  — the etserver TCP endpoint (default port 2022).
 *   id          — the 16-char client id planted on the host at bootstrap.
 *   passkey     — the 32-char secretbox key planted at bootstrap.
 *   cbs, ctx    — callbacks + opaque context.
 *
 * The caller must have ALREADY planted (id, passkey) on the host over its own
 * SSH session (e.g. `echo '<id>/<passkey>_<TERM>' | etterminal`).
 * eternaltermlib does NOT perform the SSH bootstrap.
 *
 * Returns a handle, or NULL on immediate failure.
 */
et_client *et_connect(const char *host, uint16_t port,
                      const char *id, const char *passkey,
                      const et_callbacks *cbs, void *ctx);

/* Write bytes to the stream. Returns the number of bytes accepted, or a
 * negative value on error. */
int et_send(et_client *c, const uint8_t *buf, size_t len);

/* Tear down the connection and free the handle. Safe to call after on_end. */
void et_close(et_client *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ETERNALTERMLIB_H */
