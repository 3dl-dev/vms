/*
 * test_syssvc_dlm_xnode.c - the cross-node DLM RECEIVE handler, driven against
 * a real executive through /dev/vms (vms-94c transport; DLM epic vms-7fa rung 2
 * FOUNDATION GRANT, vms-e8f1; rung 3 CROSS-NODE CONTENTION / block-then-grant +
 * BLKAST, vms-904c; single-node LP64/Alpha grant proof vms-17c).
 *
 * ⭐ THE LIVE LP64 CONTENTION PROOF. This is NOT a placeholder: the merged
 * dispatch (vms_lock_dlm_xnode_dispatch) really GRANTS an inbound compatible
 * cross-node $ENQ, QUEUES an incompatible one on the master's real waiting queue,
 * FIRES a blocking-AST decision naming the remote holder, and -- on the holder's
 * real cross-node $DEQ -- GRANTS the blocked request. Every transition is a READ
 * of the master's genuine executive lock state, not a fabricated status (INV-6).
 *
 * ONE FILE, BOTH ARCHES. Named test_syssvc_* (not test_kmod_*) so it is built by
 * the cmake qemu_syssvc_tests target and runs in the syssvc suite on BOTH x86_64
 * (tests/qemu/init.sh) AND Alpha LP64 (tools/cross-alpha/run-syssvc-tests-alpha.sh)
 * from a SINGLE source -- vms-17c's cheap single-node rung. It drives the
 * executive through the freestanding vms_kif client (src/libvmssys/vms_kif.c) and
 * a raw VMS_IOCTL_REGISTER, not the public sys$ API, because the cross-node
 * handler has no public sys$ entry point.
 *
 * A decoded cross-node DLM request (as src/vmsscs/scs_dlm.c would hand up from an
 * SCS frame) reaches vms_lock_dlm_xnode_dispatch via VMS_IOCTL_DLM_XNODE, and:
 *
 *   (1) ⭐ THE FOUNDATION GRANT (rung 2). A COMPATIBLE cross-node $ENQ is GRANTED
 *       (SS$_NORMAL) and the grant is GENUINE: GET_RESMASTER shows the resource
 *       mastered here (is_local_master=1), one lock granted (n_granted=1), held
 *       FOR the REMOTE requester's CSID (held_for == req_csid). GETLKI on the
 *       master handle shows granted_mode == EX.
 *   (2) ⭐ CONTENTION / BLOCK (rung 3). A SECOND, INCOMPATIBLE cross-node $ENQ
 *       (EX over the held EX, no NOQUEUE) is QUEUED, not declined: the dispatch
 *       returns VMS_DLM_STS_QUEUED (0 -- NOT SS$_NORMAL, NOT SS$_NOTQUEUED),
 *       queued=1, and a REAL waiting lock exists whose GETLKI granted_mode is NL
 *       (pending) while requested is EX. The master FIRES the blocking-AST
 *       decision: blocking_csid names the holder (A) that must get a BLKAST.
 *       n_granted stays 1 (the blocked request is waiting, not granted).
 *   (3) ⭐ BLOCK-THEN-GRANT (rung 3). The holder issues a real cross-node $DEQ;
 *       the master releases and grants the blocked request. GETLKI on the blocked
 *       handle now shows granted_mode == EX -- the unfakeable status flip NL->EX,
 *       driven by a real $DEQ -- and the master now holds FOR the second node's
 *       CSID.
 *   (3b) ⭐ DEFERRED-GRANT REPORT + REQUESTER-SIDE RECEIVE (vms-6ca, rung H5).
 *       The block-then-grant $DEQ now REPORTS the flipped waiter (the requester
 *       CSID + original req_lkid + master handle + granted mode) so the daemon
 *       can WIRE a deferred GRANT to it. And the REQUESTER-SIDE GRANT RECEIVE
 *       (VMS_DLM_OP_GRANT, was SS$_UNSUPPORTED) completes an executive-resident
 *       ORIGIN record: a queued-reply (mode NL) leaves it PENDING, a deferred
 *       GRANT (mode EX) flips it NL->EX -- the status flip observed on the
 *       REQUESTER node, driven only by what the master sent over the wire, read
 *       back by GETLKI. INV-6: genuine executive state, no fabricated grant.
 *   (3c) ⭐ THE BLKAST WIRE + HOLDER-SIDE RECEIVE (vms-76d, rung H6). The symmetric
 *       mirror of (3b): an ENQ that queues behind a cross-node holder now REPORTS
 *       blocking_req_lkid (the holder's requester-side handle, the BLKAST target).
 *       A GRANT receive at a granted mode carrying a blocking-AST routine
 *       establishes a HOLDER origin record; a BLKAST the master sends
 *       (VMS_DLM_OP_BLKAST, was SS$_UNSUPPORTED) then FIRES that routine for real
 *       -- a genuine user-mode AST queued to the holder's process, DRAINED back via
 *       DELIVERAST with the exact astadr/astprm registered. INV-6: genuine
 *       executive state, no fabricated AST.
 *   (4) SCOPE FENCE / INV-6: a wire NOQUEUE incompatible $ENQ still declines
 *       SS$_NOTQUEUED (honest); a $DEQ of a lock NOT held for the releasing node
 *       is refused SS$_IVLOCKID; a BLKAST naming a handle with no holder record or
 *       no registered blocking-AST routine still returns SS$_UNSUPPORTED (honest,
 *       no faked AST).
 *   (5) VALIDATION unchanged: a bad mode, a bad op, or an ENQ with an empty
 *       resource name is refused SS$_BADPARAM, not silently dropped.
 *
 * Honest SKIP (77) when /dev/vms is absent -- the cross-node handler is
 * executive-resident, so with no /dev/vms there is nothing to dispatch and
 * nothing this suite can fabricate; never a fake pass (the test_syssvc_* honest-
 * skip-77 contract, .github/workflows/ci.yml kernel-executive-negative-control).
 *
 * Grounding: docs/research-alpha-dlm-wire.md §4 (the ENQ->GRANT->BLKAST->DEQ
 * sequence, DOCUMENTED from public $ENQ/$DEQ/$LCKDEF/IDSM/Cluster Systems
 * sources); docs/design-cluster-node.md §5; docs/compat/facilities/cluster-dlm.yaml.
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
#define SS_IVLOCKID     8484u
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
    const uint32_t REQ_CSID_A = 1025u;   /* the first remote requester's CSID  */
    const uint32_t REQ_CSID_B = 1026u;   /* the second (blocking) requester    */
    const uint32_t REQ_CSID_C = 1027u;   /* a third (NOQUEUE probe)            */
    uint32_t st;
    uint32_t lkid_a = 0, lkid_b = 0, queued = 0, blk_csid = 0, blk_lkid = 0;

    /* ---- 1. ⭐ THE FOUNDATION GRANT: a compatible cross-node ENQ is GRANTED -- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           0x00040011u /*req_lkid*/, 0 /*master_lkid*/,
                           REQ_CSID_A, 0 /*master_csid: resolve*/, res, NULL,
                           &lkid_a, &queued, &blk_csid, &blk_lkid, NULL, NULL);
    CHECK(st == SS_NORMAL, "cross-node ENQ (compatible) -> SS$_NORMAL (rung-2 grant)");
    CHECK(queued == 0, "the granted request was NOT queued");
    CHECK(lkid_a != 0, "the master returned a lock handle for the grant");

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

    /* the master's own lock record: granted at EX */
    uint32_t gm = 99, rm = 99;
    st = vms_kif_getlki(lkid_a, &gm, &rm, NULL, NULL);
    CHECK(st == SS_NORMAL && gm == LCK_K_EXMODE,
          "GETLKI: the holder's lock is granted at EX");

    /* ---- 2. ⭐ CONTENTION: a SECOND, INCOMPATIBLE cross-node ENQ BLOCKS ------
     * EX over the held EX, no NOQUEUE. The master QUEUES it on its real waiting
     * queue (contention rung, vms-904c): the dispatch returns VMS_DLM_STS_QUEUED
     * -- NOT SS$_NORMAL (that is a grant), NOT SS$_NOTQUEUED (that is a NOQUEUE
     * decline) -- with queued=1 and a real waiting lock. It also FIRES the
     * blocking-AST decision: blocking_csid names holder A. */
    queued = 0; blk_csid = 0; blk_lkid = 0;
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           0x00050022u, 0, REQ_CSID_B, 0, res, NULL,
                           &lkid_b, &queued, &blk_csid, &blk_lkid, NULL, NULL);
    CHECK(st == (uint32_t)VMS_DLM_STS_QUEUED,
          "second incompatible cross-node ENQ -> VMS_DLM_STS_QUEUED (blocked, not granted)");
    CHECK(st != SS_NORMAL && st != SS_NOTQUEUED,
          "the blocked status is neither a grant nor a NOQUEUE decline");
    CHECK(queued == 1, "the request was QUEUED (queued=1)");
    CHECK(lkid_b != 0 && lkid_b != lkid_a, "a distinct master lock handle for the queued request");
    CHECK(blk_csid == REQ_CSID_A,
          "the master FIRED a BLKAST decision naming the blocking holder (A)");
    CHECK(blk_lkid == lkid_a, "the BLKAST directive targets the holder's lock handle");

    /* the queued lock is REAL and PENDING: granted NL, requested EX */
    gm = 99; rm = 99;
    st = vms_kif_getlki(lkid_b, &gm, &rm, NULL, NULL);
    CHECK(st == SS_NORMAL && gm == LCK_K_NLMODE && rm == LCK_K_EXMODE,
          "GETLKI: the blocked request is PENDING (granted NL, requested EX)");

    /* still exactly one GRANTED lock -- the blocked request waits, not counted */
    n_granted = 99;
    (void)vms_kif_get_resmaster(res, &found, &local_csid, &dir_csid,
                                &master_csid, &is_local_master, &n_granted, &held_for);
    CHECK(n_granted == 1, "still one grant -- the blocked request is waiting, not granted");

    /* ---- 3. SCOPE FENCE: a NOQUEUE incompatible ENQ still DECLINES ---------- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, LCK_M_NOQUEUE,
                           0x00060033u, 0, REQ_CSID_C, 0, res, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_NOTQUEUED,
          "NOQUEUE incompatible cross-node ENQ -> SS$_NOTQUEUED (honest decline preserved)");

    /* ---- 4. cross-node $DEQ authorization: wrong CSID is refused ------------ */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_DEQ, LCK_K_NLMODE, 0,
                           0, lkid_b /*B's queued lock*/, REQ_CSID_A /*wrong owner*/,
                           0, res, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_IVLOCKID,
          "cross-node DEQ of a lock NOT held for the releasing node -> SS$_IVLOCKID");

    /* ---- 5. ⭐ BLOCK-THEN-GRANT + DEFERRED-GRANT REPORT (vms-6ca, H5) --------
     * The holder DEQs; the master releases and GRANTS the blocked request. The
     * $DEQ dispatch now REPORTS the flipped waiter through the fields a DEQ
     * otherwise leaves 0 -- queued=1 (a waiter flipped), blocking_csid = the
     * requester to notify (B), blocking_master_lkid = its master handle (lkid_b),
     * def_req_lkid = its ORIGINAL requester handle (0x00050022), def_mode = the
     * mode it was granted at (EX). This is what the daemon reads to WIRE the
     * deferred GRANT to the requester. INV-6: it reports a grant the executive
     * genuinely made. */
    uint32_t def_flipped = 0, def_csid = 0, def_master_lkid = 0,
             def_req_lkid = 0, def_mode = 0;
    st = vms_kif_dlm_xnode(VMS_DLM_OP_DEQ, LCK_K_NLMODE, 0,
                           0, lkid_a, REQ_CSID_A, 0, res, NULL,
                           NULL, &def_flipped, &def_csid, &def_master_lkid,
                           &def_req_lkid, &def_mode);
    CHECK(st == SS_NORMAL, "holder's cross-node $DEQ -> SS$_NORMAL (released)");
    CHECK(def_flipped == 1, "the $DEQ REPORTED a deferred grant (a queued waiter flipped)");
    CHECK(def_csid == REQ_CSID_B,
          "the deferred-grant report names the requester to notify (B)");
    CHECK(def_master_lkid == lkid_b,
          "the deferred-grant report carries the flipped waiter's master handle");
    CHECK(def_req_lkid == 0x00050022u,
          "the deferred-grant report carries the requester's ORIGINAL lock handle");
    CHECK(def_mode == LCK_K_EXMODE,
          "the deferred-grant report carries the granted mode (EX)");

    /* the previously-blocked request has FLIPPED to granted at EX */
    gm = 99; rm = 99;
    st = vms_kif_getlki(lkid_b, &gm, &rm, NULL, NULL);
    CHECK(st == SS_NORMAL && gm == LCK_K_EXMODE,
          "GETLKI: the blocked request FLIPPED to granted at EX (NL->EX) after the $DEQ");

    /* the master now holds the resource FOR the second node's CSID */
    n_granted = 99; held_for = 99;
    (void)vms_kif_get_resmaster(res, &found, &local_csid, &dir_csid,
                                &master_csid, &is_local_master, &n_granted, &held_for);
    CHECK(n_granted == 1 && held_for == REQ_CSID_B,
          "the master now holds one lock, held FOR the second node (B)");

    /* ---- 5b. ⭐ REQUESTER-SIDE GRANT RECEIVE (vms-6ca, H5) -------------------
     * The OTHER half of the wire: on the REQUESTER node, a GRANT / queued-reply
     * the master sent back completes an executive-resident ORIGIN record, so the
     * request's status is genuine executive state (INV-6), not a userspace flag.
     * A queued-reply (granted mode NL) leaves it PENDING; a deferred GRANT
     * (granted mode EX) flips it NL->EX -- the status flip observed on the
     * REQUESTER node, driven ONLY by what the master sent over the wire. The op
     * was SS$_UNSUPPORTED before this rung. GETLKI reads the flip back
     * independently. RREQ_LKID is the requester's own handle for the request. */
    const uint32_t RREQ_LKID = 0x00090077u;
    /* (i) queued-reply arrives: mode NL, master handle 0x0808 on CSID B. */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_GRANT, LCK_K_NLMODE, 0,
                           RREQ_LKID, 0x00000808u, REQ_CSID_A, REQ_CSID_B,
                           "RORIGIN1", NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_NORMAL,
          "requester-side queued-reply RECEIVE -> SS$_NORMAL (was SS$_UNSUPPORTED)");
    gm = 99; rm = 99;
    st = vms_kif_getlki(RREQ_LKID, &gm, &rm, NULL, NULL);
    CHECK(st == SS_NORMAL && gm == LCK_K_NLMODE,
          "GETLKI on the requester: the origin record is PENDING (granted NL)");
    /* (ii) deferred GRANT arrives: mode EX -> the origin record FLIPS NL->EX. */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_GRANT, LCK_K_EXMODE, 0,
                           RREQ_LKID, 0x00000808u, REQ_CSID_A, REQ_CSID_B,
                           "RORIGIN1", NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_NORMAL, "requester-side deferred GRANT RECEIVE -> SS$_NORMAL");
    gm = 99; rm = 99;
    st = vms_kif_getlki(RREQ_LKID, &gm, &rm, NULL, NULL);
    CHECK(st == SS_NORMAL && gm == LCK_K_EXMODE,
          "GETLKI on the requester: the origin record FLIPPED to GRANTED at EX (NL->EX)");
    /* a GRANT with no requester handle is refused, not silently dropped */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_GRANT, LCK_K_EXMODE, 0,
                           0 /*no req_lkid*/, 0x00000808u, REQ_CSID_A, REQ_CSID_B,
                           "RORIGIN1", NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_BADPARAM, "requester-side GRANT with no req_lkid -> SS$_BADPARAM");

    /* ---- 6. release the second lock; the resource is torn down -------------- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_DEQ, LCK_K_NLMODE, 0,
                           0, lkid_b, REQ_CSID_B, 0, res, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_NORMAL, "second node's cross-node $DEQ -> SS$_NORMAL");

    /* ---- 7. ⭐ THE BLKAST WIRE: holder-side blocking-AST delivery (vms-76d, H6)
     * The symmetric mirror of the requester-side GRANT RECEIVE. First a HOLDER
     * origin record is established WITH a blocking-AST routine (a GRANT receive at
     * a granted mode carrying blkastadr); then a BLKAST the master sent FIRES that
     * routine for real -- a genuine user-mode AST queued to this process, DRAINED
     * back via DELIVERAST with the exact astadr registered. INV-6: a BLKAST naming
     * a handle with no holder record / no blocking-AST routine still declines
     * SS$_UNSUPPORTED -- never a fabricated AST. */
    const uint32_t HREQ_LKID = 0x000900A1u;   /* the holder's own request handle */
    const uint64_t HBLKASTADR = 0xC0DE1234BEEF0000ull; /* the holder's BLKAST routine */
    const uint64_t HBLKASTPRM = 0x00000000000000A1ull;
    uint32_t delivered = 99;
    /* (i) establish the HOLDER origin at EX WITH a blocking-AST routine. */
    st = vms_kif_dlm_xnode_blkast(VMS_DLM_OP_GRANT, LCK_K_EXMODE,
                                  HREQ_LKID, 0x00000909u, REQ_CSID_A, REQ_CSID_B,
                                  "HORIGIN1", HBLKASTADR, HBLKASTPRM, NULL, NULL);
    CHECK(st == SS_NORMAL, "holder-side GRANT establishes the origin at EX with a BLKAST routine");
    /* (ii) a BLKAST naming an UNKNOWN handle still declines honestly (INV-6). */
    st = vms_kif_dlm_xnode_blkast(VMS_DLM_OP_BLKAST, LCK_K_EXMODE,
                                  0x00040011u, 0, REQ_CSID_A, REQ_CSID_B, "",
                                  0, 0, NULL, &delivered);
    CHECK(st == SS_UNSUPPORTED && delivered == 0,
          "BLKAST for an unknown holder handle -> SS$_UNSUPPORTED (honest, no fake AST)");
    /* (iii) ⭐ the real BLKAST: it FIRES the holder's blocking AST. */
    delivered = 99;
    st = vms_kif_dlm_xnode_blkast(VMS_DLM_OP_BLKAST, LCK_K_EXMODE,
                                  HREQ_LKID, 0, REQ_CSID_A, REQ_CSID_B, "HORIGIN1",
                                  0, 0, NULL, &delivered);
    CHECK(st == SS_NORMAL && delivered == 1,
          "holder-side BLKAST RECEIVE -> SS$_NORMAL, a real blocking AST queued (was SS$_UNSUPPORTED)");
    /* (iv) ⭐⭐ DRAIN it: the AST genuinely landed on this process's USER queue,
     * carrying the exact routine + parameter the holder registered. */
    {
        uint64_t d_astadr = 0, d_astprm = 0;
        uint8_t d_acmode = 99;
        int drc = vms_kif_deliverast(&d_astadr, &d_astprm, &d_acmode);
        CHECK(drc == 0 && d_astadr == HBLKASTADR && d_astprm == HBLKASTPRM,
              "DELIVERAST drains the blocking AST: astadr/astprm == the holder's registered routine (the AST really fired)");
    }

    /* ---- 8. malformed requests are refused, not dropped ---- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE + 1, 0,
                           1, 0, REQ_CSID_A, 0, res, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    /* negctl: dlm-xnode-mode-unvalidated */
    CHECK(st == SS_BADPARAM, "bad lock mode -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(99u /*bad op*/, LCK_K_EXMODE, 0,
                           1, 0, REQ_CSID_A, 0, res, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_BADPARAM, "unknown op -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           1, 0, REQ_CSID_A, 0, "" /*empty name*/, NULL,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(st == SS_BADPARAM, "ENQ with empty resource name -> SS$_BADPARAM");

    printf("=== test_syssvc_dlm_xnode: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
