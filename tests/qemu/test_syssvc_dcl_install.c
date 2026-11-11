/*
 * test_syssvc_dcl_install.c - DCL's INSTALL verb runs SYS$SYSTEM:INSTALL.EXE
 * against the executive's known-file list, and what INSTALL writes -- LIST's
 * display and its messages -- is records of DCL's SYS$OUTPUT (rd vms-d41).
 *
 * WHAT VMS DOES. INSTALL is an image DCL runs in the user's process. LIST's
 * display goes to SYS$OUTPUT; a message ($PUTMSG) goes to SYS$ERROR and, when
 * SYS$OUTPUT is another file, to SYS$OUTPUT too -- once when they are the same
 * (OpenVMS System Services Reference, $PUTMSG). The known file list is
 * executive state, changed under CMKRNL.
 *
 * WHAT WAS WRONG. INSTALL wrote its messages to the substrate's fd 2, which is
 * the null device under a DCL whose SYS$ERROR is a mailbox, so `INSTALL ADD`
 * exited %X2C with no message. With the message visible, the cause was the
 * QEMU harness: it lays down the boot staging tree as ovmx_init does but left
 * out the executive's own directory of installed-image copies (VMS_KFE_DIR),
 * so the executive had nowhere to copy the image (SS$_NOSUCHFILE, which
 * INSTALL also misreported as "Known File Entry not found").
 *
 * MOVED FROM tests/dcl/test_install_command.sh (rd vms-d41), made stricter:
 * the host probe could only show INSTALL.EXE failing NOSUCHDEV (no executive).
 * Here ADD, LIST and REMOVE succeed against the executive's list, LIST shows
 * the entry with its flags, and it is gone after REMOVE. The no-executive
 * refusal stays proven on the host by src/install/test/run_install_test.sh.
 *
 * Every case drives the shipped DCL.EXE with SYS$OUTPUT/SYS$ERROR a temporary
 * mailbox and its substrate stdout/stderr on the null device
 * (ovmx_dcl_sysout.h): a line not written to SYS$OUTPUT is not seen at all.
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/install-msg-to-substrate-stderr.defect.
 * NO EXECUTIVE: EXIT_SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

#define EXIT_SKIP 77
#define DCL_PATH  "/bin/DCL.EXE"
#define SYSEXE    "/vms/SYS0/SYSCOMMON/SYSEXE"
#define SYSLIB    "/vms/SYS0/SYSCOMMON/SYSLIB"
#define KFE_IMG   SYSLIB "/TESTLIBD41$SHR.EXE"
#define KFE_DB    SYSEXE "/VMS$KNOWN_IMAGES.DAT"
#define MARK      "OVMXD41-INSTALL-AFTER-REMOVE"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char out[65536];
static char *const base_env[] = { (char *)"PATH=/bin", NULL };

/* DCL.EXE with `script` on its SYS$INPUT; `flags` as ovmx_dcl_sysout_open_flags
 * (0: SYS$OUTPUT and SYS$ERROR are both the mailbox). */
static int run_dcl(const char *script, uint32_t flags)
{
    int in_pipe[2];
    struct ovmx_dcl_sysout so;
    out[0] = '\0';
    if (!(ovmx_dcl_sysout_open_flags(&so, flags) & 1)) return -1;
    if (pipe(in_pipe) < 0) { ovmx_dcl_sysout_collect(&so, NULL, 0); return -1; }
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        ovmx_dcl_sysout_collect(&so, NULL, 0);
        return -1;
    }
    if (pid == 0) {
        ovmx_dcl_sysout_child(&so);
        dup2(in_pipe[0], STDIN_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, base_env);
        ovmx_dcl_sysout_note(&so, "EXEC-OF-DCL-FAILED\n");
        _exit(127);
    }
    close(in_pipe[0]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* DCL reads EOF */ }
    close(in_pipe[1]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    ovmx_dcl_sysout_collect(&so, out, sizeof out);
    return 0;
}

static void show(const char *what)
{
    printf("  ---- %s: DCL's SYS$OUTPUT ----\n%s  ---- end ----\n", what, out);
}

static int has(const char *s) { return strstr(out, s) != NULL; }

static int count(const char *s)
{
    int n = 0;
    for (const char *p = strstr(out, s); p; p = strstr(p + 1, s))
        n++;
    return n;
}

/* A line holding `a` and, after it on the same line, `b`. */
static int same_line(const char *a, const char *b)
{
    for (const char *p = strstr(out, a); p; p = strstr(p + 1, a)) {
        const char *e = strchr(p, '\n');
        const char *q = strstr(p + strlen(a), b);
        if (q && (!e || q < e)) return 1;
    }
    return 0;
}

/* ---- install <- tests/dcl/test_install_command.sh ---- */
static void case_install(void)
{
    mkdir("/vms", 0755); mkdir("/vms/SYS0", 0755); mkdir("/vms/SYS0/SYSCOMMON", 0755);
    mkdir(SYSLIB, 0755); mkdir(SYSEXE, 0755);
    unlink(KFE_DB);
    int fd = open(KFE_IMG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, "dummy shareable image\n", 22) != 22) { /* checked below */ }
        close(fd);
    }
    CHECK(access(KFE_IMG, R_OK) == 0, "install: the image to install exists at SYS$SHARE");
    CHECK(run_dcl("INSTALL ADD SYS$SHARE:TESTLIBD41$SHR.EXE /OPEN /SHARED\n"
                  "INSTALL LIST\n"
                  "INSTALL REMOVE SYS$SHARE:TESTLIBD41$SHR.EXE\n"
                  "WRITE SYS$OUTPUT \"" MARK "\"\n"
                  "INSTALL LIST\n"
                  "EXIT\n", 0) == 0,
          "install: DCL ran INSTALL ADD, LIST, REMOVE, LIST");
    show("install");
    char *mark = strstr(out, MARK);
    CHECK(mark != NULL, "install: DCL's own record after REMOVE is in SYS$OUTPUT");
    if (mark) *mark = '\0';                 /* out = everything before REMOVE's LIST */
    CHECK(same_line("TESTLIBD41$SHR.EXE", "Open") && same_line("TESTLIBD41$SHR.EXE", "Shar"),
          "install: INSTALL.EXE's LIST shows the executive's entry for TESTLIBD41$SHR.EXE, Open and Shared");
    CHECK(!has("NOIMAGES"), "install: the LIST after ADD is not empty");
    CHECK(!has("%INSTALL-E-") && !has("%INSTALL-W-") && !has("-SYSTEM-"),
          "install: ADD, LIST and REMOVE each succeeded against the executive (no INSTALL error)");
    if (mark) {
        *mark = MARK[0];
        CHECK(strstr(mark, "TESTLIBD41") == NULL,
              "install: after REMOVE the executive's list no longer holds TESTLIBD41$SHR.EXE");
    }
    CHECK(!has("INSTALL-I-ADDED") && !has("INSTALL-I-REMOVED"),
          "install: no DCL-side stub message stands in for the utility");
    CHECK(!has("Segmentation"), "install: nothing crashed");
    CHECK(access(KFE_DB, F_OK) != 0,
          "install: no on-disk known-image database is written (the list is the executive's)");
    unlink(KFE_DB);
    unlink(KFE_IMG);
}

/* ---- msgs: INSTALL's failure messages are SYS$OUTPUT records ---- */
static void case_msgs(void)
{
    /* SYS$ERROR is SYS$OUTPUT's mailbox: each message once. */
    CHECK(run_dcl("INSTALL REMOVE SYS$SHARE:NOTINSTD41$SHR.EXE\n"
                  "INSTALL LIST SYS$SHARE:NOTINSTD41$SHR.EXE\n"
                  "EXIT\n", 0) == 0,
          "msgs: DCL ran INSTALL REMOVE and LIST of an image that is not installed");
    show("msgs");
    /* negctl: install-msg-to-substrate-stderr */
    CHECK(count("%INSTALL-E-FAIL, failed to REMOVE entry for SYS$SHARE:NOTINSTD41$SHR.EXE") == 1,
          "msgs: REMOVE's %INSTALL-E-FAIL is a record of SYS$OUTPUT, once");
    CHECK(count("%INSTALL-W-FAIL, failed to LIST entry for SYS$SHARE:NOTINSTD41$SHR.EXE") == 1,
          "msgs: LIST's %INSTALL-W-FAIL is a record of SYS$OUTPUT, once");
    CHECK(count("-INSTALL-E-NOKFEFND, Known File Entry not found") == 2,
          "msgs: each failure's reason (-INSTALL-E-NOKFEFND) follows it in SYS$OUTPUT");

    /* SYS$OUTPUT alone is the mailbox: $PUTMSG still writes SYS$OUTPUT. */
    CHECK(run_dcl("INSTALL REMOVE SYS$SHARE:NOTINSTD41$SHR.EXE\nEXIT\n",
                  OVMX_DCL_SYSOUT_F_OUTPUT_ONLY) == 0,
          "msgs: DCL ran INSTALL REMOVE with only SYS$OUTPUT the mailbox");
    show("msgs output-only");
    CHECK(count("%INSTALL-E-FAIL, failed to REMOVE entry for SYS$SHARE:NOTINSTD41$SHR.EXE") == 1 &&
          has("-INSTALL-E-NOKFEFND"),
          "msgs: with SYS$ERROR elsewhere, the message is still a record of SYS$OUTPUT, once");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_install (INSTALL.EXE run by DCL writes DCL's SYS$OUTPUT, vms-d41) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_install: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    CHECK(access(SYSEXE "/INSTALL.EXE", X_OK) == 0,
          "INSTALL.EXE is staged at SYS$SYSTEM in the initramfs");

    case_install();
    case_msgs();

    printf("=== test_syssvc_dcl_install: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
