/*
 * fuzz_codec_scs.c - never-crash-a-peer fuzz for the SCS decoders (rd vms-5339).
 * The SCS header is decoded on EVERY inbound peer frame before anything else --
 * universal reachability. Body-level decoders (hdr / hdr_frame / dir_msg) are
 * fuzzed directly; the connection-control decoder (frame+fi) is reached via
 * classify. See fuzz_common.h for the bar.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_scs.h"

static int b_hdr(const uint8_t *b, uint32_t n)
{ struct vms_scs_hdr o; return (int)vms_scs_hdr_parse(b, n, &o); }
static int b_hdr_frame(const uint8_t *b, uint32_t n)
{ struct vms_scs_hdr o; return (int)vms_scs_hdr_parse_frame(b, n, &o); }
static int b_dir_msg(const uint8_t *b, uint32_t n)
{ struct vms_scs_dir_msg o; return (int)vms_scs_dir_msg_parse(b, n, &o); }
static void call_ctrl(const uint8_t *f, uint32_t n,
                      const struct vms_frame_info *fi, void *o)
{ (void)vms_scs_ctrl_parse(f, n, fi, (struct vms_scs_ctrl_frame *)o); }

int main(void)
{
    printf("fuzz_codec_scs: SCS never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0x5C5F);

    fz_load_fixtures();
    fz_run_body(b_hdr,       "scs_hdr_parse: 100k body fuzz, no over-read/crash");
    fz_run_body(b_hdr_frame, "scs_hdr_parse_frame: 100k body fuzz, no over-read/crash");
    fz_run_body(b_dir_msg,   "scs_dir_msg_parse: 100k body fuzz, no over-read/crash");

    /* connection-control (SHAPE A): reached via a real CONN_CTRL fixture. */
    struct vms_scs_ctrl_frame ctrl;
    int dc = fz_fixture_frame_fuzz("scs-connect-request-vaxcluster",
                                   VMS_FCLS_SCS_CONN_CTRL, call_ctrl, &ctrl);
    ct_check(dc > 0, "scs_ctrl_parse reached (fixture truncation+mutation), no over-read/crash");

    return ct_summary("fuzz_codec_scs");
}
