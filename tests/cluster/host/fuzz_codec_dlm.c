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
 *     vms_dlm_grant_notify_parse_body     (the DEFERRED GRANT, cat-0x02 op-0x02)
 *     vms_dlm_dir_answer_parse_body       (a DIRECTORY's answer to a lookup)
 *     vms_dlm_res_ident_parse_body        (the resource identity a request names)
 *     vms_dlm_dir_reg_parse_body          (the op-0x0d record, rd vms-20c)
 *     vms_dlm_rm_step_parse_body          (a remaster step, 0x0e/0x0f/0x10/0x13/0x14)
 *     vms_dlm_rm_lock_rec_parse_body      (a remaster LOCK RECORD, op-0x12)
 *     vms_dlm_rm_valblk_parse_body        (a remaster's VALUE BLOCK, op-0x1a)
 *     vms_dlm_rm_lock_grant_parse_body    (the NEW master's 82/01 to OUR op-0x12
 *                                          record, rd vms-33a)
 *     vms_dlm_rm_0f_new_master            (the new master an op-0x0f names,
 *                                          rd vms-33a)
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
{ return (int)vms_dlm_dir_hash_parse_body(b, n, (uint32_t *)c); }
static int t_rebuild(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_rebuild_parse_body(b, n, (struct vms_dlm_rebuild_record *)c); }
static int t_deq(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_deq_parse_body(b, n, (struct vms_dlm_deq *)c); }
static int t_blkast(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_blkast_parse_body(b, n, (struct vms_dlm_blkast *)c); }
static int t_valblk(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_valblk_convert_parse_body(b, n, (struct vms_dlm_valblk_convert *)c); }
/* rd vms-a4d7: the DEFERRED GRANT a master sends when a lock it answered 0xfb
 * QUEUED becomes grantable. Fuzzed WITH the decoder, not after it: a
 * peer-receive decoder that reaches a released node unfuzzed is the gap the
 * never-crash rule exists to close. */
static int t_grant_notify(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_grant_notify_parse_body(b, n, (struct vms_dlm_grant_notify *)c); }
static int t_dir_answer(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_dir_answer_parse_body(b, n, (struct vms_dlm_dir_answer *)c); }
static int t_dir_reg(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_dir_reg_parse_body(b, n, (struct vms_dlm_dir_reg *)c); }
static int t_res_ident(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_res_ident_parse_body(b, n, (struct vms_dlm_res_ident *)c); }
/* rd vms-380: the REMASTER decoders. A departing VAX's 0x0e/0x0f/0x10/0x12/0x13
 * and a new master's 0x14 are bytes a real peer chooses, and rd vms-6ad1 (RA7)
 * is about to put this node in the role that receives all of them. */
static int t_rm_step(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_rm_step_parse_body(b, n, (struct vms_dlm_rm_step *)c); }
static int t_rm_lock_rec(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_rm_lock_rec_parse_body(b, n, (struct vms_dlm_rm_lock_rec *)c); }
/* rd vms-26e: the op-0x1a, the old master's value block for one lock of ours,
 * which this node now parses whenever a real VAX remasters a tree onto it. */
static int t_rm_valblk(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_rm_valblk_parse_body(b, n, (struct vms_dlm_rm_valblk *)c); }
static int t_cm_params(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_cm_params_parse(b, n, (struct vms_cm_params *)c); }
static int t_dlksrch(const uint8_t *b, uint32_t n, void *c)
{ return (int)vms_dlm_dlksrch_parse_body(b, n, (struct vms_dlm_dlksrch_record *)c); }

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
            uint32_t h;
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
        /* 0..59 spans: below the marker (n<=28), below the value block
         * (28<n<36), a TRUNCATED value block (36<=n<52), a TRUNCATED
         * sequence (52<=n<56, rd vms-26e) and the full 56. */
        uint32_t n = (uint32_t)(rand() % 60);
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

/* rd vms-33a: the two decoders the HOLDER's half reads off a real peer -- the
 * new master's 82/01 answer to one of our op-0x12 records, and the new master
 * an op-0x0f names. Both class gates are planted (cat 0x82 / op 0x01 and cat
 * 0x02 / op 0x0f at body[8:10]) on 3/4 of the iterations, over every length
 * from 0 to 160, so the field reads behind the gates are reached; the count of
 * accepted bodies is asserted non-zero for each, so a gate change that stops
 * reaching them fails here rather than shipping a green fuzz of nothing. */
static void fuzz_rm_holder(void)
{
    struct vms_dlm_rm_lock_grant g;
    uint32_t csid, ok_grant = 0, ok_0f = 0;

    for (int i = 0; i < ITERS; i++) {
        uint8_t scratch[160];
        uint32_t n = (uint32_t)(rand() % 161);
        int which = i & 1;

        fill_random(scratch, n);
        if ((rand() & 3) && n > DLM_OP_BODY_OFF) {
            scratch[DLM_OP_BODY_OFF - 1] = which ? 0x82 : 0x02;
            scratch[DLM_OP_BODY_OFF] = which ? VMS_DLM_WIREOP_ENQ
                                             : VMS_DLM_WIREOP_RM_0F;
        }
        uint8_t *body = exact(scratch, n);
        if (which) {
            if (vms_dlm_rm_lock_grant_parse_body(body, n, &g) == VMS_CODEC_OK)
                ok_grant++;
        } else {
            if (vms_dlm_rm_0f_new_master(body, n, &csid) == VMS_CODEC_OK)
                ok_0f++;
        }
        free(body);
    }
    ct_check(ok_grant > 0, "rm_lock_grant_parse_body: 100k (half planted), "
                           "no over-read/crash, and REACHED (non-vacuous)");
    ct_check(ok_0f > 0, "rm_0f_new_master: 100k (half planted), no "
                        "over-read/crash, and REACHED (non-vacuous)");
}

/* rd vms-20c: the op-0x0d RECORD parser, with its class gates planted so the
 * identity reader behind them is actually reached: cat 0x02 / op 0x0d at
 * body[8:10] and 0x0001 at body[12:14], a name length that is usually in range,
 * and every truncation from 0 to 160 bytes. A real VAX chooses every one of
 * these bytes. The count of accepted bodies is asserted non-zero, so a gate
 * change that silently stops reaching the reader fails here instead of
 * shipping a green fuzz of nothing. */
static void fuzz_dir_reg(void)
{
    struct vms_dlm_dir_reg r;
    uint32_t accepted = 0, outside = 0;
    for (int i = 0; i < ITERS; i++) {
        uint8_t scratch[160];
        uint32_t n = (uint32_t)(rand() % 161);
        fill_random(scratch, n);
        if (n > 9)  { scratch[8] = 0x02; scratch[9] = VMS_DLM_WIREOP_REBUILD; }
        if (n > 13 && (rand() & 7)) { scratch[12] = 0x01; scratch[13] = 0x00; }
        if (n > 47 && (rand() & 3)) scratch[47] = (uint8_t)(rand() % 34);
        uint8_t *body = exact(scratch, n);
        if (vms_dlm_dir_reg_parse_body(body, n, &r) == VMS_CODEC_OK) {
            accepted++;
            if (r.id.name_len < 1 || r.id.name_len > 31 || 48u + r.id.name_len > n)
                outside++;
        }
        free(body);
    }
    printf("  (dir_reg_parse_body accepted %u of %d planted bodies)\n", accepted, ITERS);
    ct_check(accepted > 0, "dir_reg_parse_body: 100k planted+truncated, reached, no over-read/crash");
    ct_check(outside == 0, "dir_reg_parse_body: every accepted name lies inside its body");
}

int main(void)
{
    struct vms_dlm_enq_request req;
    struct vms_dlm_enq_response resp;
    struct vms_dlm_rebuild_record rec;
    struct vms_dlm_deq deq;
    struct vms_dlm_blkast blk;
    struct vms_dlm_valblk_convert cv;
    struct vms_dlm_grant_notify gn;
    struct vms_dlm_dir_answer da;
    struct vms_dlm_res_ident ri;
    struct vms_dlm_dir_reg dreg;
    struct vms_dlm_rm_step rms;
    struct vms_dlm_rm_lock_rec rmlr;
    struct vms_dlm_rm_valblk rmvb;
    struct vms_cm_params cmp;
    struct vms_dlm_dlksrch_record dlk;
    uint32_t hash;

    printf("fuzz_codec_dlm: DLM/CM wire-decode never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0x0D1B);

    fuzz_body(t_enq_req,   &req,  VMS_DLM_WIREOP_ENQ,           "enq_request_parse_body: 100k, no over-read/crash");
    fuzz_body(t_enq_resp,  &resp, VMS_DLM_WIREOP_ENQ,           "enq_response_parse_body: 100k, no over-read/crash");
    fuzz_body(t_dir_hash,  &hash, VMS_DLM_WIREOP_REBUILD,       "dir_hash_parse_body: 100k, no over-read/crash");
    fuzz_body(t_rebuild,   &rec,  VMS_DLM_WIREOP_REBUILD,       "rebuild_parse_body: 100k, no over-read/crash");
    fuzz_body(t_deq,       &deq,  VMS_DLM_WIREOP_DEQ,           "deq_parse_body: 100k, no over-read/crash");
    fuzz_body(t_blkast,    &blk,  VMS_DLM_WIREOP_BLKAST,        "blkast_parse_body: 100k, no over-read/crash");
    fuzz_body(t_valblk,    &cv,   VMS_DLM_WIREOP_CONVERT_VALBLK,"valblk_convert_parse_body: 100k, no over-read/crash");
    fuzz_body(t_grant_notify, &gn, VMS_DLM_WIREOP_GRANT_NOTIFY, "grant_notify_parse_body: 100k, no over-read/crash");
    fuzz_body(t_dir_answer, &da,  VMS_DLM_WIREOP_ENQ,           "dir_answer_parse_body: 100k, no over-read/crash");
    fuzz_body(t_res_ident, &ri,   VMS_DLM_WIREOP_ENQ,           "res_ident_parse_body: 100k, no over-read/crash");
    fuzz_body(t_rm_step,   &rms,  VMS_DLM_WIREOP_RM_10,         "rm_step_parse_body: 100k, no over-read/crash");
    fuzz_body(t_rm_lock_rec, &rmlr, VMS_DLM_WIREOP_RM_12,       "rm_lock_rec_parse_body: 100k, no over-read/crash");
    fuzz_body(t_rm_valblk, &rmvb, VMS_DLM_WIREOP_RM_1A,         "rm_valblk_parse_body: 100k, no over-read/crash");
    fuzz_body(t_cm_params, &cmp,  0,                            "cm_params_parse: 100k, no over-read/crash");
    fuzz_body(t_dlksrch,   &dlk,  VMS_DLM_WIREOP_DLKSRCH,       "dlksrch_parse_body: 100k, no over-read/crash");

    fuzz_body(t_dir_reg,   &dreg, VMS_DLM_WIREOP_REBUILD,       "dir_reg_parse_body: 100k, no over-read/crash");

    fuzz_grant_valblk();
    fuzz_dir_reg();
    fuzz_rm_holder();
    fuzz_frame();

    return ct_summary("fuzz_codec_dlm");
}
