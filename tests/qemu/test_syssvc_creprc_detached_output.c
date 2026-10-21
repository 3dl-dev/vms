/*
 * test_syssvc_creprc_detached_output.c - a DETACHED $CREPRC'd process never
 * writes on its creator's terminal (rd vms-5843).
 *
 * A detached process has no terminal. Its SYS$OUTPUT/SYS$ERROR are what its
 * creator named in $CREPRC's output/error descriptors -- or the null device.
 * Before rd vms-5843, $CREPRC handed that name straight to open(2): a VMS file
 * spec such as STARTNET's /OUTPUT=SYS$MANAGER:NETACP.LOG never opens that way,
 * the dup2 was skipped, and the detached NETACP kept the CREATOR's descriptors
 * -- the operator console STARTUP runs on -- so every DECNETD-I- status line
 * it wrote landed on OPA0:, in the middle of the login prompt (measured on a
 * booted V0.7-13 node, 2026-10-21).
 *
 * The scenario: a CREATOR (a subprocess of this suite) points its own
 * stdout/stderr at a file standing in for its terminal, then creates three
 * detached subjects running this binary in subject mode. Each subject writes
 * a marker line to its stdout and its stderr, then records that it ran.
 *   - VMSFILE: output/error = a VMS file spec no host open() can satisfy. The
 *     marker must NOT reach the creator's terminal.            (the bug)
 *   - NULLDEV: output/error = NL:. The marker must not reach it either.
 *   - HOSTPATH: output/error = a host path (what the host harness passes).
 *     The marker must be IN that file -- the positive control that proves
 *     the subjects really write, so the two "absent" checks above mean
 *     something.
 *
 * negctl: creprc-detached-output-on-creator-terminal
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <libgen.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdint.h>
#include "starlet.h"
#include "descrip.h"
#include "prcdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP   77
#define DIR_        "/tmp/c5843"
#define TERMFILE    DIR_ "/creator-terminal.out"
#define HOSTOUT     DIR_ "/hostpath.out"
#define HOSTERR     DIR_ "/hostpath.err"
#define MARK        "MARK5843"

/* The host-path case names two files: two independent O_TRUNC opens of ONE
 * path would each write from offset 0 and the stderr line would overwrite
 * the stdout line. */
static const struct { const char *role, *out, *err; } CASES[] = {
    { "VMSFILE",  "SYS$MANAGER:C5843$NOSUCH.LOG", "SYS$MANAGER:C5843$NOSUCH.LOG" },
    { "NULLDEV",  "NL:", "NL:" },
    { "HOSTPATH", HOSTOUT, HOSTERR },
};
#define NCASES (int)(sizeof(CASES) / sizeof(CASES[0]))

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* SUBJECT MODE (argv[0] = DIR_/<role>): write the marker to both streams,
 * then record that this subject ran. */
static int subject_main(const char *role)
{
    printf(MARK " %s stdout\n", role);
    fflush(stdout);
    fprintf(stderr, MARK " %s stderr\n", role);
    fflush(stderr);
    char p[128];
    snprintf(p, sizeof(p), DIR_ "/%s.ran", role);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
    return 0;
}

static int file_has(const char *path, const char *needle)
{
    char buf[8192];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static int ran(const char *role)
{
    char p[128];
    snprintf(p, sizeof(p), DIR_ "/%s.ran", role);
    return access(p, F_OK) == 0;
}

/* CREATOR MODE: its stdout/stderr are its "terminal" (TERMFILE). */
static int creator_main(int report_fd)
{
    int r[NCASES];
    (void)prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
    int t = open(TERMFILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (t < 0) return 1;
    dup2(t, STDOUT_FILENO);
    dup2(t, STDERR_FILENO);
    close(t);

    for (int i = 0; i < NCASES; i++) {
        char img[128];
        snprintf(img, sizeof(img), DIR_ "/%s", CASES[i].role);
        struct dsc$descriptor_s imgd = mkdsc(img);
        struct dsc$descriptor_s od = mkdsc(CASES[i].out);
        struct dsc$descriptor_s ed = mkdsc(CASES[i].err);
        uint32_t pid = 0;
        r[i] = (int)sys$creprc(&pid, &imgd, NULL, &od, &ed, NULL, NULL,
                               NULL, 0, 0, 0, PRC$M_DETACH);
    }
    for (int waited = 0; waited < 30000; waited += 50) {
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        int all = 1;
        for (int i = 0; i < NCASES; i++)
            if ((r[i] & 1) && !ran(CASES[i].role)) all = 0;
        if (all) break;
        poll(NULL, 0, 50);
    }
    poll(NULL, 0, 200);                 /* let the last writes land */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    return write(report_fd, r, sizeof(r)) == (ssize_t)sizeof(r) ? 0 : 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 1 && strncmp(argv[0], DIR_ "/", strlen(DIR_) + 1) == 0)
        return subject_main(basename(argv[0]));

    printf("=== test_syssvc_creprc_detached_output (a detached process never writes on its creator's terminal, rd vms-5843) ===\n");
    if (!executive_present()) {
        printf("=== test_syssvc_creprc_detached_output: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    char self[512];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) {
        printf("  FAIL: cannot resolve this test's own image\n");
        return 1;
    }
    self[sl] = '\0';
    mkdir(DIR_, 0777);
    int staged = 1;
    for (int i = 0; i < NCASES; i++) {
        char p[128];
        snprintf(p, sizeof(p), DIR_ "/%s", CASES[i].role);
        unlink(p);
        if (symlink(self, p) != 0) staged = 0;
        snprintf(p, sizeof(p), DIR_ "/%s.ran", CASES[i].role);
        unlink(p);
    }
    unlink(TERMFILE);
    unlink(HOSTOUT);
    unlink(HOSTERR);
    CHECK(staged, "staged one subject image per case (this binary, in subject mode)");

    int pfd[2];
    if (pipe(pfd) < 0) { printf("  FAIL: pipe\n"); return 1; }
    pid_t c = ovmx_fork_subprocess();
    if (c == 0) {
        close(pfd[0]);
        _exit(creator_main(pfd[1]));
    }
    close(pfd[1]);
    int r[NCASES];
    int ok = read(pfd[0], r, sizeof(r)) == (ssize_t)sizeof(r);
    waitpid(c, NULL, 0);
    CHECK(ok, "the creator reported back");
    if (!ok) {
        printf("=== test_syssvc_creprc_detached_output: %d passed, %d failed ===\n", pass, fail);
        return 1;
    }

    int all_created = 1, all_ran = 1;
    for (int i = 0; i < NCASES; i++) {
        if (!(r[i] & 1)) {
            all_created = 0;
            printf("  INFO: $CREPRC (%s, output=%s) status %08X\n",
                   CASES[i].role, CASES[i].out, (unsigned)r[i]);
        }
        if (!ran(CASES[i].role)) all_ran = 0;
    }
    CHECK(all_created, "every detached $CREPRC succeeded");
    CHECK(all_ran, "every detached subject ran and wrote its marker");
    CHECK(file_has(HOSTOUT, MARK " HOSTPATH stdout") && file_has(HOSTERR, MARK " HOSTPATH stderr"),
          "positive control: a subject whose output names an openable path writes its marker THERE");
    CHECK(!file_has(TERMFILE, MARK " NULLDEV"),
          "a detached process with output=error=NL: writes nothing on its creator's terminal");
    /* negctl: creprc-detached-output-on-creator-terminal */
    CHECK(!file_has(TERMFILE, MARK " VMSFILE"),
          "a detached process whose output names a VMS file $CREPRC cannot open writes nothing on its creator's terminal");
    CHECK(!file_has(TERMFILE, MARK " HOSTPATH"),
          "the positive-control subject's output did not leak onto the creator's terminal either");

    printf("=== test_syssvc_creprc_detached_output: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
