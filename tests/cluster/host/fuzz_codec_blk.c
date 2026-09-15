/*
 * fuzz_codec_blk.c - never-crash-a-peer fuzz for the SCA block-transfer decoders
 * (rd vms-5339): blk_hdr / blk_hdr_at / blk_trailer (body-level, with a fuzzed
 * offset / inner-length) and blk_frame (frame+fi, via classify). Block-transfer
 * framing carries no length field of its own, so the decoders take the frame's
 * received length -- a hostile short frame must not read past it. See
 * fuzz_common.h for the bar.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_blk.h"

static void call_blk_frame(const uint8_t *f, uint32_t n,
                           const struct vms_frame_info *fi, void *o)
{ (void)vms_blk_frame_parse(f, n, fi, (struct vms_blk_view *)o); }

int main(void)
{
    printf("fuzz_codec_blk: SCA block-transfer never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0xB1C6);
    fz_load_fixtures();

    /* blk_hdr / blk_hdr_at: the 28-byte header at offset 0 and at a FUZZED
     * offset `at` (the decoder must bound `at` against len). */
    struct vms_blk_hdr hdr;
    for (int i = 0; i < FZ_ITERS; i++) {
        uint8_t scratch[256];
        uint32_t n = fz_len(64);
        if (n > sizeof scratch) n = sizeof scratch;
        fz_fill(scratch, n);
        uint8_t *b = fz_exact(scratch, n);
        (void)vms_blk_hdr_parse(b, n, &hdr);
        (void)vms_blk_hdr_parse_at(b, n, (uint32_t)(rand() % (n + 40)), &hdr);
        free(b);
    }
    ct_check(1, "blk_hdr_parse[_at]: 100k body fuzz (fuzzed offset), no over-read/crash");

    /* blk_trailer: takes both the received frame_len AND a separate
     * inner_frame_len -- fuzz both independently against the exact buffer. */
    struct vms_blk_view view;
    for (int i = 0; i < FZ_ITERS; i++) {
        uint8_t scratch[256];
        uint32_t n = fz_len(80);
        if (n > sizeof scratch) n = sizeof scratch;
        fz_fill(scratch, n);
        uint8_t *b = fz_exact(scratch, n);
        (void)vms_blk_trailer_parse(b, n, (uint32_t)(rand() % (n + 40)), &view);
        free(b);
    }
    ct_check(1, "blk_trailer_parse: 100k body fuzz (fuzzed inner_frame_len), no over-read/crash");

    /* blk_frame: frame+fi. Block transfer rides the 190-content SCS_MSG frame,
     * so it is reached from a real SCS_MSG fixture (truncation + mutation). */
    int df = fz_fixture_frame_fuzz("scs-msg190-vaxcluster-cat01-op14",
                                   VMS_FCLS_SCS_MSG, call_blk_frame, &view);
    ct_check(df > 0, "blk_frame_parse reached (SCS_MSG fixture truncation+mutation), no over-read/crash");

    return ct_summary("fuzz_codec_blk");
}
