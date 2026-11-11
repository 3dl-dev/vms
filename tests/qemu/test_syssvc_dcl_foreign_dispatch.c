/*
 * test_syssvc_dcl_foreign_dispatch.c - DCL foreign-command dispatch (rd
 * vms-96e): "SYM :== $image-spec", then a BARE "SYM args" activates the image
 * with the rest of the line as its parameters. What the image prints is read
 * from DCL's SYS$OUTPUT mailbox (rd vms-380f, vms-2f39).
 *
 * MOVED FROM the host ctest foreign_command_dispatch_gate
 * (tests/integration/test_foreign_command_dispatch.sh), whose image output a
 * host DCL could only hand back on stdio: a host image has no executive and
 * cannot learn SYS$OUTPUT. Every check is kept; the PARTS step, optional on
 * the host, is mandatory here, and argument order is asserted.
 *
 *   1. bare FOO (FOO :== "$<ECHOARGS>") runs the image, P1..P3 arrive in
 *      order, the image completes, $STATUS is a VMS success.
 *   2. a nonzero image exit is surfaced as ABORT.
 *   3. SELFNAMED :== $ (no image-spec) names the symbol itself as the image.
 *   4. a symbol whose value has no leading $ is never dispatched (IVVERB).
 *   5. PARTS :== $<PARTS.EXE>, bare "PARTS LOAD 5 <file>" runs the real demo
 *      image against an indexed file on VDA0:.
 *
 * ECHOARGS and FAILIMG are this binary, copied under those names.
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-foreign-default-name-dropped.defect.
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
#include "rms/rms.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

#define EXIT_SKIP 77
#define DCL_PATH  "/bin/DCL.EXE"
#define PARTS_EXE "/vms/SYS0/SYSCOMMON/SYSEXE/PARTS.EXE"
#define PARTS_DAT "VDA0:[OVMXDIR]OVMX380F_PARTS.DAT"
#define WORK      "/tmp/ovmx380f_fc"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char out[65536];

static const char *self_base(char *buf, size_t sz)
{
    ssize_t n = readlink("/proc/self/exe", buf, sz - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    char *semi = strchr(buf, ';');
    if (semi) *semi = '\0';
    const char *b = strrchr(buf, '/');
    return b ? b + 1 : buf;
}

static int stage_self(const char *dst)
{
    int in = open("/proc/self/exe", O_RDONLY);
    if (in < 0) return -1;
    int o = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (o < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(o, buf, (size_t)n) != n) { rc = -1; break; }
    close(in);
    if (close(o) != 0) rc = -1;
    chmod(dst, 0755);
    return rc;
}

static int run_dcl(const char *script, char *const env[])
{
    int in_pipe[2];
    struct ovmx_dcl_sysout so;
    out[0] = '\0';
    if (!(ovmx_dcl_sysout_open(&so) & 1)) return -1;
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
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, env);
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
    printf("  ---- DCL's SYS$OUTPUT ----\n%s  ---- end ----\n", out);
    return 0;
}

static int has(const char *s) { return strstr(out, s) != NULL; }

static const char *line_eq(const char *want)
{
    size_t wl = strlen(want);
    for (const char *p = out; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && p[l - 1] == '\r') l--;
        if (l == wl && memcmp(p, want, wl) == 0) return p;
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* 'STATUS == "%X' then zero or more '0' then '1"' (SS$_NORMAL as %X00000001). */
static int status_success(void)
{
    for (const char *p = strstr(out, "STATUS == \"%X"); p; p = strstr(p + 1, "STATUS == \"%X")) {
        const char *q = p + strlen("STATUS == \"%X");
        while (*q == '0') q++;
        if (q[0] == '1' && q[1] == '"') return 1;
    }
    return 0;
}

/* `a`, then `b` later on the same line. */
static int same_line(const char *a, const char *b)
{
    for (const char *p = strstr(out, a); p; p = strstr(p + 1, a)) {
        const char *e = strchr(p, '\n');
        const char *q = strstr(p + strlen(a), b);
        if (q && (!e || q < e)) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char selfbuf[512];
    const char *base = self_base(selfbuf, sizeof selfbuf);
    if (strcmp(base, "ECHOARGS") == 0) {
        printf("ECHOARGS-BEGIN\n");
        for (int i = 1; i < argc; i++) printf("ARG=[%s]\n", argv[i]);
        printf("ECHOARGS-END\n");
        return 0;
    }
    if (strcmp(base, "FAILIMG") == 0) {
        printf("FAILIMG-RAN\n");
        fflush(stdout);
        return 3;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_foreign_dispatch (SYM :== $image, bare SYM runs it, vms-96e) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_foreign_dispatch: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    char *const env[] = { (char *)"PATH=/bin", (char *)"PARTS_FILE=" PARTS_DAT, NULL };
    mkdir(WORK, 0755);
    CHECK(stage_self(WORK "/ECHOARGS") == 0 && stage_self(WORK "/FAILIMG") == 0,
          "the ECHOARGS and FAILIMG images are staged");

    /* 1. dispatch, argument forwarding, $STATUS */
    CHECK(run_dcl("FOO :== \"$" WORK "/ECHOARGS\"\nFOO \"hello\" \"world\" \"123\"\nSHOW SYMBOL $STATUS\n",
                  env) == 0, "DCL ran FOO");
    const char *p1 = line_eq("ARG=[hello]"), *p2 = line_eq("ARG=[world]"), *p3 = line_eq("ARG=[123]");
    CHECK(has("ECHOARGS-BEGIN"), "bare foreign command activates the image at all");
    CHECK(p1 != NULL, "P1 arrives as the image's first argument, in order");
    CHECK(p2 != NULL, "P2 arrives second");
    CHECK(p3 != NULL, "P3 arrives third");
    CHECK(p1 && p2 && p3 && p1 < p2 && p2 < p3, "P1, P2, P3 arrive in that order");
    CHECK(has("ECHOARGS-END"), "the image actually completed (not just launched)");
    CHECK(status_success(), "successful foreign command sets $STATUS to a VMS success code");

    /* 2. nonzero image exit */
    CHECK(run_dcl("BAR :== \"$" WORK "/FAILIMG\"\nBAR\n", env) == 0, "DCL ran BAR");
    CHECK(has("FAILIMG-RAN"), "a failing foreign-command image runs before it fails");
    CHECK(has("ABORT"), "a nonzero image exit is reported (%DCL-...-ABORT), not swallowed");

    /* 3. empty image-spec defaults to the symbol's name */
    CHECK(run_dcl("SELFNAMED :== $\nSELFNAMED\n", env) == 0, "DCL ran SELFNAMED");
    /* negctl: dcl-foreign-default-name-dropped */
    CHECK(same_line("IVIMAGE", "SELFNAMED"),
          "SYM :== $ (empty image-spec) defaults the image name to the symbol");

    /* 4. a plain string symbol is not dispatched */
    CHECK(run_dcl("PLAINSYM :== HELLO\nPLAINSYM\n", env) == 0, "DCL ran PLAINSYM");
    CHECK(has("IVVERB"), "a non-$ symbol value is never treated as a foreign command");

    /* 5. the PARTS demo moment */
    CHECK(access(PARTS_EXE, X_OK) == 0, "PARTS.EXE is staged at SYS$SYSTEM in the initramfs");
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount("VDA0:")),
          "MOUNT VDA0:, the volume PARTS's indexed file goes on");
    CHECK(run_dcl("PARTS :== \"$" PARTS_EXE "\"\nPARTS LOAD 5 \"" PARTS_DAT "\"\n", env) == 0,
          "DCL ran PARTS LOAD");
    CHECK(has("PARTS-I-LOADED, 5 part records written"),
          "PARTS :== $<PARTS.EXE> then bare PARTS LOAD runs the real demo");
    CHECK(has("PARTS-S-FOUND  key PN000001"),
          "PARTS's own keyed-lookup proof still runs end to end");
    {
        struct FAB fab = cc$rms_fab;
        fab.fab$l_fna = (char *)PARTS_DAT;
        fab.fab$b_fns = (uint8_t)strlen(PARTS_DAT);
        (void)sys$erase(&fab, 0, 0);
    }

    unlink(WORK "/ECHOARGS");
    unlink(WORK "/FAILIMG");
    rmdir(WORK);
    printf("=== test_syssvc_dcl_foreign_dispatch: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
