/*
 * test_kmod_dlm_xnode.c - the cross-node DLM RECEIVE handler, driven against a
 * real executive through /dev/vms (vms-94c transport; DLM epic vms-7fa rung 2
 * FOUNDATION GRANT, vms-e8f1).
 *
 * Proves the cross-node DLM dispatch on the MASTERING node: a decoded cross-node
 * DLM request (as src/vmsscs/scs_dlm.c would hand up from an SCS frame) reaches
 * the kernel lock manager's cross-node handler (vms_lock_dlm_xnode_dispatch) via
 * VMS_IOCTL_DLM_XNODE, and:
 *
 *   (1) ⭐ THE FOUNDATION GRANT (rung 2). A COMPATIBLE cross-node $ENQ is GRANTED
 *       (SS$_NORMAL, not the rung-1 SS$_UNSUPPORTED stub), and the grant is
 *       GENUINE: GET_RESMASTER shows the resource now exists (found=1), is
 *       mastered on this node (master_csid!=0, is_local_master=1), has one
 *       granted lock (n_granted=1), and -- the cross-node proof -- that lock is
 *       held FOR the REMOTE requester's CSID (remote_holder_csid == req_csid),
 *       NOT the local process. B's executive really holds a lock for A.
 *   (2) SCOPE FENCE / INV-6: a SECOND, INCOMPATIBLE cross-node $ENQ (EX over the
 *       held EX) is DECLINED with SS$_NOTQUEUED -- cross-node contention is a
 *       LATER rung, so the handler neither queues it nor fabricates a grant.
 *   (3) HONEST DECLINE of the ops that are NOT the foundation: DEQ (release),
 *       GRANT, and BLKAST still return SS$_UNSUPPORTED (later rungs) -- a real
 *       decline, never a faked receipt.
 *   (4) VALIDATION: a bad mode, a bad op, or an ENQ with an empty resource name
 *       is refused with SS$_BADPARAM, not silently dropped.
 *
 * Uses the same freestanding kernel-interface client the other test_kmod_*
 * suites use (src/libvmssys/vms_kif.c), not a hand-rolled ioctl copy. Returns
 * nonzero when /dev/vms is absent (the kernel-executive negative-control job
 * requires a real executive).
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

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_dlm_xnode ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
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
    CHECK(st == SS_BADPARAM, "bad lock mode -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(99u /*bad op*/, LCK_K_EXMODE, 0,
                           1, 0, REQ_CSID_A, 0, res, NULL);
    CHECK(st == SS_BADPARAM, "unknown op -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           1, 0, REQ_CSID_A, 0, "" /*empty name*/, NULL);
    CHECK(st == SS_BADPARAM, "ENQ with empty resource name -> SS$_BADPARAM");

    printf("test_kmod_dlm_xnode: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
