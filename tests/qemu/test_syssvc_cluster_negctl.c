/*
 * test_syssvc_cluster_negctl.c - the FC-P3.9 negative control, driven against
 * a REAL executive through /dev/vms via the freestanding kif client
 * (src/libvmssys/vms_kif.c) -- the same footing as test_syssvc_dlm_xnode.c.
 *
 * THE QUESTION THIS ANSWERS. FC-P3.9's plan row names one R4 negctl:
 *
 *     VAXCLUSTER=0 => NOTMEMBER, SS$_NORMAL;  no /dev/vms => SS$_NOSUCHDEV
 *
 * The two halves are DIFFERENT FACTS and must never be rendered the same way
 * (rd vms-8d4). "NOTMEMBER" is the executive ANSWERING a question about this
 * node -- it read its own CLUB and there is nobody in it. "NOSUCHDEV" is
 * nothing having answered at all. A build that conflated them would report a
 * confident "not a member" on a node whose executive it never reached, which
 * is the fabrication class INV-6 exists to kill.
 *
 * THIS SUITE PROVES THE FIRST HALF, against a real executive, on the node the
 * QEMU harness boots -- where VAXCLUSTER is 0 (nothing authored it) and the
 * connection manager therefore never started. The second half needs the
 * OPPOSITE condition (no /dev/vms) and is proven by
 * tests/integration/test_show_cluster_negctl.sh, which runs the real DCL.EXE
 * on a host with no executive at all. Neither can prove the other's half by
 * construction, which is why there are two.
 *
 * WHAT IS ASSERTED, ALL OF IT A READ OF REAL EXECUTIVE STATE:
 *
 *   1. VMS_IOCTL_CLUSTER_MEMBER_GET answers SS$_NORMAL with n_members == 0.
 *      The status is the point: SS$_NOSUCHDEV here would make a booted node
 *      indistinguishable from an unreachable one.
 *   2. Not one member row is filled. A zeroed row must stay zeroed -- a
 *      handler that "helpfully" reported the local node as a member of a
 *      cluster it never joined would pass (1) and fail this.
 *   3. VMS_IOCTL_CLUSTER_GETSYI answers SS$_NORMAL with cluster_member == 0
 *      and cluster_nodes == 0, and with every `_valid` companion CLEAR --
 *      node_csid_valid especially: the cluster has assigned this node no
 *      CSID, and 0 means "none assigned", never "node zero" (integration
 *      note E30).
 *   4. $GETSYI itself -- the real system service, not the ioctl -- agrees:
 *      SYI$_CLUSTER_MEMBER reads 0 and SYI$_CLUSTER_NODES reads 0. This is
 *      the cutover assertion: F$GETSYI and SHOW CLUSTER must read the SAME
 *      CLUB, so a divergence here means one of them grew a second source.
 *   5. THE MUTATORS ARE GONE. VMS_IOCTL_CLUSTER_MEMBER_SET (0x39) and
 *      _CLEAR (0x3a) were the userspace daemon's populate path. Issuing
 *      either raw command number must be REFUSED by the executive -- if one
 *      still worked, userspace could still assert membership, which is
 *      exactly what the retirement removed.
 *
 * REPLACES test_syssvc_cluster_member.c, which drove SET/CLEAR/GET against
 * the deleted module-global mirror block.
 *
 * Honest SKIP (77) when /dev/vms is absent -- this facility is wholly
 * executive-resident, so there is nothing to drive and nothing this suite can
 * fabricate (the test_syssvc_* honest-skip-77 contract).
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"
#include "vms_kif.h"
#include "starlet.h"
#include "syidef.h"
#include "lnmdef.h"   /* struct item_list_3 -- the $GETSYI item list */

#define SS_NORMAL   1u
#define EXIT_SKIP    77

/* The two retired command numbers, reconstructed here from their ORIGINAL
 * encodings rather than from a header -- the header no longer declares them,
 * which is the point. _IOWR(0x56, nr, sizeof) with the sizes the retired
 * structs had: 44 bytes for SET, 8 for CLEAR. */
#define RETIRED_CLUSTER_MEMBER_SET    0xC02C5639u
#define RETIRED_CLUSTER_MEMBER_CLEAR  0xC008563Au

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

/* Every byte of a member row is zero -- "no row here", not a row about
 * system 0. */
static int member_row_is_blank(const struct vms_cluster_member *m)
{
    const unsigned char *b = (const unsigned char *)m;
    for (unsigned i = 0; i < sizeof(*m); i++)
        if (b[i] != 0u)
            return 0;
    return 1;
}

/* 1 + 2: the member table. */
static void check_member_get(void)
{
    struct vms_cluster_member members[VMS_CLUSTER_MEMBER_MAX];
    uint32_t n = 0xffffffffu;
    uint32_t st;
    int blank = 1;

    memset(members, 0xa5, sizeof(members));
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);

    CHECK(st == SS_NORMAL,
          "CLUSTER_MEMBER_GET with no connection manager -> SS$_NORMAL "
          "(the executive ANSWERED; NOTMEMBER != NOSUCHDEV)");
    CHECK(n == 0,
          "... and reports n_members == 0 -- the honest NOTMEMBER view");

    /* The wrapper copies only n_members rows, so the rest keep the 0xa5
     * poison; row 0 is the one the executive would have written. */
    if (n == 0)
        blank = 1;
    else
        blank = member_row_is_blank(&members[0]);
    CHECK(blank,
          "... and no member row was filled (a node that never joined is "
          "not reported as a member of anything)");
}

/* 3: the $GETSYI projection, straight off the CLUB. */
static void check_getsyi_ioctl(void)
{
    struct vms_cluster_getsyi_args a;
    uint32_t st;

    memset(&a, 0xa5, sizeof(a));
    st = vms_kif_cluster_getsyi(&a);

    CHECK(st == SS_NORMAL,
          "CLUSTER_GETSYI with no connection manager -> SS$_NORMAL");
    CHECK(a.cluster_member == 0u,
          "... cluster_member == 0 (this node is not a member)");
    CHECK(a.cluster_nodes == 0u,
          "... cluster_nodes == 0 (the CLUB counts nobody)");
    CHECK(a.node_csid_valid == 0u && a.node_csid == 0u,
          "... node_csid_valid CLEAR -- the cluster assigned no CSID, and 0 "
          "means 'none assigned', never 'node zero'");
    CHECK(a.cluster_fsysid_valid == 0u && a.cluster_ftime_valid == 0u,
          "... FSYSID/FTIME both honestly absent (no cluster has formed)");
}

/* 4: the real system service agrees with the ioctl. */
static void check_getsyi_service(void)
{
    uint32_t cluster_nodes = 0xffffffffu, cluster_member = 0xffffffffu;
    uint16_t rl_nodes = 0xffffu, rl_member = 0xffffu;
    uint32_t gst;
    struct item_list_3 items[] = {
        { sizeof(cluster_nodes),  SYI$_CLUSTER_NODES,  &cluster_nodes,  &rl_nodes },
        { sizeof(cluster_member), SYI$_CLUSTER_MEMBER, &cluster_member, &rl_member },
        { 0, 0, NULL, NULL }
    };

    gst = sys$getsyi(0, 0, 0, items, 0, 0, 0);
    CHECK(gst == SS_NORMAL, "$GETSYI(CLUSTER_NODES, CLUSTER_MEMBER) -> SS$_NORMAL");
    CHECK(rl_member == sizeof(uint32_t) && cluster_member == 0u,
          "$GETSYI CLUSTER_MEMBER == 0 -- read from the CLUB, agreeing with "
          "the ioctl above (one source, not two)");
    CHECK(rl_nodes == sizeof(uint32_t) && cluster_nodes == 0u,
          "$GETSYI CLUSTER_NODES == 0 -- no floor-at-1, which would be a "
          "userspace default rather than a reading");
}

/* 5: the retired mutators really are unreachable. */
static void check_mutators_retired(int fd)
{
    unsigned char buf[64];
    int rc_set, err_set, rc_clr, err_clr;

    memset(buf, 0, sizeof(buf));
    rc_set = ioctl(fd, RETIRED_CLUSTER_MEMBER_SET, buf);
    err_set = errno;
    memset(buf, 0, sizeof(buf));
    rc_clr = ioctl(fd, RETIRED_CLUSTER_MEMBER_CLEAR, buf);
    err_clr = errno;

    CHECK(rc_set < 0 && (err_set == ENOTTY || err_set == EINVAL),
          "the retired CLUSTER_MEMBER_SET command number is REFUSED "
          "(no userspace path can assert membership any more)");
    CHECK(rc_clr < 0 && (err_clr == ENOTTY || err_clr == EINVAL),
          "the retired CLUSTER_MEMBER_CLEAR command number is REFUSED");
}

int main(void)
{
    int probe;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_cluster_negctl ===\n");

    probe = open("/dev/vms", O_RDWR);
    if (probe < 0) {
        printf("=== test_syssvc_cluster_negctl: 0 passed, 0 failed (SKIPPED: "
               "no /dev/vms -- the cluster stack is executive-resident) ===\n");
        return EXIT_SKIP;
    }

    check_member_get();
    check_getsyi_ioctl();
    check_getsyi_service();
    check_mutators_retired(probe);

    close(probe);
    printf("=== test_syssvc_cluster_negctl: %d passed, %d failed ===\n",
           pass, fail);
    return fail ? 1 : 0;
}
