/*
 * test_syssvc_cluster_member.c - cluster membership crosses into the
 * executive (rd vms-551, docs/design-cluster-membership-executive.md),
 * driven against a real executive through /dev/vms via the freestanding kif
 * client (src/libvmssys/vms_kif.c), the same footing as
 * test_syssvc_dlm_xnode.c and test_kmod_resdir.c.
 *
 * vms.ko owns a NEW module-global membership block -- SEPARATE from
 * dlm_member_csids (the static insmod DLM directory vector vms-50e is
 * actively enqueuing against) -- that scsd populates at runtime via
 * VMS_IOCTL_CLUSTER_MEMBER_SET/CLEAR and SHOW CLUSTER reads back via
 * VMS_IOCTL_CLUSTER_MEMBER_GET. This suite drives all three directly:
 *
 *   1. A FRESH executive (nothing has SET yet) reports n_members==0 with
 *      SS$_NORMAL -- a genuine NOTMEMBER view, never an error. This is the
 *      NOTMEMBER != NOSUCHDEV distinction docs/design-cluster-membership-
 *      executive.md requires: the executive ANSWERED, it simply has nothing
 *      to report.
 *   2. SET a few members (csid/sysid/scsnode/state); GET reads them back
 *      from the REAL executive block, VALUE-VERIFIED field-by-field --
 *      not merely that n_members rose.
 *   3. A repeat SET on an existing csid UPDATES in place (insert-or-update
 *      by csid), not a duplicate append.
 *   4. CLEAR one member; GET shows the view shrank by exactly one, and the
 *      cleared csid is gone while the others are untouched.
 *   5. CLEAR is idempotent: clearing an absent csid is SS$_NORMAL, a no-op,
 *      never an error -- a retransmitted departure signal cannot fail it.
 *
 * INV-6: every assertion below reads the REAL executive-resident block
 * through /dev/vms; nothing here hand-sets a userspace structure or fakes a
 * membership view.
 *
 * Honest SKIP (77) when /dev/vms is absent -- this facility is wholly
 * executive-resident, so there is nothing to drive and nothing this suite
 * can fabricate (the test_syssvc_* honest-skip-77 contract, ci.yml
 * kernel-executive-negative-control).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

#define SS_NORMAL   1u
#define EXIT_SKIP    77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

static int find_csid(const struct vms_cluster_member *m, uint32_t n, uint32_t csid)
{
    for (uint32_t i = 0; i < n; i++)
        if (m[i].csid == csid)
            return (int)i;
    return -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_cluster_member ===\n");

    int probe = open("/dev/vms", O_RDWR);
    if (probe < 0) {
        printf("=== test_syssvc_cluster_member: 0 passed, 0 failed (SKIPPED: no "
               "/dev/vms -- the cluster-membership block is executive-resident) ===\n");
        return EXIT_SKIP;
    }
    close(probe);

    struct vms_cluster_member members[VMS_CLUSTER_MEMBER_MAX];
    uint32_t n = 0xffffffffu;
    uint32_t st;

    /* ---- 1. A fresh executive: NOTMEMBER, not an error --------------- */
    memset(members, 0, sizeof(members));
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);
    CHECK(st == SS_NORMAL, "GET on a fresh executive -> SS$_NORMAL (executive answered)");
    CHECK(n == 0, "fresh executive reports n_members==0 (NOTMEMBER, not an error)");

    /* ---- 2. SET three members; GET reads them back, value-verified --- */
    const uint32_t CSID_A = 1025u, CSID_B = 1026u, CSID_C = 1027u;
    st = vms_kif_cluster_member_set(CSID_A, 1025u, "NODEA", "MEMBER");
    CHECK(st == SS_NORMAL, "SET NODEA (csid 1025) -> SS$_NORMAL");
    st = vms_kif_cluster_member_set(CSID_B, 1026u, "NODEB", "MEMBER");
    CHECK(st == SS_NORMAL, "SET NODEB (csid 1026) -> SS$_NORMAL");
    st = vms_kif_cluster_member_set(CSID_C, 1027u, "NODEC", "BRK_NON");
    CHECK(st == SS_NORMAL, "SET NODEC (csid 1027, BRK_NON) -> SS$_NORMAL");

    memset(members, 0, sizeof(members));
    n = 0;
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);
    CHECK(st == SS_NORMAL, "GET after 3 SETs -> SS$_NORMAL");
    CHECK(n == 3, "GET reports exactly the 3 members SET");

    int ia = find_csid(members, n, CSID_A);
    int ib = find_csid(members, n, CSID_B);
    int ic = find_csid(members, n, CSID_C);
    CHECK(ia >= 0 && ib >= 0 && ic >= 0, "all three csids are present in the readback");
    if (ia >= 0) {
        CHECK(members[ia].sysid == 1025u, "NODEA: sysid matches what was SET");
        CHECK(strcmp(members[ia].scsnode, "NODEA") == 0, "NODEA: scsnode matches");
        CHECK(strcmp(members[ia].state, "MEMBER") == 0, "NODEA: state matches");
    }
    if (ic >= 0) {
        CHECK(members[ic].sysid == 1027u, "NODEC: sysid matches what was SET");
        CHECK(strcmp(members[ic].scsnode, "NODEC") == 0, "NODEC: scsnode matches");
        CHECK(strcmp(members[ic].state, "BRK_NON") == 0, "NODEC: state (BRK_NON) matches");
    }

    /* ---- 3. A repeat SET on an existing csid UPDATES, not appends ---- */
    st = vms_kif_cluster_member_set(CSID_B, 1026u, "NODEB", "BRK_NON");
    CHECK(st == SS_NORMAL, "re-SET NODEB with a new state -> SS$_NORMAL");
    memset(members, 0, sizeof(members));
    n = 0;
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);
    CHECK(st == SS_NORMAL, "GET after re-SET -> SS$_NORMAL");
    CHECK(n == 3, "re-SET on an existing csid UPDATED in place -- count still 3, not 4");
    ib = find_csid(members, n, CSID_B);
    CHECK(ib >= 0 && strcmp(members[ib].state, "BRK_NON") == 0,
          "NODEB's state reflects the update (BRK_NON)");

    /* ---- 4. CLEAR one member; the view shrinks by exactly one -------- */
    st = vms_kif_cluster_member_clear(CSID_B);
    CHECK(st == SS_NORMAL, "CLEAR NODEB (csid 1026) -> SS$_NORMAL");
    memset(members, 0, sizeof(members));
    n = 0;
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);
    CHECK(st == SS_NORMAL, "GET after CLEAR -> SS$_NORMAL");
    CHECK(n == 2, "GET reports 2 members after CLEARing one of 3");
    CHECK(find_csid(members, n, CSID_B) < 0, "the cleared csid (NODEB) is gone");
    ia = find_csid(members, n, CSID_A);
    ic = find_csid(members, n, CSID_C);
    CHECK(ia >= 0 && ic >= 0, "the two untouched members (NODEA, NODEC) survive the CLEAR");

    /* ---- 5. CLEAR is idempotent: an absent csid is a no-op, not an error */
    st = vms_kif_cluster_member_clear(CSID_B);
    CHECK(st == SS_NORMAL, "CLEAR of an already-absent csid -> SS$_NORMAL (idempotent no-op)");
    memset(members, 0, sizeof(members));
    n = 0;
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);
    CHECK(st == SS_NORMAL && n == 2,
          "GET after the idempotent CLEAR still reports 2 -- nothing else moved");

    /* ---- cleanup: CLEAR the two remaining members this suite added --- */
    (void)vms_kif_cluster_member_clear(CSID_A);
    (void)vms_kif_cluster_member_clear(CSID_C);
    memset(members, 0, sizeof(members));
    n = 0xffffffffu;
    st = vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX, &n);
    CHECK(st == SS_NORMAL && n == 0,
          "GET after cleanup: back to n_members==0 (NOTMEMBER, SS$_NORMAL)");

    printf("=== test_syssvc_cluster_member: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
