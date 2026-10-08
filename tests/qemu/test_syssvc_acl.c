/*
 * test_syssvc_acl.c - access control lists on Files-11 files, enforced by the
 * executive ACP (vms-d404).
 *
 * Every expectation here is what OpenVMS VAX V7.3 was observed to do
 * (docs/oracle/vax73-acl.md). On the writable real-VAX ODS-2 fixture (VDA0:)
 * the privileged parent creates files in [OVMXDIR], sets their protection and
 * ACL through IO$_MODIFY, and a re-exec'd UNPRIVILEGED child -- UIC [100,100]
 * from its real credentials, no file privileges -- opens them by file ID:
 *
 *   - an ACE naming the child grants read the protection code denies (F1);
 *   - an ACE naming the child that grants nothing denies read world allows (F2);
 *   - the owner keeps the protection code's owner field past a denying ACE (F3);
 *   - the first matching ACE decides ([100,100] READ before [100,*] NONE, and
 *     the reverse) (F4);
 *   - IDENTIFIER=* (0xFFFFFFFF) matches every process (F5);
 *   - an ACE carrying OPTIONS=DEFAULT is not used for the file itself (F6);
 *   - changing an ACL needs CONTROL: a non-owner is refused SS$_NOPRIV until an
 *     ACE grants it CONTROL (F1).
 *
 * The ACL edits themselves: ADD puts the ACE first and replaces an ACE for the
 * same identifier, DEL of an absent ACE is SS$_NOENTRY, DELETEALL keeps
 * PROTECTED ACEs, READ of an empty ACL is SS$_ACLEMPTY. $PARSE_ACL/$FORMAT_ACL
 * turn the oracle's text into these bytes and back, and $SET_SECURITY /
 * $GET_SECURITY reach the same ACL by file name. DCL SET ACL, SHOW ACL and
 * DIRECTORY/ACL run against it last.
 *
 * Every file it creates is deleted again. No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <poll.h>
#include <signal.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "ossdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"
#define OVMXDIR_FID_NUM 11u
#define UNPRIV_GID 100
#define UNPRIV_UID 100
#define CHILD_UIC 0x00640064u           /* [100,100] (decimal 100 = octal 144) */
#define DCL_PATH "/bin/DCL.EXE"

static int pass, fail;

/* How [1,4] prints: [SYSTEM] where the rights database names it (a system disk
 * with RIGHTSLIST.DAT), else [1,4] -- $FORMAT_ACL names what $IDTOASC names. */
static char sys_uic_txt[48];
static void init_sys_uic_txt(void)
{
    char name[40];
    uint16_t nl = 0;
    uint32_t resid = 0, attr = 0;
    struct dsc$descriptor_s nd = { sizeof(name) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
    if ((sys$idtoasc(0x00010004u, &nl, &nd, &resid, &attr, NULL) & 1) && nl) {
        name[nl] = '\0';
        snprintf(sys_uic_txt, sizeof(sys_uic_txt), "[%s]", name);
    } else {
        snprintf(sys_uic_txt, sizeof(sys_uic_txt), "[1,4]");
    }
}
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
}

/* one identifier ACE: size 8 + 4n, type ACE$C_KEYID, flags, access, ids */
static uint32_t mkace(uint8_t *a, uint16_t flags, uint32_t access, uint32_t id1, uint32_t id2)
{
    uint32_t n = id2 ? 2u : 1u, k, ids[2] = { id1, id2 };
    memset(a, 0, 16);
    a[0] = (uint8_t)(8u + 4u * n);
    a[1] = 1;
    a[2] = (uint8_t)flags; a[3] = (uint8_t)(flags >> 8);
    a[4] = (uint8_t)access; a[5] = (uint8_t)(access >> 8);
    for (k = 0; k < n; k++) {
        a[8 + 4 * k] = (uint8_t)ids[k];       a[9 + 4 * k] = (uint8_t)(ids[k] >> 8);
        a[10 + 4 * k] = (uint8_t)(ids[k] >> 16); a[11 + 4 * k] = (uint8_t)(ids[k] >> 24);
    }
    return a[0];
}

static uint32_t create_file(uint32_t chan, const char *name, uint16_t og, uint16_t om,
                            uint16_t prot, uint16_t *fid)
{
    struct vms_acp_fileop_args f;
    uint32_t st;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_CREATE;
    f.modifiers = VMS_ACP_M_CREATE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1;
    f.version = 1;
    f.attr_ctl = VMS_ACP_ATTR_PROT | VMS_ACP_ATTR_OWNER;
    f.attr.fileprot = prot;
    f.attr.uic_group = og; f.attr.uic_member = om;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    *fid = f.fid_num;
    return st;
}

static uint32_t delete_file(uint32_t chan, const char *name)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_DELETE;
    f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = OVMXDIR_FID_NUM; f.did_seq = 1;
    f.version = 1;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

static uint32_t aclop(uint32_t chan, uint16_t fid, uint32_t op, void *buf, uint32_t *len)
{
    struct vms_acp_fileop_args f;
    uint32_t st;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_MODIFY;
    f.fidmode = 1;
    f.fid_num = fid; f.fid_seq = 1;
    f.acl_op = op;
    f.acl_len = len ? *len : 0;
    f.acl_buf = (uint64_t)(uintptr_t)buf;
    st = vms_kif_acp_fileop(&f);
    if (len) *len = f.acl_len;
    return st;
}

static uint32_t add_ace(uint32_t chan, uint16_t fid, uint16_t flags, uint32_t access,
                        uint32_t id1, uint32_t id2)
{
    uint8_t a[16];
    uint32_t n = mkace(a, flags, access, id1, id2);
    return aclop(chan, fid, VMS_ACP_ACL_ADD, a, &n);
}

/* --- inheritance on create (vms-d404, docs/oracle/vax73-acl/default-propagation.txt) --- */
struct pfid { uint16_t num, seq; uint8_t rvn, nmx; };

/* IO$_CREATE `name` in directory `dir` with NO protection/owner attributes, so
 * the file system's own choice (previous version, DEFAULT_PROTECTION) applies.
 * version 0 = a new highest version. */
static uint32_t pcreate(uint32_t chan, const struct pfid *dir, const char *name, int is_dir,
                        uint16_t version, struct pfid *out)
{
    struct vms_acp_fileop_args f;
    uint32_t st;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_CREATE;
    f.modifiers = VMS_ACP_M_CREATE;
    f.did_num = dir->num; f.did_seq = dir->seq; f.did_rvn = dir->rvn; f.did_nmx = dir->nmx;
    f.version = version;
    if (is_dir)
        f.attr.filechar = 0x2000;          /* FH2$M_DIRECTORY */
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_fileop(&f);
    out->num = f.fid_num; out->seq = f.fid_seq; out->rvn = f.fid_rvn; out->nmx = f.fid_nmx;
    return st;
}

static uint32_t pdelete(uint32_t chan, const struct pfid *dir, const char *name, uint16_t version)
{
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan;
    f.func = VMS_ACP_FOP_DELETE;
    f.modifiers = VMS_ACP_M_DELETE;
    f.did_num = dir->num; f.did_seq = dir->seq; f.did_rvn = dir->rvn; f.did_nmx = dir->nmx;
    f.version = version;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    return vms_kif_acp_fileop(&f);
}

/* the file's protection word, read back over IO$_ACCESS by File ID */
static uint16_t pprot(uint32_t chan, uint16_t fid)
{
    struct vms_acp_access_args a;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.fidmode = 1;
    a.fid_num = fid; a.fid_seq = 1;
    if (!(vms_kif_acp_access(&a) & 1))
        return 0xDEAD;
    (void)vms_kif_acp_deaccess(chan);
    return (uint16_t)a.attr.fileprot;
}

/* the file's ACL equals `want` (len bytes) */
static int pacl_is(uint32_t chan, uint16_t fid, const uint8_t *want, uint32_t len)
{
    uint8_t got[512];
    uint32_t n = sizeof(got);
    memset(got, 0, sizeof(got));
    if (!(aclop(chan, fid, VMS_ACP_ACL_READ, got, &n) & 1) && len != 0)
        return 0;
    return n == len && (len == 0 || memcmp(got, want, len) == 0);
}

static uint32_t add_raw(uint32_t chan, uint16_t fid, const uint8_t *ace)
{
    uint32_t n = ace[0];
    return aclop(chan, fid, VMS_ACP_ACL_ADD, (void *)ace, &n);
}

#define ACEB(fl, acc, id) 12, 1, (uint8_t)(fl), (uint8_t)((fl) >> 8), (uint8_t)(acc), 0, 0, 0, \
    (uint8_t)(id), (uint8_t)((id) >> 8), (uint8_t)((id) >> 16), (uint8_t)((id) >> 24)
#define U200_201 0x00800081u               /* [200,201] */
#define U300_ANY 0x00C0FFFFu               /* [300,*] */
#define U200_ANY 0x0080FFFFu               /* [200,*] */
#define U1_4     0x00010004u               /* [1,4] */

/* --- the unprivileged child ------------------------------------------------ */

struct child_rep { uint32_t assign_st, op_st; };

/* mode 'r': IO$_ACCESS (read) by FID; mode 'a': add an ACE ([1,4] READ) by FID */
static int run_child(int wfd, char mode, uint16_t fid)
{
    struct child_rep rep = { 0, 0 };
    uint32_t chan = 0;

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }
    rep.assign_st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    if (rep.assign_st & 1) {
        if (mode == 'r') {
            struct vms_acp_access_args a;
            memset(&a, 0, sizeof(a));
            a.chan = chan;
            a.fidmode = 1;
            a.fid_num = fid; a.fid_seq = 1;
            rep.op_st = vms_kif_acp_access(&a);
            if (rep.op_st & 1)
                (void)vms_kif_acp_deaccess(chan);
        } else {
            rep.op_st = add_ace(chan, fid, 0, 1u, 0x00010004u, 0);
        }
        (void)vms_kif_dassgn(chan);
    }
    (void)!write(wfd, &rep, sizeof(rep));
    return 0;
}

/* Run the child as [100,100]; returns the status it observed (0 on harness failure). */
/* Reads from a child are BOUNDED: a broken executive (a lock that is never
 * released, an ACP request that never completes) must fail this suite, not hang
 * the guest and starve every suite after it. */
#define CHILD_WAIT_MS 60000
static ssize_t read_bounded(int fd, void *buf, size_t len, int *timed_out)
{
    struct pollfd pf = { fd, POLLIN, 0 };
    int r = poll(&pf, 1, CHILD_WAIT_MS);
    if (r <= 0) {
        *timed_out = 1;
        return -1;
    }
    return read(fd, buf, len);
}

static uint32_t as_child(const char *self, char mode, uint16_t fid)
{
    int p[2];
    pid_t pid;
    struct child_rep rep = { 0, 0 };
    if (pipe(p) < 0)
        return 0;
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        char wfd[16], fs[16], md[2] = { mode, 0 };
        close(p[0]);
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            _exit(1);
        snprintf(wfd, sizeof(wfd), "%d", p[1]);
        snprintf(fs, sizeof(fs), "%u", fid);
        execl(self, self, "--child", wfd, md, fs, (char *)NULL);
        _exit(1);
    }
    close(p[1]);
    if (pid > 0) {
        int to = 0;
        if (read_bounded(p[0], &rep, sizeof(rep), &to) != (ssize_t)sizeof(rep))
            rep.op_st = 0;
        if (to) {
            printf("  (the [100,100] child did not answer in %d s; killed)\n", CHILD_WAIT_MS / 1000);
            kill(pid, SIGKILL);
        }
        waitpid(pid, NULL, 0);
    }
    close(p[0]);
    return (rep.assign_st & 1) ? rep.op_st : 0;
}

/* --- DCL ------------------------------------------------------------------- */

static char *const dcl_env[] = { "PATH=/bin", NULL };

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    pid_t pid;
    size_t used = 0;

    out[0] = '\0';
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0)
        return -1;
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], 0); dup2(out_pipe[1], 1); dup2(out_pipe[1], 2);
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    (void)!write(in_pipe[1], script, strlen(script));
    close(in_pipe[1]);
    for (;;) {
        int to = 0;
        ssize_t n = read_bounded(out_pipe[0], out + used, outsz - 1 - used, &to);
        if (to) {
            printf("  (DCL did not finish in %d s; killed)\n", CHILD_WAIT_MS / 1000);
            kill(pid, SIGKILL);
            break;
        }
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    waitpid(pid, NULL, 0);
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t st, chan = 0, n;
    uint16_t f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0, f7 = 0;
    uint8_t acl[512], ace[16];

    if (argc >= 5 && strcmp(argv[1], "--child") == 0)
        return run_child(atoi(argv[2]), argv[3][0], (uint16_t)atoi(argv[4]));

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_acl: ACLs on ODS-2 files, enforced by the executive ACP ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_acl: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }
    init_sys_uic_txt();
    printf("  (identifier [1,4] prints as %s here)\n", sys_uic_txt);
    st = vms_kif_acp_mount(ODS2_UNIT);
    check(st & 1, "$MOUNT of the writable ODS-2 " ODS2_UNIT);
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check((st & 1) && chan, "$ASSIGN a file-class channel");

    /* S:RWED,O:RWED,G,W = 0xFF00; S:RWED,O:RWED,G,W:RE = 0xAF00 */
    check(create_file(chan, "ACLF1.DAT", 1, 4, 0xFF00, &f1) & 1, "create ACLF1.DAT [1,4] (S:RWED,O:RWED,G,W)");
    check(create_file(chan, "ACLF2.DAT", 1, 4, 0xAF00, &f2) & 1, "create ACLF2.DAT [1,4] (S:RWED,O:RWED,G,W:RE)");
    check(create_file(chan, "ACLF3.DAT", 100, 100, 0xFF00, &f3) & 1, "create ACLF3.DAT owned by [100,100] (O:RWED)");
    check(create_file(chan, "ACLF4.DAT", 1, 4, 0xFF00, &f4) & 1, "create ACLF4.DAT [1,4] (W none)");
    check(create_file(chan, "ACLF5.DAT", 1, 4, 0xAF00, &f5) & 1, "create ACLF5.DAT [1,4] (W:RE)");
    check(create_file(chan, "ACLF6.DAT", 1, 4, 0xFF00, &f6) & 1, "create ACLF6.DAT [1,4] (W none)");
    check(create_file(chan, "ACLF7.DAT", 1, 4, 0xFF00, &f7) & 1, "create ACLF7.DAT [1,4] for the edit checks");

    /* -- the protection code alone (controls) -- */
    check(as_child(argv[0], 'r', f1) == SS$_NOPRIV, "control: [100,100] is refused ACLF1 (world denies read)");
    check(as_child(argv[0], 'r', f2) & 1, "control: [100,100] reads ACLF2 (world allows read)");

    /* -- F1: an ACE grants what the protection code denies -- */
    check(add_ace(chan, f1, 0, 1u, CHILD_UIC, 0) & 1, "ACLF1 gets (IDENTIFIER=[100,100],ACCESS=READ)");
    /* negctl: acp-acl-not-consulted */
    check(as_child(argv[0], 'r', f1) & 1, "F1: the ACE grants [100,100] read the protection code denies");

    /* -- F2: a matching ACE that grants nothing is final for world -- */
    check(add_ace(chan, f2, 0, 0u, CHILD_UIC, 0) & 1, "ACLF2 gets (IDENTIFIER=[100,100],ACCESS=NONE)");
    /* negctl: acp-acl-deny-falls-to-world */
    check(as_child(argv[0], 'r', f2) == SS$_NOPRIV, "F2: the matching NONE ACE denies the read world allows");

    /* -- F3: the owner keeps the owner field past a denying ACE -- */
    check(add_ace(chan, f3, 0, 0u, CHILD_UIC, 0) & 1, "ACLF3 gets (IDENTIFIER=[100,100],ACCESS=NONE)");
    check(as_child(argv[0], 'r', f3) & 1, "F3: the owner [100,100] still reads through its O:RWED");

    /* -- F4: the first matching ACE decides -- */
    check(add_ace(chan, f4, 0, 0u, 0x0064FFFFu, 0) & 1, "ACLF4 gets (IDENTIFIER=[100,*],ACCESS=NONE)");
    check(add_ace(chan, f4, 0, 1u, CHILD_UIC, 0) & 1, "...then (IDENTIFIER=[100,100],ACCESS=READ), which goes first");
    check(as_child(argv[0], 'r', f4) & 1, "F4: [100,100] READ, ahead of [100,*] NONE, grants");
    check(add_ace(chan, f4, 0, 0u, 0x0064FFFFu, 0) & 1, "re-adding [100,*] NONE moves it to the top");
    check(as_child(argv[0], 'r', f4) == SS$_NOPRIV, "F4: now [100,*] NONE matches first and denies");

    /* -- F5: IDENTIFIER=* matches every process -- */
    check(add_ace(chan, f5, 0, 0u, 0xFFFFFFFFu, 0) & 1, "ACLF5 gets (IDENTIFIER=*,ACCESS=NONE)");
    check(as_child(argv[0], 'r', f5) == SS$_NOPRIV, "F5: IDENTIFIER=* denies [100,100] the world read");

    /* -- F6: a DEFAULT ACE is not used for the file itself -- */
    check(add_ace(chan, f6, 0x0100, 1u, CHILD_UIC, 0) & 1, "ACLF6 gets (IDENTIFIER=[100,100],OPTIONS=DEFAULT,ACCESS=READ)");
    check(as_child(argv[0], 'r', f6) == SS$_NOPRIV, "F6: the DEFAULT ACE grants nothing on the file itself");

    /* -- CONTROL: changing the ACL -- */
    /* negctl: acp-acl-control-not-checked */
    check(as_child(argv[0], 'a', f1) == SS$_NOPRIV, "a non-owner without CONTROL may not change ACLF1's ACL");
    check(add_ace(chan, f1, 0, 0x11u, CHILD_UIC, 0) & 1, "ACLF1's [100,100] ACE becomes READ+CONTROL");
    check(as_child(argv[0], 'a', f1) & 1, "with CONTROL granted by an ACE, [100,100] adds an ACE to ACLF1");

    /* -- the edits, read back -- */
    n = sizeof(acl);
    st = aclop(chan, f7, VMS_ACP_ACL_READ, acl, &n);
    check(st == SS$_ACLEMPTY, "an ACL read of a file with no ACL is SS$_ACLEMPTY");
    check(add_ace(chan, f7, 0, 3u, 0x00010004u, 0) & 1, "add (IDENTIFIER=[1,4],ACCESS=READ+WRITE)");
    n = sizeof(acl);
    st = aclop(chan, f7, VMS_ACP_ACL_READ, acl, &n);
    mkace(ace, 0, 3u, 0x00010004u, 0);
    check((st & 1) && n == 12 && memcmp(acl, ace, 12) == 0,
          "the ACL reads back as the oracle's 12 bytes: 0C 01 0000 03000000 04000100");
    check(add_ace(chan, f7, 0x0200, 1u, 0x3FFFFFFFu, 0) & 1, "add (IDENTIFIER=[*,*],OPTIONS=PROTECTED,ACCESS=READ)");
    check(add_ace(chan, f7, 0, 0u, 0x00010004u, 0) & 1, "add [1,4] NONE: replaces the [1,4] ACE");
    n = sizeof(acl);
    st = aclop(chan, f7, VMS_ACP_ACL_READ, acl, &n);
    check((st & 1) && n == 24 && acl[8] == 4 && acl[10] == 1 && acl[4] == 0 && acl[20] == 0xFF,
          "two ACEs: [1,4] NONE first (replaced, moved to the top), then [*,*]");
    n = mkace(ace, 0, 0u, 0x00640064u, 0);
    check(aclop(chan, f7, VMS_ACP_ACL_DEL, ace, &n) == SS$_NOENTRY,
          "deleting an ACE that is not there is SS$_NOENTRY");
    n = 0;
    check(aclop(chan, f7, VMS_ACP_ACL_DELETEALL, NULL, &n) & 1, "delete the ACL");
    n = sizeof(acl);
    st = aclop(chan, f7, VMS_ACP_ACL_READ, acl, &n);
    /* negctl: acp-acl-deleteall-drops-protected */
    check((st & 1) && n == 12 && acl[2] == 0 && acl[3] == 2,
          "the PROTECTED ACE survives deleting the ACL");
    n = 0;
    check(aclop(chan, f7, VMS_ACP_ACL_PURGE, NULL, &n) & 1, "delete every ACE, protected too");
    n = sizeof(acl);
    check(aclop(chan, f7, VMS_ACP_ACL_READ, acl, &n) == SS$_ACLEMPTY, "the ACL is empty again");

    /* -- $PARSE_ACL / $FORMAT_ACL -- */
    {
        static char txt[] = "(identifier=[1,4], access=r+w)";
        static char bad[] = "(IDENTIFIER=[1,4],ACCESS=BOGUS)";
        struct dsc$descriptor_s td = { sizeof(txt) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, txt };
        struct dsc$descriptor_s bd2 = { sizeof(bad) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, bad };
        uint8_t bin[64];
        char out[128];
        struct dsc$descriptor_s bd = { sizeof(bin), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)bin };
        struct dsc$descriptor_s od = { sizeof(out), DSC$K_DTYPE_T, DSC$K_CLASS_S, out };
        uint16_t ep = 0, ol = 0;

        st = sys$parse_acl(&td, &bd, &ep, NULL, 0);
        mkace(ace, 0, 3u, 0x00010004u, 0);
        /* negctl: sys-parse-acl-drops-access */
        check((st & 1) && memcmp(bin, ace, 12) == 0, "$PARSE_ACL (identifier=[1,4], access=r+w) is the oracle's bytes");
        bd.dsc$w_length = bin[0];
        st = sys$format_acl(&bd, &ol, &od, NULL, NULL, NULL, NULL, NULL);
        {
            char want[96];
            snprintf(want, sizeof(want), "(IDENTIFIER=%s,ACCESS=READ+WRITE)", sys_uic_txt);
            check((st & 1) && ol == strlen(want) && memcmp(out, want, ol) == 0,
                  "$FORMAT_ACL prints (IDENTIFIER=<[1,4]>,ACCESS=READ+WRITE)");
        }
        bd.dsc$w_length = sizeof(bin);
        st = sys$parse_acl(&bd2, &bd, &ep, NULL, 0);
        check(st == SS$_IVACL && strncmp(bad + ep, "ACCESS=BOGUS)", 13) == 0,
              "$PARSE_ACL of ACCESS=BOGUS is SS$_IVACL, error position at ACCESS=BOGUS)");
    }

    /* -- $SET_SECURITY / $GET_SECURITY by name -- */
    {
        static char cls[] = "FILE", obj[] = "VDA0:[OVMXDIR]ACLF7.DAT";
        struct dsc$descriptor_s cd = { 4, DSC$K_DTYPE_T, DSC$K_CLASS_S, cls };
        struct dsc$descriptor_s od = { sizeof(obj) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, obj };
        struct { uint16_t len, code; void *buf; uint16_t *ret; } it[4];
        uint32_t ctx = 0, alen = 0, owner = 0, prot = 0;
        uint16_t rl = 0;

        mkace(ace, 0, 1u, CHILD_UIC, 0);
        it[0].len = 12; it[0].code = OSS$_ACL_ADD_ENTRY; it[0].buf = ace; it[0].ret = NULL;
        it[1].len = 0; it[1].code = 0;
        st = sys$set_security(&cd, &od, NULL, OSS$M_LOCAL, it, &ctx, NULL);
        check((st & 1) && ctx != 0, "$SET_SECURITY OSS$M_LOCAL holds an ACE in the context");
        n = sizeof(acl);
        /* negctl: rms-set-security-local-applied-early */
        check(aclop(chan, f7, VMS_ACP_ACL_READ, acl, &n) == SS$_ACLEMPTY,
              "...and the file's ACL is unchanged until the context is released");
        it[0].len = 0; it[0].code = 0;
        st = sys$set_security(NULL, NULL, NULL, OSS$M_RELCTX, it, &ctx, NULL);
        check(st & 1, "$SET_SECURITY OSS$M_RELCTX applies it");
        it[0].len = 4; it[0].code = OSS$_ACL_LENGTH; it[0].buf = &alen; it[0].ret = NULL;
        it[1].len = 4; it[1].code = OSS$_OWNER; it[1].buf = &owner; it[1].ret = NULL;
        it[2].len = 4; it[2].code = OSS$_PROTECTION; it[2].buf = &prot; it[2].ret = NULL;
        it[3].len = 0; it[3].code = 0;
        st = sys$get_security(&cd, &od, NULL, 0, it, NULL, NULL);
        check((st & 1) && alen == 12 && owner == 0x00010004u && prot == 0xFF00u,
              "$GET_SECURITY: ACL length 12, owner [1,4], protection 0xFF00");
        it[0].len = sizeof(acl); it[0].code = OSS$_ACL_READ; it[0].buf = acl; it[0].ret = &rl;
        it[1].len = 0; it[1].code = 0;
        st = sys$get_security(&cd, &od, NULL, 0, it, NULL, NULL);
        check((st & 1) && rl == 12 && memcmp(acl, ace, 12) == 0, "$GET_SECURITY OSS$_ACL_READ returns the ACE");
        check(as_child(argv[0], 'r', f7) & 1, "...and the ACE set by name grants [100,100] read");
    }

    /* -- DCL SET ACL / SHOW ACL / DIRECTORY/ACL -- */
    if (access(DCL_PATH, X_OK) == 0) {
        static char out[16384];
        run_dcl("$ SET ACL/ACL=(IDENTIFIER=[1,4],ACCESS=READ+WRITE) VDA0:[OVMXDIR]ACLF6.DAT\n"
                "$ SHOW ACL VDA0:[OVMXDIR]ACLF6.DAT\n"
                "$ SET ACL/DELETE VDA0:[OVMXDIR]ACLF6.DAT\n"
                "$ SHOW ACL VDA0:[OVMXDIR]ACLF6.DAT\n"
                "$ SET ACL/ACL=(IDENTIFIER=[1,4],ACCESS=BOGUS) VDA0:[OVMXDIR]ACLF6.DAT\n"
                "$ DIRECTORY/ACL VDA0:[OVMXDIR]ACLF7.DAT\n"
                "$ EXIT\n", out, sizeof(out));
        printf("%s", out);
        char want[96];
        snprintf(want, sizeof(want), "          (IDENTIFIER=%s,ACCESS=READ+WRITE)", sys_uic_txt);
        check(strstr(out, "Object type: FILE,  Object name: ") != NULL &&
              strstr(out, want) != NULL,
              "DCL SET ACL then SHOW ACL prints the ACE as VMS does");
        check(strstr(out, "%SYSTEM-W-ACLEMPTY, access control list is empty") != NULL,
              "after SET ACL/DELETE, SHOW ACL is %SYSTEM-W-ACLEMPTY");
        check(strstr(out, "%SET-F-SYNTAX, error parsing 'ACCESS=BOGUS)'") != NULL &&
              strstr(out, "-SYSTEM-F-IVACL, invalid access control list entry syntax") != NULL,
              "SET ACL of a bad ACE reports %SET-F-SYNTAX + -SYSTEM-F-IVACL");
        check(strstr(out, "ACLF7.DAT;1") != NULL &&
              strstr(out, "          (IDENTIFIER=[144,144],ACCESS=READ)") != NULL,
              "DIRECTORY/ACL lists the file and its ACE");
    } else {
        check(0, DCL_PATH " is present in the initramfs");
    }

    /* -- an ACL longer than the primary header (vms-a88c) --
     * VMS continues it in an extension header (docs/oracle/vax73-acl/ace-editing.txt,
     * BIG.TXT: 35 ACEs, 25 in the primary, 10 in segment 1). ACLF8 gets
     * (IDENTIFIER=[100,100],ACCESS=READ) first and 34 other ACEs after it; each
     * goes first, so the [100,100] ACE ends last -- in the extension header. */
    {
        uint16_t f8 = 0;
        uint8_t big[4096];
        uint32_t bn, g;
        int ok = 1;

        check(create_file(chan, "ACLF8.DAT", 1, 4, 0xFF00, &f8) & 1, "create ACLF8.DAT [1,4] (W none)");
        ok &= (int)(add_ace(chan, f8, 0, 1u, CHILD_UIC, 0) & 1);
        for (g = 0; g < 34; g++)
            ok &= (int)(add_ace(chan, f8, 0, 1u, ((200u + g) << 16) | 0xFFFFu, 0) & 1);
        /* negctl: acp-acl-spill-refused */
        check(ok, "35 ACEs are accepted (the list outgrows the primary header)");
        bn = sizeof(big);
        memset(big, 0, sizeof(big));
        check((aclop(chan, f8, VMS_ACP_ACL_READ, big, &bn) & 1) && bn == 35u * 12u &&
              big[8] == 0xFF && big[9] == 0xFF && big[10] == (uint8_t)233 &&
              big[34 * 12 + 8] == 0x64 && big[34 * 12 + 10] == 0x64,
              "reading the ACL returns all 35, newest first, [100,100] last");
        /* negctl: acp-acl-ext-not-matched */
        check(as_child(argv[0], 'r', f8) & 1,
              "the [100,100] ACE in the extension header grants [100,100] read");
        check(add_ace(chan, f8, 0x0000, 1u, ((200u + 40u) << 16) | 0xFFFFu, 0) & 1,
              "a 36th ACE goes first; the rest move down the chain");
        bn = sizeof(big);
        check((aclop(chan, f8, VMS_ACP_ACL_READ, big, &bn) & 1) && bn == 36u * 12u &&
              big[35 * 12 + 8] == 0x64,
              "36 ACEs read back, [100,100] still last");
        {
            uint8_t a[16];
            uint32_t n = mkace(a, 0, 1u, CHILD_UIC, 0);
            check(aclop(chan, f8, VMS_ACP_ACL_DEL, a, &n) & 1, "delete the [100,100] ACE");
        }
        bn = sizeof(big);
        check((aclop(chan, f8, VMS_ACP_ACL_READ, big, &bn) & 1) && bn == 35u * 12u,
              "35 ACEs remain after the delete");
        check(as_child(argv[0], 'r', f8) == SS$_NOPRIV, "without that ACE [100,100] is refused again");
        bn = 0;
        check(aclop(chan, f8, VMS_ACP_ACL_DELETEALL, NULL, &bn) & 1, "SET ACL/DELETE of the long ACL");
        bn = sizeof(big);
        check(aclop(chan, f8, VMS_ACP_ACL_READ, big, &bn) == SS$_ACLEMPTY,
              "the ACL is empty, the extension header released");
        ok = 1;
        for (g = 0; g < 30; g++)
            ok &= (int)(add_ace(chan, f8, 0, 1u, ((300u + g) << 16) | 0xFFFFu, 0) & 1);
        check(ok, "30 ACEs again (a fresh extension header)");
        check(delete_file(chan, "ACLF8.DAT") & 1, "delete ACLF8.DAT with its extension header");
    }

    /* -- inheritance on create (docs/oracle/vax73-acl/default-propagation.txt) --
     * [OVMXDIR]PROPD.DIR carries, in this order,
     *   (IDENTIFIER=[200,201],OPTIONS=DEFAULT,ACCESS=READ+WRITE)
     *   (IDENTIFIER=[300,*],ACCESS=READ)
     *   (IDENTIFIER=[200,*],OPTIONS=DEFAULT+NOPROPAGATE,ACCESS=EXECUTE)
     *   (DEFAULT_PROTECTION,SYSTEM:RWED,OWNER:RWED,GROUP:R,WORLD:R)
     * and files are created in it with no protection of their own. */
    {
        /* GROUP:R,WORLD:R -- unlike the process default %XFA00, so a file that took
         * the process default instead would read differently. */
        static const uint8_t dirdef[24] = { 24, 9, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0x10, 0, 0, 0,
                                            0x1E, 0, 0, 0, 0x1E, 0, 0, 0 };
        static const uint8_t a_def[12] = { ACEB(0x0100, 3, U200_201) };
        static const uint8_t a_300[12] = { ACEB(0, 1, U300_ANY) };
        static const uint8_t a_np[12]  = { ACEB(0x0900, 4, U200_ANY) };
        static const uint8_t a_ctl[12] = { ACEB(0, 0x10, U1_4) };
        static const uint8_t file_acl[24] = { ACEB(0, 3, U200_201), ACEB(0x0800, 4, U200_ANY) };
        static const uint8_t sub_acl[48] = { ACEB(0x0100, 3, U200_201), ACEB(0, 1, U300_ANY),
                                             24, 9, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0x10, 0, 0, 0,
                                             0x1E, 0, 0, 0, 0x1E, 0, 0, 0 };
        static const uint8_t b_acl[12] = { ACEB(0, 3, U200_201) };
        static const uint8_t v2_acl[24] = { ACEB(0, 0x10, U1_4), ACEB(0, 3, U200_201) };
        struct pfid ovmx = { OVMXDIR_FID_NUM, 1, 0, 0 }, pd, sub, fa, fb, fa2;
        int ok;

        check(pcreate(chan, &ovmx, "PROPD.DIR", 1, 1, &pd) & 1, "create [OVMXDIR]PROPD.DIR");
        ok  = (int)(add_raw(chan, pd.num, dirdef) & 1);
        ok &= (int)(add_raw(chan, pd.num, a_np) & 1);
        ok &= (int)(add_raw(chan, pd.num, a_300) & 1);
        ok &= (int)(add_raw(chan, pd.num, a_def) & 1);
        check(ok, "PROPD.DIR gets two DEFAULT ACEs, a plain ACE and a DEFAULT_PROTECTION ACE");

        check(pcreate(chan, &pd, "A.TXT", 0, 0, &fa) & 1, "create [.PROPD]A.TXT with no protection of its own");
        /* negctl: acp-acl-default-not-propagated */
        check(pacl_is(chan, fa.num, file_acl, sizeof(file_acl)),
              "A.TXT inherits the DEFAULT ACEs, DEFAULT cleared, NOPROPAGATE kept; not the plain ACE");
        /* negctl: acp-default-protection-ignored */
        check(pprot(chan, fa.num) == 0xEE00, "A.TXT's protection is the DEFAULT_PROTECTION ACE's (RWED,RWED,R,R)");

        check(pcreate(chan, &pd, "SUB.DIR", 1, 1, &sub) & 1, "create [.PROPD]SUB.DIR");
        check(pacl_is(chan, sub.num, sub_acl, sizeof(sub_acl)),
              "SUB.DIR inherits PROPD.DIR's ACL as it is, except the NOPROPAGATE ACE");
        check(pcreate(chan, &sub, "B.TXT", 0, 0, &fb) & 1, "create [.PROPD.SUB]B.TXT");
        check(pacl_is(chan, fb.num, b_acl, sizeof(b_acl)),
              "B.TXT inherits SUB.DIR's DEFAULT ACE, DEFAULT cleared");
        check(pprot(chan, fb.num) == 0xEE00,
              "B.TXT's protection comes from the DEFAULT_PROTECTION SUB.DIR inherited");

        /* a new version: the previous version's ACL (less NOPROPAGATE) and protection */
        check(add_raw(chan, fa.num, a_ctl) & 1, "A.TXT;1 gets (IDENTIFIER=[1,4],ACCESS=CONTROL)");
        {
            struct vms_acp_fileop_args m;
            memset(&m, 0, sizeof(m));
            m.chan = chan; m.func = VMS_ACP_FOP_MODIFY; m.fidmode = 1;
            m.fid_num = fa.num; m.fid_seq = 1;
            m.attr_ctl = VMS_ACP_ATTR_PROT; m.attr.fileprot = 0xE000;   /* S:RWED,O:RWED,G:RWED,W:R */
            check(vms_kif_acp_fileop(&m) & 1, "A.TXT;1 protection set to (RWED,RWED,RWED,R)");
        }
        check(pcreate(chan, &pd, "A.TXT", 0, 0, &fa2) & 1, "create A.TXT;2");
        check(pacl_is(chan, fa2.num, v2_acl, sizeof(v2_acl)),
              "A.TXT;2 inherits A.TXT;1's ACL less its NOPROPAGATE ACE");
        check(pprot(chan, fa2.num) == 0xE000, "A.TXT;2 inherits A.TXT;1's protection (RWED,RWED,RWED,R)");

        ok  = (int)(pdelete(chan, &sub, "B.TXT", 1) & 1);
        ok &= (int)(pdelete(chan, &pd, "SUB.DIR", 1) & 1);
        ok &= (int)(pdelete(chan, &pd, "A.TXT", 2) & 1);
        ok &= (int)(pdelete(chan, &pd, "A.TXT", 1) & 1);
        ok &= (int)(pdelete(chan, &ovmx, "PROPD.DIR", 1) & 1);
        check(ok, "delete the inheritance files and directories (restore)");
    }

    /* -- clean up -- */
    {
        static const char *names[] = { "ACLF1.DAT", "ACLF2.DAT", "ACLF3.DAT", "ACLF4.DAT",
                                       "ACLF5.DAT", "ACLF6.DAT", "ACLF7.DAT" };
        unsigned i;
        int ok = 1;
        for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            ok &= (int)(delete_file(chan, names[i]) & 1);
        check(ok, "delete the test files (restore)");
    }
    (void)vms_kif_dassgn(chan);
    (void)vms_kif_acp_dmount(ODS2_UNIT);
    printf("=== test_syssvc_acl: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
