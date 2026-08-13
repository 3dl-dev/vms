/*
 * test_lib_spawn_dcl.c - lib$spawn creates a REAL DCL subprocess (vms-98c)
 *
 * Self-host spine #4 (vms-ec70), exec-drive prereq A. Proves that lib$spawn
 * spawns the actual DCL command interpreter and that interpreter really runs
 * the command -- the fix for the facade this item removed, where lib$spawn
 * fork+exec'd `/bin/sh -c <command>` (the Unix Bourne shell) and reported
 * SS$_NORMAL for a "SHOW TIME" no DCL ever saw.
 *
 * FAILS-ON-FACADE, TWO WAYS:
 *
 *   (1) OBSERVABLE EFFECT FROM A REAL DCL CHILD. lib$spawn("SHOW TIME") with
 *       SYS$OUTPUT redirected to a file. The assertion is that the file holds
 *       the CURRENT YEAR -- taken independently from the C library clock in
 *       THIS process (the oracle), never from lib$spawn. Only DCL's SHOW TIME
 *       prints a VMS date carrying that year; /bin/sh -c "SHOW TIME" prints a
 *       "SHOW: not found" error and no year, so the pre-fix body fails this.
 *       The effect is sourced from the real child (its redirected SYS$OUTPUT),
 *       through the real lib$spawn resolve+fork+exec path, with no monkeypatch.
 *
 *   (2) HONEST FAILURE, NO /bin/sh FALLBACK. With the DCL CLI image absent,
 *       lib$spawn must return a VMS error and create NOTHING -- it must not
 *       substitute another program and must not report success. Asserts the
 *       return status is a failure (even) and the output file was never made.
 *
 * MECHANISM UNDER TEST (hermetic, no shared /vms, no /dev/vms). lib$spawn
 * resolves the CLI image through the VMS filespec translator as
 * SYS$SYSTEM:DCL.EXE. This test redefines SYS$SYSTEM (LNM$PROCESS_TABLE, which
 * the LNM$FILE_DEV search list consults first) to a private temp directory and
 * stages the build's own DCL image (vmsdcl, passed in as VMSDCL_PATH) there as
 * DCL.EXE. So the real resolution path runs, but against a private tree -- the
 * same technique test_login_logicals uses for SYS$LOGIN. `vmsdcl -c "SHOW
 * TIME"` needs no executive (see tests/integration/test_dcl_basic.sh), so the
 * whole spawn->DCL->SHOW TIME path is provable on a host ctest.
 *
 * NOT COVERED HERE (prereqs B/C, vms-e0b + vms-9003): the persistent-
 * subprocess + mailbox + write-attention-AST protocol MMK's build_target.c
 * uses to stream many commands into one long-lived DCL and read each command's
 * full $STATUS back. This proves the create+run primitive that rides under it.
 *
 * Doc pins (VSI OpenVMS, public): RTL Library (LIB$) Routines Reference Manual,
 * LIB$SPAWN; DCL Dictionary, SPAWN and SHOW TIME.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

/* Honest-skip exit code: matches the CTest SKIP_RETURN_CODE property. */
#define SKIP_EXIT 77

static int failures = 0;

#define CHECK(cond, msg) do {                                           \
    if (cond) { printf("PASS: %s\n", (msg)); }                          \
    else { printf("FAIL: %s\n", (msg)); failures++; }                   \
} while (0)

/* Read a whole file into buf; returns bytes read, or -1 if it does not exist. */
static long slurp(const char *path, char *buf, size_t bufsz) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

int main(void) {
    /* The DCL image the build produced, handed over by CTest (see the test's
     * CMake ENVIRONMENT property). Absent -> honest skip, never a fake pass. */
    const char *dcl_src = getenv("OVMX_TEST_DCL_IMAGE");
    if (!dcl_src || !dcl_src[0]) {
        printf("SKIP: OVMX_TEST_DCL_IMAGE not set (no DCL image to spawn)\n");
        return SKIP_EXIT;
    }
    if (access(dcl_src, X_OK) != 0) {
        printf("SKIP: DCL image %s is not executable in this environment\n",
               dcl_src);
        return SKIP_EXIT;
    }

    /* Private, hermetic system root. */
    char tmpl[] = "/tmp/ovmx_spawn_XXXXXX";
    char *sysdir = mkdtemp(tmpl);
    if (!sysdir) { perror("mkdtemp"); return 2; }

    char dcl_exe[1024];
    snprintf(dcl_exe, sizeof(dcl_exe), "%s/DCL.EXE", sysdir);
    if (symlink(dcl_src, dcl_exe) != 0) {
        /* symlink may fail across filesystems; fall through to a hard copy. */
        char cp[2200];
        snprintf(cp, sizeof(cp), "cp -f '%s' '%s'", dcl_src, dcl_exe);
        if (system(cp) != 0) { perror("stage DCL.EXE"); return 2; }
        chmod(dcl_exe, 0755);
    }

    /* Redirect SYS$SYSTEM at the private root so SYS$SYSTEM:DCL.EXE resolves
     * to our staged image. LNM$PROCESS_TABLE is first in LNM$FILE_DEV. */
    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { fprintf(stderr, "no lnm manager\n"); return 2; }
    uint32_t cst_lnm = lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM",
                                  sysdir, 0, LNM_MODE_SUPER);
    CHECK(cst_lnm & 1, "define SYS$SYSTEM -> private root");

    /* Sanity: the resolver lib$spawn uses must land on our staged image. */
    char resolved[1024];
    int rr = vmsfs_to_linux_path("SYS$SYSTEM:DCL.EXE", resolved,
                                 sizeof(resolved));
    printf("INFO: SYS$SYSTEM:DCL.EXE -> %s (rc=%d)\n", resolved, rr);
    CHECK(rr == 1 && strcmp(resolved, dcl_exe) == 0,
          "SYS$SYSTEM:DCL.EXE resolves to staged image");

    /* The output path lib$spawn will write, computed via the SAME resolver so
     * the test reads exactly the file the child produces (case/format-safe). */
    char out_linux[1024];
    int orr = vmsfs_to_linux_path("SYS$SYSTEM:OUT.TXT", out_linux,
                                  sizeof(out_linux));
    if (orr != 1) { fprintf(stderr, "cannot resolve output spec\n"); return 2; }
    unlink(out_linux);

    /* ---- TEST 1: real DCL child runs SHOW TIME, observable in SYS$OUTPUT ---- */
    struct dsc$descriptor_s cmd_d = {
        (uint16_t)strlen("SHOW TIME"), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"SHOW TIME"
    };
    struct dsc$descriptor_s out_d = {
        (uint16_t)strlen("SYS$SYSTEM:OUT.TXT"), DSC$K_DTYPE_T, DSC$K_CLASS_S,
        (char *)"SYS$SYSTEM:OUT.TXT"
    };

    fflush(stdout);  /* empty stdio buffer so the forked child never inherits it */
    uint32_t child_pid = 0, completion = 0;
    uint32_t r = lib$spawn(&cmd_d, NULL, &out_d, NULL, NULL,
                           &child_pid, &completion,
                           NULL, NULL, NULL, NULL, NULL, NULL);

    CHECK(r == SS$_NORMAL, "lib$spawn returns SS$_NORMAL for a created subprocess");
    CHECK(completion & 1, "subprocess completion status is success (odd)");
    CHECK(child_pid != 0, "lib$spawn returns a subprocess PID");

    /* Independent oracle: the current year from THIS process's clock. */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char year[16];
    snprintf(year, sizeof(year), "%d", 1900 + lt->tm_year);

    char body[8192];
    long n = slurp(out_linux, body, sizeof(body));
    printf("INFO: SYS$OUTPUT capture (%ld bytes): %.120s\n", n, n > 0 ? body : "");
    CHECK(n > 0, "SYS$OUTPUT file was created by the DCL child");
    CHECK(n > 0 && strstr(body, year) != NULL,
          "DCL SHOW TIME output contains the current year (real DCL ran it)");

    /* ---- TEST 2: no CLI image -> honest failure, nothing created ---- */
    unlink(dcl_exe);                 /* remove the staged DCL.EXE */
    char out2_linux[1024];
    vmsfs_to_linux_path("SYS$SYSTEM:OUT2.TXT", out2_linux, sizeof(out2_linux));
    unlink(out2_linux);

    struct dsc$descriptor_s out2_d = {
        (uint16_t)strlen("SYS$SYSTEM:OUT2.TXT"), DSC$K_DTYPE_T, DSC$K_CLASS_S,
        (char *)"SYS$SYSTEM:OUT2.TXT"
    };
    fflush(stdout);
    uint32_t completion2 = 0;
    uint32_t r2 = lib$spawn(&cmd_d, NULL, &out2_d, NULL, NULL,
                            NULL, &completion2,
                            NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(!(r2 & 1), "lib$spawn fails honestly when the CLI image is absent");

    struct stat st2;
    CHECK(stat(out2_linux, &st2) != 0,
          "no SYS$OUTPUT created when the command never ran (no /bin/sh fallback)");

    /* Cleanup (best effort). */
    unlink(out_linux);
    unlink(out2_linux);
    rmdir(sysdir);

    printf("\n%s (%d failure%s)\n",
           failures == 0 ? "ALL PASS" : "FAILURES", failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
