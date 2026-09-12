/*
 * fuzz_codec_dlm.c - never-crash-a-peer fuzz for the DLM (+ CM PARAMS) wire
 * decoders (rd vms-5339). An OVMX cluster node decodes these frames from
 * UNTRUSTED remote peers; a malformed / truncated / hostile frame that
 * over-reads or crashes a released node is exactly the disaster the ⭐⭐
 * never-crash-a-peer invariant forbids (R5-release-critical).
 *
 * Surface (vms_cluster_codec_dlm.c, owned by the cluster lane) — the 7 body
 * decoders SCS delivers untrusted bytes to, plus the CM PARAMS body decoder
 * (vms_cluster_codec_cm.c):
 *     vms_dlm_enq_request_parse_body      (ENQ / CONVERT)
 *     vms_dlm_enq_response_parse_body     (GRANT / DENY, +#1194 grant-valblk)
 *     vms_dlm_dir_hash_parse_body         (directory hash)
 *     vms_dlm_rebuild_parse_body          (join-time rebuild record)
 *     vms_dlm_deq_parse_body              ($DEQ release)
 *     vms_dlm_blkast_parse_body           (blocking AST)
 *     vms_dlm_valblk_convert_parse_body   (CONVERT carrying the value block)
 *     vms_cm_params_parse                 (CNXMAN op-01 PARAMS, votes)
 * plus the frame-level entry (vms_frame_classify -> vms_dlm_*_parse), the real
 * inbound path (body = frame + VMS_OFF_SYSAP_BODY, so a short frame exercises
 * the body-offset guard).
 *
 * THE BAR (as scoped WITH the codec owner — deliberately NOT clean-on-reject):
 * the decoder contract is RETURN-CODE-GATED (the caller must not read `out` on
 * a non-OK return, and the decoder may legitimately write some `out` fields
 * before a later check fails), so this fuzz does NOT assert `out` is
 * zeroed/clean on failure and does NOT assert return-code semantics on random
 * input (a structurally-valid random frame returning OK is not fabrication).
 * What it proves:
 *   (a) NO over-read / OOB / UB / crash on ANY truncation or hostile bytes;
 *   (b) NO fabrication-BY-over-read: the decoder never reads a field it was not
 *       given -- enforced structurally by giving each input an EXACT-sized heap
 *       allocation, so ASan's redzone sits immediately past the frame and even a
 *       one-byte over-read is a hard trap regardless of the input's validity;
 *   (c) bounded output: a write past a fixed-size `out` struct is an ASan/UBSan
 *       trap.
 * The CMake target builds this AND a twin copy of the codec sources WITH
 * -fsanitize=address,undefined (where the toolchain has it), so (a)-(c) are hard
 * failures, not silent passes. If it trips, the exact bytes + op go to the codec
 * owner -- this test never patches the codec.
 */
#include "cluster_test.h"
#include "vms_cluster_codec.h"        /* vms_frame_classify, VMS_FCLS_*, VMS_OFF_SYSAP_BODY */
#include "vms_cluster_codec_dlm.h"
#include "vms_cluster_codec_cm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* VMS_OFF_DLM_OP is an ABSOLUTE frame offset (81); the body starts at
 * VMS_OFF_SYSAP_BODY (72), so the opcode lives at body[9]. */
#define DLM_OP_BODY_OFF (VMS_OFF_DLM_OP - VMS_OFF_SYSAP_BODY)

/* Draw a random length biased around the real body/frame sizes and their
 * boundaries (0, tiny, ~132-byte body, ~190-byte frame, a little over). */
static uint32_t rand_len(uint32_t hi)
{
    int r = rand() % 8;
    switch (r) {
    case 0: return 0;
    case 1: return (uint32_t)(rand() % 12);          /* tiny / sub-header */
    case 2: return hi ? (uint32_t)(rand() % hi) : 0; /* anywhere up to hi */
    case 3: return hi > 4 ? hi - (uint32_t)(rand() % 5) : hi; /* just under hi */
    default: return (uint32_t)(rand() % (hi + 16));  /* around + a bit over */
    }
}

static void fill_random(uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        b[i] = (uint8_t)rand();
}

/* An EXACT-sized copy: the returned buffer is exactly n bytes (n>=1), so ASan
 * red-zones the byte at index n. Caller frees. For n==0 a 1-byte buffer is
 * returned (a zero-length decode must read nothing; the slack is harmless). */
static uint8_t *exact(const uint8_t *src, uint32_t n)
{
    uint8_t *p = (uint8_t *)malloc(n ? n : 1);
    if (p && n)
        memcpy(p, src, n);
    return p;
}

#define ITERS 100000

/* Fuzz one DLM body decoder. `op` is the opcode this decoder expects; for a
 * fraction of iterations we plant it at body[9] so the input clears the op gate
 * and drives the decoder DEEP into its field reads (where a late over-read
 * would live), instead of bouncing off the first check. */
typedef int (*body_fn)(const uint8_t *body, uint32_t len, void *ctx);

static void fuzz_body(body_fn fn, void *ctx, uint8_t op, const char *label)
{
    uint8_t scratch[224];
    for (int i = 0; i < ITERS; i++) {
        uint32_t n = rand_len(160);
        fill_random(scratch, n < sizeof scratch ? n : sizeof scratch);
        if (n > sizeof scratch) n = sizeof scratch;
        if ((rand() & 3) && n > DLM_OP_BODY_OFF)      /* 3/4: clear the op gate */
            scratch[DLM_OP_BODY_OFF] = op;
        uint8_t *body = exact(scratch, n);
        (void)fn(body, n, ctx);                       /* ASan is the assertion */
        free(body);
    }
    ct_check(1, label);
}

/* Per-decoder thunks (uniform (body,len,ctx) shape). */
static int t_enq_req(const uint8_t *b, uint32_t n, void *c)
{ uint8_t op; return (int)vms_dlm_enq_request_parse_body(b, n, &op, (struct vms_dlm_enq_request *)c); }
static int t_enq_resp(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_enq_response_parse_body(b, n, (struct vms_dlm_enq_response *)c); }
static int t_dir_hash(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_dir_hash_parse_body(b, n, (uint16_t *)c); }
static int t_rebuild(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_rebuild_parse_body(b, n, (struct vms_dlm_rebuild_record *)c); }
static int t_deq(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_deq_parse_body(b, n, (struct vms_dlm_deq *)c); }
static int t_blkast(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_blkast_parse_body(b, n, (struct vms_dlm_blkast *)c); }
static int t_valblk(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_valblk_convert_parse_body(b, n, (struct vms_dlm_valblk_convert *)c); }
static int t_cm_params(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_cm_params_parse(b, n, (struct vms_cm_params *)c); }

/* Frame-level path: classify a random full frame, then run every DLM frame
 * decoder on it. Exercises classify + the body-offset extraction (a short frame
 * hits the len < VMS_OFF_SYSAP_BODY guard) -- the actual inbound peer path. */
static void fuzz_frame(void)
{
    for (int i = 0; i < ITERS; i++) {
        uint32_t n = rand_len(200);
        uint8_t scratch[256];
        if (n > sizeof scratch) n = sizeof scratch;
        fill_random(scratch, n);
        uint8_t *f = exact(scratch, n);
        struct vms_frame_info fi;
        if (vms_frame_classify(f, n, &fi) == VMS_CODEC_OK) {
            struct vms_dlm_enq_request req; uint8_t op;
            struct vms_dlm_enq_response resp;
            struct vms_dlm_rebuild_record rec;
            struct vms_dlm_deq d;
            struct vms_dlm_blkast bk;
            struct vms_dlm_valblk_convert cv;
            uint16_t h;
            (void)vms_dlm_enq_request_parse(f, n, &fi, &op, &req);
            (void)vms_dlm_enq_response_parse(f, n, &fi, &resp);
            (void)vms_dlm_dir_hash_parse(f, n, &fi, &h);
            (void)vms_dlm_rebuild_parse(f, n, &fi, &rec);
            (void)vms_dlm_deq_parse(f, n, &fi, &d);
            (void)vms_dlm_blkast_parse(f, n, &fi, &bk);
            (void)vms_dlm_valblk_convert_parse(f, n, &fi, &cv);
        }
        free(f);
    }
    ct_check(1, "frame-level classify->parse: 100k random frames, no over-read/crash");
}

/* Targeted seeds for #1194's (vms-727) grant-with-valblk branch in
 * vms_dlm_enq_response_parse_body: the marker is body[28]==0x10 AND the LE32 at
 * body[32:36]=={01 00 fa 00}, after which it reads a 16-byte value block at
 * body[36:52]. A hostile peer can set the marker and then TRUNCATE before
 * body[52] (or before body[36], or below the marker offsets). Exact-sized
 * buffers make any over-read of the value block a hard ASan trap; the codec's
 * vms_wire_view_ok guard must hold on every truncation. */
static void fuzz_grant_valblk(void)
{
    struct vms_dlm_enq_response resp;
    for (int i = 0; i < ITERS; i++) {
        uint8_t scratch[64];
        /* 0..55 spans: below the marker (n<=28), below the value block
         * (28<n<36), a TRUNCATED value block (36<=n<52), and the full 52. */
        uint32_t n = (uint32_t)(rand() % 56);
        fill_random(scratch, n);
        if (n > DLM_OP_BODY_OFF) scratch[DLM_OP_BODY_OFF] = VMS_DLM_WIREOP_ENQ;
        if (n > 28) scratch[28] = 0x10;                        /* marker byte  */
        if (n > 35) { scratch[32] = 0x01; scratch[33] = 0x00;  /* marker LE32  */
                      scratch[34] = 0xfa; scratch[35] = 0x00; }
        uint8_t *body = exact(scratch, n);
        (void)vms_dlm_enq_response_parse_body(body, n, &resp);  /* ASan is the check */
        free(body);
    }
    ct_check(1, "enq_response grant-valblk marker + truncated value block "
                "(body[36:52], #1194/vms-727): 100k, no over-read/crash");
}

int main(void)
{
    struct vms_dlm_enq_request req;
    struct vms_dlm_enq_response resp;
    struct vms_dlm_rebuild_record rec;
    struct vms_dlm_deq deq;
    struct vms_dlm_blkast blk;
    struct vms_dlm_valblk_convert cv;
    struct vms_cm_params cmp;
    uint16_t hash;

    printf("fuzz_codec_dlm: DLM/CM wire-decode never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0x0D1B);

    fuzz_body(t_enq_req,   &req,  VMS_DLM_WIREOP_ENQ,           "enq_request_parse_body: 100k, no over-read/crash");
    fuzz_body(t_enq_resp,  &resp, VMS_DLM_WIREOP_ENQ,           "enq_response_parse_body: 100k, no over-read/crash");
    fuzz_body(t_dir_hash,  &hash, VMS_DLM_WIREOP_REBUILD,       "dir_hash_parse_body: 100k, no over-read/crash");
    fuzz_body(t_rebuild,   &rec,  VMS_DLM_WIREOP_REBUILD,       "rebuild_parse_body: 100k, no over-read/crash");
    fuzz_body(t_deq,       &deq,  VMS_DLM_WIREOP_DEQ,           "deq_parse_body: 100k, no over-read/crash");
    fuzz_body(t_blkast,    &blk,  VMS_DLM_WIREOP_BLKAST,        "blkast_parse_body: 100k, no over-read/crash");
    fuzz_body(t_valblk,    &cv,   VMS_DLM_WIREOP_CONVERT_VALBLK,"valblk_convert_parse_body: 100k, no over-read/crash");
    fuzz_body(t_cm_params, &cmp,  0,                            "cm_params_parse: 100k, no over-read/crash");

    fuzz_grant_valblk();
    fuzz_frame();

    return ct_summary("fuzz_codec_dlm");
}
