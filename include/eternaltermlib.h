/* SPDX-FileCopyrightText: 2026 True Positive LLC
 * SPDX-License-Identifier: Apache-2.0 */

/*
 * eternaltermlib — portable C client library for the Eternal Terminal (ET)
 * transport. This header is the ENTIRE public surface: a small callback-based
 * C ABI over ET's re-connectable byte stream. No PTY, no terminal, no platform
 * assumptions — usable from Swift/Obj-C (iOS), a Linux test harness, or any FFI.
 *
 * All callbacks fire on eternaltermlib's internal transport thread; the caller
 * is responsible for hopping to its own queue/actor if needed. The `buf`
 * passed to on_bytes is owned by the callee and valid ONLY for the duration
 * of the callback — copy it if you need the bytes past the call.
 *
 * eternaltermlib takes an ALREADY-planted (id, passkey) credential; it does
 * NOT perform the SSH bootstrap ET normally does via system `ssh`.
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
    ET_STATE_CONNECTING   = 0,
    ET_STATE_CONNECTED    = 1,
    ET_STATE_ROAMING      = 2,  /* link lost; ET is reconnecting + will replay the gap */
    ET_STATE_DISCONNECTED = 3
} et_state;

/* Typed failure codes. Never used for "you passed garbage" via a bare NULL
 * return from et_connect — see et_connect's own doc comment for that case. */
typedef enum {
    ET_ERR_CLOSED  = -1,        /* handle already torn down */
    ET_ERR_INVALID = -2         /* bad argument */
} et_err;

/*
 * Caller-supplied callbacks. All are invoked on eternaltermlib's internal
 * transport thread; the caller is responsible for hopping to its own
 * queue/actor if needed. `ctx` is the opaque pointer passed to et_connect.
 */
typedef struct {
    /* Received plaintext stream bytes (already decrypted), decoded shell
     * output. `buf` is owned by the callee and valid only during the
     * callback; copy it if you need the bytes past the call. */
    void (*on_bytes)(void *ctx, const uint8_t *buf, size_t len);

    /* Connection state transition. */
    void (*on_state)(void *ctx, et_state state);

    /* Terminal teardown (non-resumable). `reason` may be NULL. After on_end
     * the handle is dead; call et_close to free it. */
    void (*on_end)(void *ctx, const char *reason);
} et_callbacks;

/*
 * Connection parameters. A params struct (rather than positional args) so
 * the ABI can grow without breaking callers.
 */
typedef struct {
    const char        *host;
    uint16_t           port;          /* 0 -> default 2022 */
    const char        *id;            /* 16-char bootstrap client id */
    const char        *passkey;       /* 32-char bootstrap secretbox key */
    const char *const *env_keys;      /* InitialPayload env map keys (include "TERM") */
    const char *const *env_vals;      /* parallel values; env_count entries */
    size_t             env_count;
    uint16_t           cols, rows;    /* initial window, char cells */
    uint16_t           width, height; /* initial window, pixels; 0 if unknown */
    int                keepalive_secs;/* 0 -> ET default (5) */
} et_config;

/*
 * Open an ET connection to an already-bootstrapped endpoint.
 *
 * Non-blocking: spawns the transport thread and returns immediately. Returns
 * NULL only on synchronous argument failure (bad config: NULL host/id/passkey,
 * or env_count > 0 with a NULL env array/entry). Async handshake/connect
 * failure (e.g. wrong passkey) is reported via on_end on the transport
 * thread, not via this return value.
 *
 * cfg strings (host/id/passkey/env) are deep-copied; the caller may free them
 * on return.
 */
et_client *et_connect(const et_config *cfg, const et_callbacks *cbs, void *ctx);

/* Enqueue keystrokes/input bytes (-> TERMINAL_BUFFER). Returns the number of
 * bytes accepted, or a negative et_err on failure.
 *
 * Must not be called concurrently with et_close on the same handle, nor
 * after et_close has been called: the handle is dead once et_close starts
 * tearing it down, and racing et_send against et_close is undefined
 * behavior. The caller must externally serialize et_send/et_set_window_size
 * calls with et_close (e.g. call et_close only after all in-flight et_send
 * calls for that handle have returned). */
int et_send(et_client *c, const uint8_t *buf, size_t len);

/* Enqueue a window-size change (-> TERMINAL_INFO). width/height in pixels, 0
 * if unknown. Returns 0 on success, or a negative et_err on failure. Subject
 * to the same serialization-with-et_close requirement as et_send above. */
int et_set_window_size(et_client *c, uint16_t cols, uint16_t rows,
                       uint16_t width, uint16_t height);

/* Tear down the connection and free the handle: wakes the transport loop,
 * shuts down the ET connection, joins the thread, frees the handle.
 * Idempotent and safe to call more than once (including after on_end) —
 * only the first call tears down; later calls are a no-op.
 *
 * NOT safe to call concurrently with et_send/et_set_window_size on the same
 * handle: the caller must externally serialize so no other call on this
 * handle is in flight when et_close runs (idempotency covers repeated
 * *sequential* et_close calls, not a live race with other API calls). */
void et_close(et_client *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ETERNALTERMLIB_H */
