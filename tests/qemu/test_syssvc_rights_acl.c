/*
 * test_syssvc_rights_acl.c - $GRANTID / $REVOKID change an ACL access decision
 * (vms-7d5a over vms-d404).
 *
 * The process rights list is executive state. On the writable real-VAX ODS-2
 * fixture (VDA0:) the privileged parent creates [OVMXDIR]RGTF.DAT, owned by [1,4]
 * with no world access, carrying the ACE (IDENTIFIER=%X80012345,ACCESS=READ). An
 * unprivileged child -- UIC [100,100] from its real credentials -- reports its VMS
 * pid and then opens the file by file ID whenever the parent asks:
 *
 *   - before anything is granted the child is refused (it holds no %X80012345);
 *   - the parent's $GRANTID of the identifier TO THE CHILD (by pid) answers
 *     SS$_WASCLR, a second one SS$_WASSET -- and the child now opens the file;
 *   - $REVOKID answers SS$_WASSET, then SS$_WASCLR -- and the child is refused again;
 *   - the child's own $GRANTID (no CMKRNL) is SS$_NOPRIV and changes nothing;
 *   - $GRANTID by an unknown name is SS$_NOSUCHID.
 *
 * Statuses are those of the OpenVMS V7.3 / Alpha V8.4 probe
 * (docs/oracle/semantics/rights). The file is deleted again. No /dev/vms -> SKIP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "chpdef.h"
#include "armdef.h"
#include "iledef.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"
#define OVMXDIR_FID_NUM 11u
#define UNPRIV_GID 100
#define UNPRIV_UID 100
#define TEST_ID 0x80012345u

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

/* --- the unprivileged child: answers commands from the parent -------------- */
static int run_child(int rfd, int wfd, uint16_t fid)
{
    uint32_t st = 0, chan = 0;
    struct vms_procinfo info;
    char cmd;

    memset(&info, 0, sizeof(info));
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL) || !(vms_kif_getjpi_self(&info) & 1) ||
        !(vms_kif_acp_assign(ODS2_UNIT, &chan) & 1)) {
        st = 0;
        (void)!write(wfd, &st, sizeof(st));
        return 1;
    }
    (void)!write(wfd, &info.vms_pid, sizeof(info.vms_pid));
    while (read(rfd, &cmd, 1) == 1 && cmd != 'q') {
        if (cmd == 'o') {
            struct vms_acp_access_args a;
            memset(&a, 0, sizeof(a));
            a.chan = chan;
            a.fidmode = 1;
            a.fid_num = fid; a.fid_seq = 1;
            st = vms_kif_acp_access(&a);
            if (st & 1)
                (void)vms_kif_acp_deaccess(chan);
        } else {
            uint32_t idq[2] = { TEST_ID, 0 };
            st = sys$grantid(NULL, NULL, idq, NULL, NULL, 0);
        }
        (void)!write(wfd, &st, sizeof(st));
    }
    (void)vms_kif_dassgn(chan);
    return 0;
}

static int to_child = -1, from_child = -1;
static uint32_t ask(char cmd)
{
    uint32_t st = 0;
    if (write(to_child, &cmd, 1) != 1 || read(from_child, &st, sizeof(st)) != (ssize_t)sizeof(st))
        return 0;
    return st;
}

int main(int argc, char **argv)
{
    uint32_t st, chan = 0, child_pid = 0, prv = 0;
    uint16_t fid = 0;
    int p2c[2], c2p[2];
    pid_t pid;

    if (argc >= 5 && strcmp(argv[1], "--child") == 0)
        return run_child(atoi(argv[2]), atoi(argv[3]), (uint16_t)atoi(argv[4]));

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_rights_acl: $GRANTID / $REVOKID change an ACL decision ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_rights_acl: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    check(vms_kif_acp_mount(ODS2_UNIT) & 1, "$MOUNT of the writable ODS-2 " ODS2_UNIT);
    check((vms_kif_acp_assign(ODS2_UNIT, &chan) & 1) && chan, "$ASSIGN a file-class channel");

    {
        struct vms_acp_fileop_args f;
        uint8_t ace[12] = { 12, 1, 0, 0, 1, 0, 0, 0,
                            (uint8_t)TEST_ID, (uint8_t)(TEST_ID >> 8),
                            (uint8_t)(TEST_ID >> 16), (uint8_t)(TEST_ID >> 24) };
        memset(&f, 0, sizeof(f));
        f.chan = chan;
        f.func = VMS_ACP_FOP_CREATE;
        f.modifiers = VMS_ACP_M_CREATE;
        f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1;
        f.version = 1;
        f.attr_ctl = VMS_ACP_ATTR_PROT | VMS_ACP_ATTR_OWNER;
        f.attr.fileprot = 0xFF00;              /* S:RWED,O:RWED,G,W */
        f.attr.uic_group = 1; f.attr.uic_member = 4;
        strncpy(f.name, "RGTF.DAT", VMS_ACP_NAME_SIZE - 1);
        st = vms_kif_acp_fileop(&f);
        fid = f.fid_num;
        check(st & 1, "create RGTF.DAT [1,4] (S:RWED,O:RWED,G,W)");
        memset(&f, 0, sizeof(f));
        f.chan = chan;
        f.func = VMS_ACP_FOP_MODIFY;
        f.fidmode = 1;
        f.fid_num = fid; f.fid_seq = 1;
        f.acl_op = VMS_ACP_ACL_ADD;
        f.acl_len = sizeof(ace);
        f.acl_buf = (uint64_t)(uintptr_t)ace;
        check(vms_kif_acp_fileop(&f) & 1, "RGTF.DAT gets (IDENTIFIER=%X80012345,ACCESS=READ)");
    }

    if (pipe(p2c) < 0 || pipe(c2p) < 0) {
        check(0, "pipes for the child");
        return 1;
    }
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        char rfd[16], wfd[16], fs[16];
        close(p2c[1]); close(c2p[0]);
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            _exit(1);
        snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
        snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
        snprintf(fs, sizeof(fs), "%u", fid);
        execl(argv[0], argv[0], "--child", rfd, wfd, fs, (char *)NULL);
        _exit(1);
    }
    close(p2c[0]); close(c2p[1]);
    to_child = p2c[1]; from_child = c2p[0];
    check(read(from_child, &child_pid, sizeof(child_pid)) == (ssize_t)sizeof(child_pid) && child_pid,
          "the [100,100] child is registered and reports its VMS pid");

    check(ask('o') == SS$_NOPRIV, "before any grant the child is refused RGTF.DAT");

    {
        uint32_t idq[2] = { TEST_ID, 0 };
        st = sys$grantid(&child_pid, NULL, idq, NULL, &prv, 0);
        check(st == SS$_WASCLR, "$GRANTID %X80012345 to the child is SS$_WASCLR (newly granted)");
        st = sys$grantid(&child_pid, NULL, idq, NULL, &prv, 0);
        check(st == SS$_WASSET, "a second $GRANTID is SS$_WASSET (already held)");
        /* negctl: acp-rights-list-not-consulted */
        check(ask('o') & 1, "holding %X80012345 the child opens RGTF.DAT: the grant changed the ACL decision");

        st = sys$revokid(&child_pid, NULL, idq, NULL, &prv, 0);
        check(st == SS$_WASSET, "$REVOKID is SS$_WASSET (was held)");
        st = sys$revokid(&child_pid, NULL, idq, NULL, &prv, 0);
        check(st == SS$_WASCLR, "a second $REVOKID is SS$_WASCLR");
        check(ask('o') == SS$_NOPRIV, "after the revoke the child is refused again");

        /* negctl: rights-grant-cmkrnl-not-checked */
        check(ask('g') == SS$_NOPRIV, "the child's own $GRANTID (no CMKRNL) is SS$_NOPRIV");
        check(ask('o') == SS$_NOPRIV, "...and changes nothing: still refused");
    }
    {
        static char nm[] = "OVMX_NO_SUCH_IDENT";
        struct dsc$descriptor_s nd = { sizeof(nm) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, nm };
        check(sys$grantid(&child_pid, NULL, NULL, &nd, NULL, 0) == SS$_NOSUCHID,
              "$GRANTID by an unknown name is SS$_NOSUCHID");
    }

    /* $CHKPRO with the calling process as subject (vms-d404): its UIC and rights
     * list are the executive's; CHP$_PRIV 0 sets its privileges aside. The object
     * is [1,4] with no access for any category and the ACE
     * (IDENTIFIER=%X80012345,ACCESS=READ): only holding the identifier grants. */
    {
        uint32_t acc = ARM$M_READ, own = (1u << 16) | 4u, prot = 0xFFFF, nopriv[2] = { 0, 0 };
        uint32_t idq[2] = { TEST_ID, 0 };
        uint8_t ace[12] = { 12, 1, 0, 0, ARM$M_READ, 0, 0, 0,
                            (uint8_t)TEST_ID, (uint8_t)(TEST_ID >> 8),
                            (uint8_t)(TEST_ID >> 16), (uint8_t)(TEST_ID >> 24) };
        ILE3 it[6];

        memset(it, 0, sizeof(it));
        it[0].ile3$w_length = 4;  it[0].ile3$w_code = CHP$_ACCESS; it[0].ile3$ps_bufaddr = &acc;
        it[1].ile3$w_length = 4;  it[1].ile3$w_code = CHP$_OWNER;  it[1].ile3$ps_bufaddr = &own;
        it[2].ile3$w_length = 4;  it[2].ile3$w_code = CHP$_PROT;   it[2].ile3$ps_bufaddr = &prot;
        it[3].ile3$w_length = 8;  it[3].ile3$w_code = CHP$_PRIV;   it[3].ile3$ps_bufaddr = nopriv;
        it[4].ile3$w_length = 12; it[4].ile3$w_code = CHP$_ACL;    it[4].ile3$ps_bufaddr = ace;
        (void)sys$revokid(NULL, NULL, idq, NULL, NULL, 0);
        check(sys$chkpro(it, NULL, NULL) == SS$_NOPRIV,
              "$CHKPRO: the caller, not holding %X80012345, is refused the object");
        check(sys$grantid(NULL, NULL, idq, NULL, NULL, 0) == SS$_WASCLR,
              "$GRANTID %X80012345 to the caller itself");
        /* negctl: chkpro-acl-ignored */
        /* negctl: chkpro-self-rights-ignored */
        check(sys$chkpro(it, NULL, NULL) == SS$_NORMAL,
              "$CHKPRO: holding %X80012345 the caller is granted read by the ACE");
        check(sys$revokid(NULL, NULL, idq, NULL, NULL, 0) == SS$_WASSET,
              "$REVOKID %X80012345 from the caller (restore)");
    }

    (void)!write(to_child, "q", 1);
    waitpid(pid, NULL, 0);
    {
        struct vms_acp_fileop_args f;
        memset(&f, 0, sizeof(f));
        f.chan = chan;
        f.func = VMS_ACP_FOP_DELETE;
        f.modifiers = VMS_ACP_M_DELETE;
        f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1;
        f.version = 1;
        strncpy(f.name, "RGTF.DAT", VMS_ACP_NAME_SIZE - 1);
        check(vms_kif_acp_fileop(&f) & 1, "delete RGTF.DAT (restore)");
    }
    (void)vms_kif_dassgn(chan);
    (void)vms_kif_acp_dmount(ODS2_UNIT);
    printf("=== test_syssvc_rights_acl: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
