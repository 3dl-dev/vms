/*
 * fuzz_codec_cm.c - never-crash-a-peer fuzz for the CNXMAN (CM) body decoders
 * (rd vms-5339). CNXMAN admission/membership frames are hostile-reachable during
 * join. These are all body-level (body,len,out) decoders; the PARAMS decoder is
 * already covered by #1200. See fuzz_common.h for the bar.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_cm.h"

static int b_envelope(const uint8_t *b, uint32_t n)
{ struct vms_cm_envelope o; return (int)vms_cm_envelope_parse(b, n, &o); }
static int b_open(const uint8_t *b, uint32_t n)
{ struct vms_cm_open o; return (int)vms_cm_open_parse(b, n, &o); }
static int b_barrier(const uint8_t *b, uint32_t n)
{ struct vms_cm_barrier o; return (int)vms_cm_barrier_parse(b, n, &o); }
static int b_model(const uint8_t *b, uint32_t n)
{ struct vms_cm_model o; return (int)vms_cm_model_parse(b, n, &o); }
static int b_dlm_rebuild(const uint8_t *b, uint32_t n)
{ struct vms_cm_dlm_rebuild o; return (int)vms_cm_dlm_rebuild_parse(b, n, &o); }
static int b_membership(const uint8_t *b, uint32_t n)
{ struct vms_cm_membership_rec o; return (int)vms_cm_membership_rec_parse(b, n, &o); }

int main(void)
{
    printf("fuzz_codec_cm: CNXMAN body-decoder never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0xC33A);

    fz_run_body(b_envelope,    "cm_envelope_parse: 100k body fuzz, no over-read/crash");
    fz_run_body(b_open,        "cm_open_parse: 100k body fuzz, no over-read/crash");
    fz_run_body(b_barrier,     "cm_barrier_parse: 100k body fuzz, no over-read/crash");
    fz_run_body(b_model,       "cm_model_parse: 100k body fuzz, no over-read/crash");
    fz_run_body(b_dlm_rebuild, "cm_dlm_rebuild_parse: 100k body fuzz, no over-read/crash");
    fz_run_body(b_membership,  "cm_membership_rec_parse: 100k body fuzz, no over-read/crash");

    return ct_summary("fuzz_codec_cm");
}
