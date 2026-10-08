/*
 * test_syssvc_privilege_enforce.c - the executive REFUSES without, and GRANTS with,
 * the privileges VMS gates object access on (vms-5b8 follow-on, vms-44a).
 *
 * One root test process (it holds SETPRV) drops itself to the unprivileged UIC
 * [100,100] with VMS_IOCTL_SETIDENT and then switches single privileges on and off
 * through $SETPRV, observing the executive's verdict each time:
 *
 *   TMPMBX  $CREMBX (temporary) is SS$_NOPRIV without it, succeeds with it.
 *   PRMMBX  $CREMBX (permanent) is SS$_NOPRIV without it, succeeds with it.
 *   NETMBX  $ASSIGN of the DECnet device _NET: is SS$_NOPRIV without it, a channel with it.
 *   READALL IO$_ACCESS (read) of SYSUAF.DAT (S:RWE,O:RWE,G:none,W:none) is
 *           SS$_NOPRIV for [100,100] and granted with READALL alone.
 *   GRPPRV  [100,100] reads a S:RWED file owned by [100,1] (its own group) with
 *           GRPPRV, not one owned by [200,1], nor a [100,1] file with S none.
 *   BYPASS  the same open is granted with BYPASS alone.
 *   SYSPRV  the same open is granted with SYSPRV alone (the accessor qualifies for
 *           the SYSTEM protection category).
 *
 * After each grant the privilege is switched off again and the refusal re-checked, so
 * every verdict is decided by exactly one privilege bit. No /dev/vms -> honest SKIP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define SYSVOL_UNIT "VDA300:"

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

static uint32_t resolve_did(uint32_t chan, uint16_t parent, const char *name)
{
    struct vms_acp_access_args a;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = parent;
    a.did_seq = parent ? 1 : 0;
    strncpy(a.name, name, VMS_ACP_NAME_SIZE - 1);
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_access(&a)))
        return 0;
    {
        uint32_t fn = a.fid_num;
        (void)vms_kif_acp_deaccess(chan);
        return fn;
    }
}

static uint32_t open_sysuaf(uint32_t chan, uint16_t sysexe)
{
    struct vms_acp_access_args a;
    uint32_t st;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = sysexe;
    a.did_seq = 1;
    strncpy(a.name, "SYSUAF.DAT", VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    (void)vms_kif_acp_deaccess(chan);
    return st;
}

/* A file in [SYS0.SYSCOMMON.SYSEXE] with the given owner and protection. */
static uint32_t mkfile(uint32_t chan, uint16_t dir, const char *name, uint16_t g, uint16_t m,
                       uint16_t prot)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_CREATE;
    f.modifiers = VMS_ACP_M_CREATE;
    f.did_num = dir; f.did_seq = 1;
    f.version = 1;
    f.attr_ctl = VMS_ACP_ATTR_PROT | VMS_ACP_ATTR_OWNER;
    f.attr.fileprot = prot;
    f.attr.uic_group = g; f.attr.uic_member = m;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

static uint32_t rmfile(uint32_t chan, uint16_t dir, const char *name)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_DELETE;
    f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = dir; f.did_seq = 1;
    f.version = 1;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

static uint32_t openf(uint32_t chan, uint16_t dir, const char *name)
{
    struct vms_acp_access_args a;
    uint32_t st;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = dir;
    a.did_seq = 1;
    strncpy(a.name, name, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    if (st & 1)
        (void)vms_kif_acp_deaccess(chan);
    return st;
}

static void priv(uint64_t bit, int on)
{
    uint64_t prev = 0;
    (void)vms_kif_setprv(bit, on, 0, &prev);
}

static uint32_t crembx(uint32_t permanent)
{
    uint32_t chan = 0, unit = 0;
    char dev[64];
    uint32_t st = vms_kif_mbx_create(permanent, 0, 0, &chan, &unit, dev, sizeof(dev));
    if ($VMS_STATUS_SUCCESS(st))
        (void)vms_kif_dassgn((uint16_t)chan);
    return st;
}

int main(void)
{
    uint32_t st, chan = 0;
    uint16_t sys0, syscommon, sysexe;
    const uint64_t base = PRV$M_SETPRV;

    printf("=== test_syssvc_privilege_enforce: mailbox + file-access privileges ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_privilege_enforce: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(SYSVOL_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the generated system-disk " SYSVOL_UNIT);
    st = vms_kif_acp_assign(SYSVOL_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel");
    sys0 = (uint16_t)resolve_did(chan, 0, "SYS0.DIR");
    syscommon = (uint16_t)resolve_did(chan, sys0, "SYSCOMMON.DIR");
    sysexe = (uint16_t)resolve_did(chan, syscommon, "SYSEXE.DIR");
    check(sys0 && syscommon && sysexe, "walk to [SYS0.SYSCOMMON.SYSEXE] as the privileged parent");

    /* GRPPRV fixtures, made while still privileged: S:RWED only (0xFFF0) owned by
     * [100,1] (decimal group 100: the dropped identity's own group) and by [200,1]; and O:RWED only
     * (0xFF0F) owned by [100,1]. */
    check(mkfile(chan, sysexe, "GRPT1.DAT", 100, 1, 0xFFF0) & 1, "create GRPT1.DAT [100,1] (S:RWED,O,G,W)");
    check(mkfile(chan, sysexe, "GRPT2.DAT", 200, 1, 0xFFF0) & 1, "create GRPT2.DAT [200,1] (S:RWED,O,G,W)");
    check(mkfile(chan, sysexe, "GRPT3.DAT", 100, 1, 0xFF0F) & 1, "create GRPT3.DAT [100,1] (S,O:RWED,G,W)");

    /* Drop to an unprivileged UIC; keep only SETPRV so single bits can be toggled. */
    st = vms_kif_setident("PRIVT", (100u << 16) | 100u, base);
    check($VMS_STATUS_SUCCESS(st), "SETIDENT drops this process to [100,100] holding only SETPRV");

    /* --- TMPMBX / PRMMBX ---------------------------------------------------- */
    priv(PRV$M_TMPMBX | PRV$M_PRMMBX, 0);
    /* negctl: mbx-tmpmbx-check-removed */
    check(crembx(0) == SS$_NOPRIV, "$CREMBX (temporary) without TMPMBX is SS$_NOPRIV");
    /* negctl: mbx-prmmbx-check-removed */
    check(crembx(1) == SS$_NOPRIV, "$CREMBX (permanent) without PRMMBX is SS$_NOPRIV");
    priv(PRV$M_TMPMBX, 1);
    check($VMS_STATUS_SUCCESS(crembx(0)), "$CREMBX (temporary) with TMPMBX succeeds");
    check(crembx(1) == SS$_NOPRIV, "TMPMBX alone does not permit a PERMANENT mailbox");
    priv(PRV$M_TMPMBX, 0);
    priv(PRV$M_PRMMBX, 1);
    check($VMS_STATUS_SUCCESS(crembx(1)), "$CREMBX (permanent) with PRMMBX succeeds");
    priv(PRV$M_PRMMBX, 0);

    /* --- NETMBX: a channel to the DECnet network device -------------------------- */
    /* The device exists only on a system with a NIC (the executive enters _NET: on the
     * primary ETH0:). The x86 harness has one; the Alpha harness has none. Without the
     * device there is nothing to refuse, so the section is skipped LOUDLY there -- it is
     * not counted as a pass, and the x86 run (where the negctl bites) is the proof. */
    priv(PRV$M_NETMBX, 1);
    {
        uint32_t nchan = 0;
        uint32_t on = vms_kif_assign("_NET:", &nchan);
        if (on == SS$_NOSUCHDEV) {
            printf("  SKIP: no _NET: device on this system (no NIC): the NETMBX section is not exercised here\n");
        } else {
            check($VMS_STATUS_SUCCESS(on) && nchan != 0,
                  "$ASSIGN _NET: with NETMBX yields a channel");
            if ($VMS_STATUS_SUCCESS(on))
                (void)vms_kif_dassgn(nchan);
            priv(PRV$M_NETMBX, 0);
            nchan = 0;
            /* negctl: net-assign-netmbx-check-removed */
            check(vms_kif_assign("_NET:", &nchan) == SS$_NOPRIV,
                  "$ASSIGN _NET: without NETMBX is SS$_NOPRIV");
        }
    }
    priv(PRV$M_NETMBX, 0);

    /* --- READALL / BYPASS / SYSPRV on a file the UIC has no category for ----- */
    check(open_sysuaf(chan, sysexe) == SS$_NOPRIV,
          "[100,100] with no file privilege is REFUSED SYSUAF.DAT (control)");
    priv(PRV$M_READALL, 1);
    /* negctl: acp-readall-ignored */
    check($VMS_STATUS_SUCCESS(open_sysuaf(chan, sysexe)), "READALL alone grants the read");
    priv(PRV$M_READALL, 0);
    check(open_sysuaf(chan, sysexe) == SS$_NOPRIV, "READALL off again: refused");
    priv(PRV$M_BYPASS, 1);
    /* negctl: acp-bypass-ignored */
    check($VMS_STATUS_SUCCESS(open_sysuaf(chan, sysexe)), "BYPASS alone grants the read");
    priv(PRV$M_BYPASS, 0);
    check(open_sysuaf(chan, sysexe) == SS$_NOPRIV, "BYPASS off again: refused");
    priv(PRV$M_SYSPRV, 1);
    /* negctl: acp-sysprv-ignored */
    check($VMS_STATUS_SUCCESS(open_sysuaf(chan, sysexe)),
          "SYSPRV alone grants the read (SYSTEM protection category)");
    priv(PRV$M_SYSPRV, 0);
    check(open_sysuaf(chan, sysexe) == SS$_NOPRIV, "SYSPRV off again: refused");

    /* --- GRPPRV: the system category for its own group's files (V7.3 oracle) --- */
    check(openf(chan, sysexe, "GRPT1.DAT") == SS$_NOPRIV,
          "[100,100] without GRPPRV is refused GRPT1.DAT (control)");
    priv(PRV$M_GRPPRV, 1);
    /* negctl: acp-grpprv-ignored */
    check(openf(chan, sysexe, "GRPT1.DAT") & 1,
          "GRPPRV reads its own group's GRPT1.DAT through the system field");
    check(openf(chan, sysexe, "GRPT2.DAT") == SS$_NOPRIV,
          "GRPPRV does not reach GRPT2.DAT, owned by another group");
    check(openf(chan, sysexe, "GRPT3.DAT") == SS$_NOPRIV,
          "GRPPRV grants only what the system field allows (GRPT3.DAT: S none)");
    priv(PRV$M_GRPPRV, 0);
    priv(PRV$M_BYPASS, 1);
    check((rmfile(chan, sysexe, "GRPT1.DAT") & 1) && (rmfile(chan, sysexe, "GRPT2.DAT") & 1) &&
          (rmfile(chan, sysexe, "GRPT3.DAT") & 1), "delete the GRPPRV fixtures (restore)");
    priv(PRV$M_BYPASS, 0);

    (void)vms_kif_dassgn((uint16_t)chan);
    printf("=== test_syssvc_privilege_enforce: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
