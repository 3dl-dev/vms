/*
 * test_kmod_dlm_xnode.c - the cross-node DLM RECEIVE handler, driven against a
 * real executive through /dev/vms (vms-94c, DLM epic vms-7fa rung 1).
 *
 * Proves the RECEIVE half of the DLM message transport: a decoded cross-node
 * DLM request (as src/vmsscs/scs_dlm.c would hand up from an SCS frame) reaches
 * the kernel lock manager's cross-node handler (vms_lock_dlm_xnode_dispatch) via
 * VMS_IOCTL_DLM_XNODE, and:
 *
 *   (1) the handler is REACHABLE and returns SS$_UNSUPPORTED for every valid op
 *       (ENQ/GRANT/DEQ/BLKAST) -- rung 1 is the transport, the grant is rung 2;
 *   (2) it VALIDATES the request: a bad mode, a bad op, or an ENQ/DEQ with an
 *       empty resource name is refused with SS$_BADPARAM, not silently dropped;
 *   (3) ⭐ INV-6: NO FABRICATED GRANT. After a cross-node ENQ is dispatched, the
 *       resource is STILL not present and STILL unmastered (GET_RESMASTER
 *       found=0, master_csid=0). The handler did not create, master, or grant
 *       anything -- it honestly declined, exactly as dlm_resolve_master() does
 *       for the SEND side. This is what separates rung 1 (a real pipe that
 *       declines) from a facade (a pipe that fakes success).
 *
 * Uses the same freestanding kernel-interface client the other test_kmod_*
 * suites use (src/libvmssys/vms_kif.c), not a hand-rolled ioctl copy. Returns
 * nonzero when /dev/vms is absent (the kernel-executive negative-control job
 * requires a real executive).
 *
 * Grounding: docs/design-cluster-node.md §5; docs/compat/facilities/cluster-dlm.yaml.
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

    static const uint8_t vblk[LCK_VALBLK_SIZE] = {
        0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
        0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF
    };
    const char *res = "DLMXNODE1";

    /* ---- 1. every valid op reaches the handler and honestly declines ---- */
    uint32_t st;
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0x0011 /*VALBLK|SYSTEM*/,
                           0x00040011u, 0, 1025u, 0, res, vblk);
    CHECK(st == SS_UNSUPPORTED, "cross-node ENQ reaches handler -> SS$_UNSUPPORTED (rung 1)");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_DEQ, LCK_K_NLMODE, 0,
                           0x00040011u, 0x00080002u, 1025u, 1026u, res, NULL);
    CHECK(st == SS_UNSUPPORTED, "cross-node DEQ -> SS$_UNSUPPORTED");

    /* GRANT/BLKAST are responses -- they carry no resource name, and the handler
     * still reaches them and declines (does not BADPARAM on the empty name). */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_GRANT, LCK_K_EXMODE, 0,
                           0x00040011u, 0x00080002u, 1025u, 1026u, "", NULL);
    CHECK(st == SS_UNSUPPORTED, "cross-node GRANT (no resnam) -> SS$_UNSUPPORTED");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_BLKAST, LCK_K_EXMODE, 0,
                           0x00040011u, 0x00080002u, 1025u, 1026u, "", NULL);
    CHECK(st == SS_UNSUPPORTED, "cross-node BLKAST (no resnam) -> SS$_UNSUPPORTED");

    /* ---- 2. malformed requests are refused, not dropped ---- */
    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE + 1, 0,
                           1, 0, 1025u, 0, res, NULL);
    CHECK(st == SS_BADPARAM, "bad lock mode -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(99u /*bad op*/, LCK_K_EXMODE, 0,
                           1, 0, 1025u, 0, res, NULL);
    CHECK(st == SS_BADPARAM, "unknown op -> SS$_BADPARAM");

    st = vms_kif_dlm_xnode(VMS_DLM_OP_ENQ, LCK_K_EXMODE, 0,
                           1, 0, 1025u, 0, "" /*empty name*/, NULL);
    CHECK(st == SS_BADPARAM, "ENQ with empty resource name -> SS$_BADPARAM");

    /* ---- 3. ⭐ INV-6: the ENQ dispatch fabricated NOTHING ---- */
    uint32_t found = 99, local_csid = 0, dir_csid = 0, master_csid = 99,
             is_local_master = 99, n_granted = 99;
    st = vms_kif_get_resmaster(res, &found, &local_csid, &dir_csid,
                               &master_csid, &is_local_master, &n_granted);
    CHECK(st == SS_NORMAL, "GET_RESMASTER readback");
    CHECK(found == 0, "cross-node ENQ did NOT create the resource (no fake grant)");
    CHECK(master_csid == 0, "cross-node ENQ did NOT master the resource");
    CHECK(is_local_master == 0, "resource is not locally mastered");
    CHECK(n_granted == 0, "no lock was granted for the cross-node request");

    printf("test_kmod_dlm_xnode: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
