/*
 * test_syssvc_acp_delete_prot.c - the Files-11 ACP enforces the DELETE (D) protection bit.
 *
 * OpenVMS Guide to System Security (file class): write access lets a process change a
 * file's contents but not delete it; removing or renaming a file's name needs delete
 * access. DCL DELETE: "Requires delete (D) access to the file and write (W) access to
 * the parent directory". So, for the unprivileged UIC [100,100]:
 *
 *   - with SYSPRV (the system category; [SYS0.SYSCOMMON.SYSEXE] is then writable), a
 *     file whose every field lacks D (S:RWE,O:RWE) is NOT deleted and NOT renamed --
 *     SS$_NOPRIV, though write is granted; the same file with S:RWED,O:RWED is deleted;
 *   - with no privilege, a file it owns with O:RWED is NOT deleted from a directory
 *     it may not write (the world field of SYSEXE) -- SS$_NOPRIV.
 *
 * Fixtures are made and removed while privileged (BYPASS). No /dev/vms -> SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "ssdef.h"
#include "prvdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define SYSVOL_UNIT "VDA300:"

/* Protection words (a set bit DENIES; fields S,O,G,W from bit 0; R=1 W=2 E=4 D=8). */
#define PROT_SO_RWE   0xFF88u   /* S:RWE, O:RWE, G:, W: -- no D anywhere */
#define PROT_SO_RWED  0xFF00u   /* S:RWED, O:RWED, G:, W: */

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

static uint32_t mkfile(uint32_t chan, uint16_t dir, const char *name, uint16_t prot)
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
    f.attr.uic_group = 100; f.attr.uic_member = 100;
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

static uint32_t mvfile(uint32_t chan, uint16_t dir, const char *name, const char *to)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_MODIFY;
    f.modifiers = VMS_ACP_M_MOVE;
    f.did_num = dir; f.did_seq = 1;
    f.version = 1;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    strncpy(f.new_name, to, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

static uint32_t openw(uint32_t chan, uint16_t dir, const char *name)
{
    struct vms_acp_access_args a;
    uint32_t st;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = dir;
    a.did_seq = 1;
    a.acctl = VMS_ACP_ACCTL_WRITE;
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

int main(void)
{
    uint32_t st, chan = 0;
    uint16_t sys0, syscommon, sysexe;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_acp_delete_prot: the DELETE protection bit ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_acp_delete_prot: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    ovmx_become_login();

    st = vms_kif_acp_mount(SYSVOL_UNIT);
    check($VMS_STATUS_SUCCESS(st), "$MOUNT of the generated system-disk " SYSVOL_UNIT);
    st = vms_kif_acp_assign(SYSVOL_UNIT, &chan);
    check($VMS_STATUS_SUCCESS(st) && chan != 0, "$ASSIGN a file-class channel");
    sys0 = (uint16_t)resolve_did(chan, 0, "SYS0.DIR");
    syscommon = (uint16_t)resolve_did(chan, sys0, "SYSCOMMON.DIR");
    sysexe = (uint16_t)resolve_did(chan, syscommon, "SYSEXE.DIR");
    check(sys0 && syscommon && sysexe, "walk to [SYS0.SYSCOMMON.SYSEXE]");

    check(mkfile(chan, sysexe, "DELNOD.DAT", PROT_SO_RWE) & 1,
          "create DELNOD.DAT [100,100] (S:RWE,O:RWE,G,W)");
    check(mkfile(chan, sysexe, "DELOK.DAT", PROT_SO_RWED) & 1,
          "create DELOK.DAT [100,100] (S:RWED,O:RWED,G,W)");
    check(mkfile(chan, sysexe, "DIRW.DAT", PROT_SO_RWED) & 1,
          "create DIRW.DAT [100,100] (S:RWED,O:RWED,G,W)");

    st = vms_kif_setident("DELPT", (100u << 16) | 100u, PRV$M_SETPRV);
    check($VMS_STATUS_SUCCESS(st), "SETIDENT drops this process to [100,100] holding only SETPRV");

    /* --- the file's own D right (SYSPRV: the directory is writable) ---------- */
    priv(PRV$M_SYSPRV, 1);
    check(openw(chan, sysexe, "DELNOD.DAT") & 1,
          "SYSPRV: DELNOD.DAT is open for WRITE (write is granted, delete is not)");
    /* negctl: acp-delete-bit-not-wanted */
    check(rmfile(chan, sysexe, "DELNOD.DAT") == SS$_NOPRIV,
          "deleting a file whose protection grants no D is SS$_NOPRIV, write notwithstanding");
    check(mvfile(chan, sysexe, "DELNOD.DAT", "DELNOD2.DAT") == SS$_NOPRIV,
          "renaming a file whose protection grants no D is SS$_NOPRIV");
    check($VMS_STATUS_SUCCESS(rmfile(chan, sysexe, "DELOK.DAT")),
          "the same delete of DELOK.DAT (S:RWED) succeeds");
    priv(PRV$M_SYSPRV, 0);

    /* --- the directory's W right (no privilege: SYSEXE's world field) --------- */
    /* negctl: acp-delete-dir-write-not-checked */
    check(rmfile(chan, sysexe, "DIRW.DAT") == SS$_NOPRIV,
          "deleting its own O:RWED file from a directory it may not write is SS$_NOPRIV");

    /* --- restore ---------------------------------------------------------------- */
    /* Unasserted, so a negative control that let a delete or rename through
     * reddens only the assertion it names. */
    priv(PRV$M_BYPASS, 1);
    (void)rmfile(chan, sysexe, "DELNOD.DAT");
    (void)rmfile(chan, sysexe, "DELNOD2.DAT");
    (void)rmfile(chan, sysexe, "DELOK.DAT");
    (void)rmfile(chan, sysexe, "DIRW.DAT");
    priv(PRV$M_BYPASS, 0);

    printf("=== test_syssvc_acp_delete_prot: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
