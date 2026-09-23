/*
 * fuzz_codec_hello.c - never-crash-a-peer fuzz for the HELLO/SOLICIT decoders
 * (rd vms-5339). HELLO and SOLICIT are UNAUTHENTICATED multicast processed
 * pre-membership, pre-VC, by ANY node on the datalink -- the most hostile-
 * reachable peer-receive surface on a released node. Fixture-seeded (truncation
 * + mutation of a real valid frame) so the parser is genuinely reached; the
 * non-vacuity assertion makes a never-reached parser a hard failure. See
 * fuzz_common.h for the exact-sized-buffer + ASan/UBSan bar.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_hello.h"

static void call_hello(const uint8_t *f, uint32_t n,
                       const struct vms_frame_info *fi, void *o)
{ (void)vms_hello_parse(f, n, fi, (struct vms_hello_frame *)o); }
static void call_solicit(const uint8_t *f, uint32_t n,
                         const struct vms_frame_info *fi, void *o)
{ (void)vms_solicit_parse(f, n, fi, (struct vms_solicit_frame *)o); }

int main(void)
{
    struct vms_hello_frame hello;
    struct vms_solicit_frame sol;
    int dh, ds;

    printf("fuzz_codec_hello: HELLO/SOLICIT never-crash-a-peer fuzz (rd vms-5339)\n");
    srand(0x4E11);
    fz_load_fixtures();

    dh = fz_fixture_frame_fuzz("hello-multicast-vax1", VMS_FCLS_HELLO,
                               call_hello, &hello);
    ct_check(dh > 0, "hello_parse reached (fixture truncation+mutation), no over-read/crash");

    ds = fz_fixture_frame_fuzz("solicit-vax3-satellite-boot", VMS_FCLS_SOLICIT,
                               call_solicit, &sol);
    ct_check(ds > 0, "solicit_parse reached (fixture truncation+mutation), no over-read/crash");

    /* rd vms-0f8: the SECOND discovery revision is the same unauthenticated
     * pre-membership surface, reached by the same anonymous multicast -- and
     * its frame is SIX BYTES SHORTER than the one the decoder was written
     * against, which is exactly the shape an over-read hides in. Seeded from
     * the real V5.5-2H4 specimen. */
    dh = fz_fixture_frame_fuzz("hello-c3-vaxc-v55-multicast", VMS_FCLS_HELLO_C3,
                               call_hello, &hello);
    ct_check(dh > 0, "hello_parse reached on the C03 revision (fixture "
                     "truncation+mutation), no over-read/crash");

    return ct_summary("fuzz_codec_hello");
}
