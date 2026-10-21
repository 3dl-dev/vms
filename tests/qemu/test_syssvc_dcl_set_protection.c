/*
 * test_syssvc_dcl_set_protection.c - SET PROTECTION and SET FILE change the
 * Files-11 header through the executive ACP, never a host file (rd vms-1fb7).
 *
 * What it proves against a real /dev/vms, on the writable VDA0:[OVMXDIR]:
 *
 *   1. DCL SET PROTECTION=(S:RWED,O:RWED,G:RE,W:R) leaves the protection word
 *      0xEA00 in the file HEADER -- read back here straight through the ACP
 *      (IO$_ACCESS, attr.fileprot), and seen by DCL's own VMS-side readers,
 *      F$FILE_ATTRIBUTES(...,"PRO") and DIRECTORY/PROTECTION. $STATUS success.
 *   2. SET FILE/PROTECTION=(G:R) merges onto it (only GROUP changes): 0xEE00.
 *   3. SET FILE/OWNER_UIC=[144,144] makes [100,100] the header's owner.
 *   4. A file that is not there: %SET-E-SEARCHFAIL / -RMS-E-FNF,
 *      $STATUS %X0077123A (SET PROTECTION) and %SET-F-SEARCHFAIL,
 *      $STATUS %X0077123C (SET FILE) -- the VAX1 forms
 *      (tests/lab/captures/set-protection-20261021/), without the
 *      inhibit-display bit (rd vms-390).
 *   5. SET FILE/VERSION_LIMIT is refused SS$_UNSUPPORTED (%X00000E4A): the ACP
 *      has no write path for it yet, and it used to be "acknowledged".
 *   6. Changing protection is a CONTROL right (OpenVMS Guide to System
 *      Security), not a write: an unprivileged [100,100] process may not change
 *      the protection of a WORLD-WRITABLE file it does not own (SS$_NOPRIV,
 *      header unchanged), and may change the one it owns.
 *
 * Files created are deleted again. No /dev/vms -> honest SKIP (77).
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
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"
#define OVMXDIR_FID_NUM 11u
#define UNPRIV_GID 100
#define UNPRIV_UID 100
#define DCL_PATH "/bin/DCL.EXE"
#define CHILD_WAIT_MS 60000

static int pass, fail;
static void check(int c, const char *m)
{
    if (c) { printf("  PASS: %s\n", m); pass++; }
    else   { printf("  FAIL: %s\n", m); fail++; }
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

/* The header's protection and owner, read through the ACP by name. */
static uint32_t header_of(uint32_t chan, const char *name, uint16_t *prot,
                          uint16_t *og, uint16_t *om)
{
    struct vms_acp_access_args a;
    uint32_t st;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = OVMXDIR_FID_NUM; a.did_seq = 1;
    a.version = 1;
    strncpy(a.name, name, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    if (st & 1) {
        (void)vms_kif_acp_deaccess(chan);
        *prot = a.attr.fileprot;
        if (og) *og = a.attr.uic_group;
        if (om) *om = a.attr.uic_member;
    }
    return st;
}

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

/* --- the unprivileged [100,100] child: IO$_MODIFY a file's protection ---- */
struct child_rep { uint32_t assign_st, op_st; };

static int run_child(int wfd, uint16_t fid, uint16_t prot)
{
    struct child_rep rep = { 0, 0 };
    uint32_t chan = 0;

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {  /* identity: the unprivileged fork */
        (void)!write(wfd, &rep, sizeof(rep));
        return 1;
    }
    rep.assign_st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    if (rep.assign_st & 1) {
        struct vms_acp_fileop_args f;
        memset(&f, 0, sizeof(f));
        f.chan = chan;
        f.func = VMS_ACP_FOP_MODIFY;
        f.fidmode = 1;
        f.fid_num = fid; f.fid_seq = 1;
        f.attr_ctl = VMS_ACP_ATTR_PROT;
        f.attr.fileprot = prot;
        rep.op_st = vms_kif_acp_fileop(&f);
        (void)vms_kif_dassgn(chan);
    }
    (void)!write(wfd, &rep, sizeof(rep));
    return 0;
}

static uint32_t as_child(const char *self, uint16_t fid, uint16_t prot)
{
    int p[2];
    pid_t pid;
    struct child_rep rep = { 0, 0 };
    if (pipe(p) < 0)
        return 0;
    fflush(NULL);
    pid = ovmx_fork_unprivileged(UNPRIV_GID, UNPRIV_UID);
    if (pid == 0) {
        char wfd[16], fs[16], ps[16];
        close(p[0]);
        if (setgid(UNPRIV_GID) != 0 || setuid(UNPRIV_UID) != 0)
            _exit(1);
        snprintf(wfd, sizeof(wfd), "%d", p[1]);
        snprintf(fs, sizeof(fs), "%u", fid);
        snprintf(ps, sizeof(ps), "%u", prot);
        execl(self, self, "--child", wfd, fs, ps, (char *)NULL);
        _exit(1);
    }
    close(p[1]);
    if (pid > 0) {
        int to = 0;
        if (read_bounded(p[0], &rep, sizeof(rep), &to) != (ssize_t)sizeof(rep))
            rep.op_st = 0;
        if (to)
            kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    close(p[0]);
    return (rep.assign_st & 1) ? rep.op_st : 0;
}

/* --- DCL ----------------------------------------------------------------- */
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
    pid = ovmx_fork_subprocess();
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
        if (to) { kill(pid, SIGKILL); break; }
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
    static char out[16384];
    uint32_t st, chan = 0;
    uint16_t fid_dcl = 0, fid_other = 0, fid_mine = 0, prot = 0, og = 0, om = 0;

    if (argc >= 5 && strcmp(argv[1], "--child") == 0)
        return run_child(atoi(argv[2]), (uint16_t)atoi(argv[3]), (uint16_t)atoi(argv[4]));

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_set_protection: SET PROTECTION / SET FILE through the ACP ===\n");
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_set_protection: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    (void)vms_kif_acp_mount(ODS2_UNIT);          /* an earlier suite may have mounted it */
    st = vms_kif_acp_assign(ODS2_UNIT, &chan);
    check((st & 1) && chan, "$ASSIGN a file-class channel to " ODS2_UNIT);

    /* S:RWED,O:RWED,G,W = 0xFF00 to start from. */
    check(create_file(chan, "PRB1FB7.TXT", 1, 4, 0xFF00, &fid_dcl) & 1,
          "create PRB1FB7.TXT [1,4] (S:RWED,O:RWED,G,W)");

    /* One DCL session per probe: DCL's messages go to SYS$ERROR and its
     * SHOW/WRITE output to SYS$OUTPUT, so within one captured session their
     * relative order is not the order they were produced. */
#define F "VDA0:[OVMXDIR]PRB1FB7.TXT"
#define NOF "VDA0:[OVMXDIR]NOSUCH1FB7.TXT"

    /* 1. the header, read through the ACP, then DCL's VMS-side readers */
    check(run_dcl("SET PROTECTION=(S:RWED,O:RWED,G:RE,W:R) " F "\nSHOW SYMBOL $STATUS\n",
                  out, sizeof out) == 0, "DCL ran SET PROTECTION");
    printf("  ---- SET PROTECTION ----\n%s  ---- end ----\n", out);
    check(strstr(out, "%X00000001") != NULL && strchr(out, '%') == strstr(out, "%X"),
          "SET PROTECTION succeeds silently ($STATUS %X00000001)");
    st = header_of(chan, "PRB1FB7.TXT", &prot, NULL, NULL);
    printf("  (header protection after SET PROTECTION: %04X)\n", prot);
    /* negctl: dcl-set-protection-host-chmod */
    check((st & 1) && prot == 0xEA00,
          "the HEADER now holds (S:RWED,O:RWED,G:RE,W:R) = 0xEA00 (read through the ACP)");
    check(run_dcl("WRITE SYS$OUTPUT \"PRO1=\" + F$FILE_ATTRIBUTES(\"" F "\",\"PRO\")\n",
                  out, sizeof out) == 0, "DCL ran F$FILE_ATTRIBUTES");
    printf("%s", out);
    /* negctl: dcl-set-protection-host-chmod */
    check(strstr(out, "PRO1=(S:RWED,O:RWED,G:RE,W:R)") != NULL,
          "F$FILE_ATTRIBUTES(...,\"PRO\") reads the new protection");
    check(run_dcl("DIRECTORY/PROTECTION " F "\n", out, sizeof out) == 0,
          "DCL ran DIRECTORY/PROTECTION");
    printf("%s", out);
    check(strstr(out, "(S:RWED,O:RWED,G:RE,W:R)") != NULL,
          "DIRECTORY/PROTECTION shows the new protection");

    /* 2. SET FILE/PROTECTION merges onto the current word */
    check(run_dcl("SET FILE/PROTECTION=(G:R) " F "\nSHOW SYMBOL $STATUS\n"
                  "WRITE SYS$OUTPUT \"PRO2=\" + F$FILE_ATTRIBUTES(\"" F "\",\"PRO\")\n",
                  out, sizeof out) == 0, "DCL ran SET FILE/PROTECTION");
    printf("  ---- SET FILE/PROTECTION ----\n%s  ---- end ----\n", out);
    st = header_of(chan, "PRB1FB7.TXT", &prot, NULL, NULL);
    check(strstr(out, "%X00000001") != NULL && (st & 1) && prot == 0xEE00,
          "SET FILE/PROTECTION=(G:R) changes only GROUP: header 0xEE00");
    check(strstr(out, "PRO2=(S:RWED,O:RWED,G:R,W:R)") != NULL,
          "F$FILE_ATTRIBUTES reads the merged protection");

    /* 3. owner */
    check(run_dcl("SET FILE/OWNER_UIC=[144,144] " F "\nSHOW SYMBOL $STATUS\n",
                  out, sizeof out) == 0, "DCL ran SET FILE/OWNER_UIC");
    printf("  ---- SET FILE/OWNER_UIC ----\n%s  ---- end ----\n", out);
    st = header_of(chan, "PRB1FB7.TXT", &prot, &og, &om);
    check(strstr(out, "%X00000001") != NULL && (st & 1) && og == 0144 && om == 0144,
          "SET FILE/OWNER_UIC=[144,144] makes [144,144] the header's owner");

    /* 4. a file that is not there (VAX1 forms) */
    check(run_dcl("SET PROTECTION=(W:R) " NOF "\nSHOW SYMBOL $STATUS\n", out, sizeof out) == 0,
          "DCL ran SET PROTECTION of a missing file");
    printf("  ---- missing file ----\n%s  ---- end ----\n", out);
    check(strstr(out, "%SET-E-SEARCHFAIL, error searching for ") != NULL &&
          strstr(out, "-RMS-E-FNF, file not found") != NULL &&
          strstr(out, "%X0077123A") != NULL,
          "SET PROTECTION of a missing file: %SET-E-SEARCHFAIL / -RMS-E-FNF, %X0077123A");
    check(run_dcl("SET FILE/TRUNCATE " NOF "\nSHOW SYMBOL $STATUS\n", out, sizeof out) == 0,
          "DCL ran SET FILE/TRUNCATE of a missing file");
    printf("%s", out);
    check(strstr(out, "%SET-F-SEARCHFAIL, error searching for ") != NULL &&
          strstr(out, "-RMS-E-FNF, file not found") != NULL &&
          strstr(out, "%X0077123C") != NULL,
          "SET FILE of a missing file: %SET-F-SEARCHFAIL / -RMS-E-FNF, %X0077123C");

    /* 5. /VERSION_LIMIT refused honestly */
    check(run_dcl("SET FILE/VERSION_LIMIT=3 " F "\nSHOW SYMBOL $STATUS\n", out, sizeof out) == 0,
          "DCL ran SET FILE/VERSION_LIMIT");
    printf("%s", out);
    check(strstr(out, "%SYSTEM-E-UNSUPPORTED, unsupported operation or function") != NULL &&
          strstr(out, "%X00000E4A") != NULL,
          "SET FILE/VERSION_LIMIT is refused SS$_UNSUPPORTED (%X00000E4A)");

    /* 6. CONTROL, not write: [100,100], unprivileged */
    check(create_file(chan, "PRO1FB7.TXT", 1, 4, 0x0000, &fid_other) & 1,
          "create PRO1FB7.TXT [1,4] with every category RWED (world-writable)");
    check(create_file(chan, "PRM1FB7.TXT", 100, 100, 0xFF00, &fid_mine) & 1,
          "create PRM1FB7.TXT owned by [100,100]");
    /* negctl: acp-modify-prot-needs-only-write */
    check(as_child(argv[0], fid_other, 0xFFFF) == SS$_NOPRIV,
          "a world-writable file's protection is refused to a non-owner without CONTROL (SS$_NOPRIV)");
    st = header_of(chan, "PRO1FB7.TXT", &prot, NULL, NULL);
    /* negctl: acp-modify-prot-needs-only-write */
    check((st & 1) && prot == 0x0000, "and its header protection is unchanged");
    check(as_child(argv[0], fid_mine, 0xEE00) & 1,
          "the owner (CONTROL by ownership) changes its own file's protection");
    st = header_of(chan, "PRM1FB7.TXT", &prot, NULL, NULL);
    check((st & 1) && prot == 0xEE00, "the owner's change is in the header");

    check(delete_file(chan, "PRB1FB7.TXT") & 1, "delete PRB1FB7.TXT (restore)");
    check(delete_file(chan, "PRO1FB7.TXT") & 1, "delete PRO1FB7.TXT (restore)");
    check(delete_file(chan, "PRM1FB7.TXT") & 1, "delete PRM1FB7.TXT (restore)");
    (void)vms_kif_dassgn(chan);

    printf("=== test_syssvc_dcl_set_protection: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
