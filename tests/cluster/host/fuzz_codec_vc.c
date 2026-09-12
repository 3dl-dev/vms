/*
 * fuzz_codec_vc.c - never-crash-a-peer fuzz for the VC-formation decoders
 * (rd vms-5339): SCS START and CREDIT. Both are frame+fi decoders, fixture-
 * seeded (truncation + mutation of a real valid frame) with a non-vacuity
 * assertion. See fuzz_common.h for the bar.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_vc.h"

static void call_start(const uint8_t *f, uint32_t n,
                       const struct vms_frame_info *fi, void *o)
{ (void)vms_scs_start_parse(f, n, fi, (struct vms_scs_start_frame *)o); }
static void call_credit(const uint8_t *f, uint32_t n,
                        const struct vms_frame_info *fi, void *o)
{ (void)vms_scs_credit_parse(f, n, fi, (struct vms_scs_credit_frame *)o); }

int main(void)
{
    struct vms_scs_start_frame start;
    struct vms_scs_credit_frame credit;
    int dsr, dcr;

    printf("fuzz_codec_vc: VC START/CREDIT never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0x1C3C);
    fz_load_fixtures();

    dsr = fz_fixture_frame_fuzz("scs-start-vax2-config-round0", VMS_FCLS_SCS_START,
                                call_start, &start);
    ct_check(dsr > 0, "scs_start_parse reached (fixture truncation+mutation), no over-read/crash");

    dcr = fz_fixture_frame_fuzz("scs-credit-return-short", VMS_FCLS_SCS_CREDIT,
                                call_credit, &credit);
    ct_check(dcr > 0, "scs_credit_parse reached (fixture truncation+mutation), no over-read/crash");

    return ct_summary("fuzz_codec_vc");
}
