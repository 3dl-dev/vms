/*
 * test_syssvc_dlm_xnode.c - the cross-node DLM RECEIVE handler, driven against
 * a real executive through /dev/vms (vms-94c transport; DLM epic vms-7fa rung 2
 * FOUNDATION GRANT, vms-e8f1; single-node LP64/Alpha grant proof vms-17c).
 *
 * ⭐ THE LIVE LP64 GRANT PROOF (post-#849). This is NOT a rung-1 placeholder:
 * the merged rung-2 dispatch (vms_lock_dlm_xnode_dispatch) really GRANTS an
 * inbound compatible cross-node $ENQ through vms_enq_core on the mastering node,
 * holding the lock for the REMOTE requester's CSID. This suite asserts that
 * genuine behavior on a 64-bit target -- proving the cross-node grant, and the
 * vms_dlm_xnode_args / vms_enq_args LP64 struct layout it rides, are width-
 * correct under Alpha LP64 (owner_csid / req_csid / the GET_RESMASTER held_for
 * readback are all quadword-clean).
 *
 * ONE FILE, BOTH ARCHES. Named test_syssvc_* (not test_kmod_*) so it is built by
 * the cmake qemu_syssvc_tests target and runs in the syssvc suite on BOTH x86_64
 * (tests/qemu/init.sh) AND Alpha LP64 (tools/cross-alpha/run-syssvc-tests-alpha.sh)
 * from a SINGLE source -- vms-17c's cheap single-node rung. It still drives the
 * executive through the freestanding vms_kif client (src/libvmssys/vms_kif.c)
 * and a raw VMS_IOCTL_REGISTER, not the public sys$ API, because the cross-node
 * handler has no public sys$ entry point.
 *
 * A decoded cross-node DLM request (as src/vmsscs/scs_dlm.c would hand up from an
 * SCS frame) reaches vms_lock_dlm_xnode_dispatch via VMS_IOCTL_DLM_XNODE, and:
 *
 *   (1) ⭐ THE FOUNDATION GRANT (rung 2). A COMPATIBLE cross-node $ENQ is GRANTED
 *       (SS$_NORMAL), and the grant is GENUINE: GET_RESMASTER shows the resource
 *       now exists (found=1), is mastered on this node (master_csid==local_csid,
 *       is_local_master=1), has one granted lock (n_granted=1), and -- the
 *       cross-node proof -- that lock is held FOR the REMOTE requester's CSID
 *       (held_for == req_csid), NOT the local process. B's executive really
 *       holds a lock for A.
 *   (2) SCOPE FENCE / INV-6: a SECOND, INCOMPATIBLE cross-node $ENQ (EX over the
 *       held EX) is DECLINED with SS$_NOTQUEUED -- rung 2 uses LCK_M_NOQUEUE, so
 *       cross-node contention (a LATER rung) is neither queued nor fabricated;
 *       n_granted stays 1.
 *   (3) HONEST DECLINE of the ops that are NOT the foundation: DEQ (release),
 *       GRANT, and BLKAST still return SS$_UNSUPPORTED (later rungs) -- a real
 *       decline, never a faked receipt.
 *   (4) VALIDATION unchanged at every rung: a bad mode, a bad op, or an ENQ with
 *       an empty resource name is refused with SS$_BADPARAM, not silently dropped.
 *
 * Uses the same freestanding kernel-interface client the other executive suites
 * use (src/libvmssys/vms_kif.c), not a hand-rolled ioctl copy. Honest SKIP (77)
 * when /dev/vms is absent -- the cross-node handler is executive-resident, so
 * with no /dev/vms there is nothing to dispatch and nothing this suite can
 * fabricate; never a fake pass (the test_syssvc_* honest-skip-77 contract,
 * .github/workflows/ci.yml kernel-executive-negative-control).
 *
 * Grounding: docs/research-alpha-dlm-wire.md §7 (rung-2 GRANT semantics from
 * public $ENQ/$LCKDEF/IDSM/Cluster Systems sources); docs/design-cluster-node.md
 * §5; docs/compat/facilities/cluster-dlm.yaml.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

#define SS_NORMAL       1u
#define SS_BADPARAM     0x14u
#define SS_NOTQUEUED    2488u
#define SS_UNSUPPORTED  2296u
#define EXIT_SKIP       77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dlm_xnode ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        /* No executive present: the cross-node handler is executive-resident,
         * so there is nothing to dispatch and nothing to fabricate. Honest
         * SKIP (77), never a fake pass -- the test_syssvc_* device-absent
         * contract (ci.yml kernel-executive-negative-control). */
        printf("=== test_syssvc_dlm_xnode: 0 passed, 0 failed (SKIPPED: no /dev/vms -- "
               "the cross-node DLM dispatch is executive-resident) ===\n");
        return EXIT_SKIP;
    }

    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    reg.vms_pid = (uint32_t)getpid();
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(reg.status == SS_NORMAL, "register");

    const char *res = "DLMXNODE1";
    const uint32_t REQ_CSID_A = 1025u;   /* the remote requester's CSID */

    /* ---- 1. ⭐ THE FOUNDATION GRANT: a compatible cross-node ENQ is GRANTED -- */
    uint32_t st;
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           0x00040011u /*req_lkid*/, 0 /*master_lkid*/,
                           REQ_CSID_A, 0 /*master_csid: resolve*/, res, NULL);
    CHECK(st == SS_NORMAL, "cross-node ENQ (compatible) -> SS$_NORMAL (rung-2 grant)");

    /* ---- 1b. the grant is GENUINE: the master's DB really holds it for A ---- */
    uint32_t found = 0, local_csid = 0, dir_csid = 0, master_csid = 0,
             is_local_master = 0, n_granted = 0, held_for = 0;
    st = vms_kif_get_resmaster(res, &found, &local_csid, &dir_csid,
                               &master_csid, &is_local_master, &n_granted,
                               &held_for);
    CHECK(st == SS_NORMAL, "GET_RESMASTER readback");
    CHECK(found == 1, "cross-node ENQ mastered the resource (found=1)");
    CHECK(master_csid != 0 && master_csid == local_csid,
          "resource is mastered on THIS node (master_csid==local_csid)");
    CHECK(is_local_master == 1, "is_local_master=1");
    CHECK(n_granted == 1, "exactly one lock granted on the resource");
    CHECK(held_for == REQ_CSID_A,
          "the grant is held FOR the REMOTE requester's CSID (not the local proc)");

    /* ---- 2. SCOPE FENCE: a SECOND, INCOMPATIBLE cross-node ENQ is DECLINED ---
     * EX over the held EX cannot be granted; cross-node contention is a later
     * rung, so the handler declines with SS$_NOTQUEUED -- it does NOT queue the
     * request and does NOT fabricate a grant (INV-6). n_granted stays 1. */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           0x00050022u, 0, 1026u /*a different peer*/, 0, res, NULL);
    CHECK(st == SS_NOTQUEUED,
          "second incompatible cross-node ENQ -> SS$_NOTQUEUED (contention not faked/queued)");
    found = 99; n_granted = 99;
    (void)vms_kif_get_resmaster(res, &found, &local_csid, &dir_csid,
                                &master_csid, &is_local_master, &n_granted, &held_for);
    CHECK(n_granted == 1, "still exactly one grant -- the declined request was NOT queued");

    /* ---- 3. HONEST DECLINE of the non-foundation ops (later rungs) ---- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_DEQ, LCK_K_NLMODE, 0,
                           0x00040011u, 0x00080002u, REQ_CSID_A, 1026u, res, NULL);
    CHECK(st == SS_UNSUPPORTED, "cross-node DEQ (release) -> SS$_UNSUPPORTED (later rung)");

    /* GRANT/BLKAST are responses -- they carry no resource name, and the handler
     * still reaches them and declines (does not BADPARAM on the empty name). */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_GRANT, LCK_K_EXMODE, 0,
                           0x00040011u, 0x00080002u, REQ_CSID_A, 1026u, "", NULL);
    CHECK(st == SS_UNSUPPORTED, "cross-node GRANT (no resnam) -> SS$_UNSUPPORTED");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_BLKAST, LCK_K_EXMODE, 0,
                           0x00040011u, 0x00080002u, REQ_CSID_A, 1026u, "", NULL);
    CHECK(st == SS_UNSUPPORTED, "cross-node BLKAST (no resnam) -> SS$_UNSUPPORTED");

    /* ---- 4. malformed requests are refused, not dropped ---- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE + 1, 0,
                           1, 0, REQ_CSID_A, 0, res, NULL);
    /* negctl: dlm-xnode-mode-unvalidated */
    CHECK(st == SS_BADPARAM, "bad lock mode -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(99u /*bad op*/, LCK_K_EXMODE, 0,
                           1, 0, REQ_CSID_A, 0, res, NULL);
    CHECK(st == SS_BADPARAM, "unknown op -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           1, 0, REQ_CSID_A, 0, "" /*empty name*/, NULL);
    CHECK(st == SS_BADPARAM, "ENQ with empty resource name -> SS$_BADPARAM");

    printf("=== test_syssvc_dlm_xnode: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
