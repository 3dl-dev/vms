/*
 * fuzz_common.h - shared helpers for the cluster peer-receive never-crash-a-peer
 * fuzzers (rd vms-5339), the exact-sized-buffer + ASan/UBSan bar #1200 set for
 * the DLM decoders, extended per-family across the rest of the untrusted
 * peer-receive decode surface (hello / scs / cm / vc / blk).
 *
 * THE BAR (scoped with the codec owner; the codec contract is RETURN-CODE-GATED,
 * NOT clean-on-reject): assert (a) no over-read / OOB / UB / crash on any
 * truncation or hostile bytes, (b) no fabrication-by-over-read, (c) bounded
 * output -- and NOTHING about the return code or `out` contents on garbage (a
 * structurally-valid random frame returning OK is not a bug; a partially-written
 * `out` before a rejected non-OK return is contract-correct). (a)-(c) are
 * enforced structurally: every fuzzed input lives in an EXACT-sized heap
 * allocation, so ASan's redzone sits immediately past the frame and even a
 * one-byte over-read is a hard trap. The fuzz never patches the codec; a trip
 * hands the exact bytes + op to the owner.
 *
 * Two seams:
 *   - fz_run_body(): body-level (body,len,out) decoders, fuzzed directly with
 *     random exact-sized inputs (always reached).
 *   - fz_fixture_frame_fuzz(): frame+fi decoders, reached from a REAL valid
 *     frame of their class (a fixture) by truncation walk + byte mutation, since
 *     random frames almost never satisfy a class's classify gate. Returns the
 *     dispatch COUNT so the caller can assert the parser was genuinely reached
 *     (a never-reached parser would be a facade -- INV-6 -- caught as a failure).
 */
#ifndef FUZZ_COMMON_H
#define FUZZ_COMMON_H

#include "cluster_test.h"
#include "cluster_fixture.h"
#include "vms_cluster_codec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FZ_ITERS
#define FZ_ITERS 100000
#endif

/* EXACT-sized copy: exactly n bytes (n>=1), so ASan red-zones index n. n==0 -> a
 * 1-byte buffer (a zero-length decode must read nothing). Caller frees. */
static inline uint8_t *fz_exact(const uint8_t *src, uint32_t n)
{
    uint8_t *p = (uint8_t *)malloc(n ? n : 1);
    if (p && n)
        memcpy(p, src, n);
    return p;
}

/* A length biased around a target size and its boundaries. */
static inline uint32_t fz_len(uint32_t hi)
{
    switch (rand() % 8) {
    case 0: return 0;
    case 1: return (uint32_t)(rand() % 12);
    case 2: return hi ? (uint32_t)(rand() % hi) : 0;
    case 3: return hi > 4 ? hi - (uint32_t)(rand() % 5) : hi;
    default: return (uint32_t)(rand() % (hi + 16));
    }
}

static inline void fz_fill(uint8_t *b, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        b[i] = (uint8_t)rand();
}

/* Body-level (SHAPE B) decoder fuzz: FZ_ITERS random exact-sized inputs. */
typedef int (*fz_body_fn)(const uint8_t *body, uint32_t len);
static inline void fz_run_body(fz_body_fn fn, const char *label)
{
    uint8_t scratch[256];
    int i;
    for (i = 0; i < FZ_ITERS; i++) {
        uint32_t n = fz_len(200);
        if (n > sizeof scratch) n = sizeof scratch;
        fz_fill(scratch, n);
        uint8_t *b = fz_exact(scratch, n);
        (void)fn(b, n);
        free(b);
    }
    ct_check(1, label);
}

/* ---- fixture-seeded SHAPE A fuzz (guaranteed reach + non-vacuity) -------- */
#define FZ_MAX_FX 96
static struct vms_fixture g_fz_fx[FZ_MAX_FX];
static int g_fz_n = 0;

static inline void fz_load_fixtures(void)
{
    char err[256];
    g_fz_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
                                  g_fz_fx, FZ_MAX_FX, err, sizeof err);
    if (g_fz_n < 0) {
        printf("  FAIL: fixture corpus load: %s\n", err);
        g_fz_n = 0;
    }
}

static inline const struct vms_fixture *fz_fixture(const char *name)
{
    int i;
    for (i = 0; i < g_fz_n; i++)
        if (strcmp(g_fz_fx[i].name, name) == 0)
            return &g_fz_fx[i];
    return (const struct vms_fixture *)0;
}

/* Dispatch thunk: call the family's frame+fi parser on a classified frame. */
typedef void (*fz_frame_fn)(const uint8_t *frame, uint32_t len,
                            const struct vms_frame_info *fi, void *out);

/* Truncation walk + byte-mutation of `fxname`, dispatching to `call` on
 * fi.cls==want_cls. Returns the dispatch count (<0 if the fixture is missing). */
static inline int fz_fixture_frame_fuzz(const char *fxname, uint8_t want_cls,
                                        fz_frame_fn call, void *out)
{
    const struct vms_fixture *fx = fz_fixture(fxname);
    struct vms_frame_info fi;
    int dispatched = 0;
    uint32_t p;
    int i;

    if (fx == (const struct vms_fixture *)0)
        return -1;

    for (p = 0; p <= fx->wire_len; p++) {           /* truncation walk */
        uint8_t *b = fz_exact(fx->bytes, p);
        if (vms_frame_classify(b, p, &fi) == VMS_CODEC_OK && fi.cls == want_cls) {
            call(b, p, &fi, out);
            dispatched++;
        }
        free(b);
    }
    for (i = 0; i < FZ_ITERS; i++) {                /* byte mutation */
        uint32_t n = fx->wire_len;
        uint8_t *b = fz_exact(fx->bytes, n);
        int flips = 1 + rand() % 4;
        int k;
        for (k = 0; k < flips && n; k++)
            b[rand() % n] ^= (uint8_t)(1 + rand() % 255);
        if (vms_frame_classify(b, n, &fi) == VMS_CODEC_OK && fi.cls == want_cls) {
            call(b, n, &fi, out);
            dispatched++;
        }
        free(b);
    }
    return dispatched;
}

#endif /* FUZZ_COMMON_H */
