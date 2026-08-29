/*
 * test_lib_spawn_dcl.c - lib$spawn's HONEST BOUNDARY on a host without the
 * executive (vms-98c, updated for B0 vms-e9a)
 *
 * B0 (docs/design-libspawn-ovmx.md §3a/§3d) rerouted lib$spawn through the ONE
 * executive-registered creation primitive, $CREPRC (sys_process.c), so that a
 * lib$spawn'd subprocess is a genuine VMS process the rest of process
 * management can see ($GETJPI/SHOW SYSTEM/$DELPRC). That REGISTRATION needs the
 * executive: lib$spawn now inherits $CREPRC's /dev/vms dependency and, without
 * it, FAILS HONESTLY -- it no longer has an unregistered fork/exec to fall back
 * to (design §3d: a subprocess VMS process management cannot see was never
 * fully honest). The pre-B0 body fork+exec'd DCL directly with no executive at
 * all; before THAT (the vms-98c facade this file was born to kill) it
 * fork+exec'd `/bin/sh -c <command>` and reported SS$_NORMAL for a "SHOW TIME"
 * no DCL ever saw.
 *
 * WHERE THE POSITIVE PROOF LIVES NOW. "A real DCL child runs the command AND is
 * executive-registered under its prcnam, resolvable by $GETJPI BY NAME" is the
 * B0 anti-INV-6 assertion, and it can only be made against a real /dev/vms --
 * so it lives in tests/qemu/test_syssvc_libspawn_reg.c, run under the booted
 * executive. This host ctest proves the complementary property that needs NO
 * executive:
 *
 *   (1) NO EXECUTIVE -> HONEST FAILURE. With DCL.EXE resolvable+executable but
 *       no /dev/vms, lib$spawn returns a VMS error (even status) and creates
 *       nothing -- it does not run an unregistered subprocess and does not
 *       report success. (When /dev/vms IS present -- unusual for a host ctest --
 *       it instead runs the full positive proof: a real DCL SHOW TIME whose
 *       output carries the current year, sourced independently from this
 *       process's own clock.)
 *
 *   (2) NO CLI IMAGE -> HONEST FAILURE, NO FALLBACK. With the DCL CLI image
 *       absent, lib$spawn returns a VMS error and creates NOTHING -- it must
 *       not substitute another program and must not report success. Independent
 *       of the executive (the preflight image check fails first).
 *
 * MECHANISM (hermetic, no shared /vms). lib$spawn resolves the CLI image
 * through the VMS filespec translator as SYS$SYSTEM:DCL.EXE. This test redefines
 * SYS$SYSTEM (LNM$PROCESS_TABLE, first in LNM$FILE_DEV) to a private temp
 * directory and stages the build's own DCL image (vmsdcl, OVMX_TEST_DCL_IMAGE)
 * there as DCL.EXE, the same technique test_login_logicals uses for SYS$LOGIN.
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
#include <fcntl.h>
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

    /* Is the executive reachable? B0 lib$spawn registers the subprocess through
     * $CREPRC, which needs /dev/vms. On a plain host ctest it is absent (Rule 6
     * forbids insmod'ing vms.ko on the host), so the executive-absent honest-
     * failure branch is what runs here; the full positive proof is the qemu
     * suite. Probe by open() -- access() checks the real uid against the node
     * mode and can false-negative a node the process can open (the same
     * false-negative sys_process.c/cmd_spawn warn about). */
    int exec_present = 0;
    { int p = open("/dev/vms", O_RDWR); if (p >= 0) { exec_present = 1; close(p); } }
    printf("INFO: /dev/vms %s\n", exec_present ? "present" : "absent");

    /* ---- TEST 1: create through $CREPRC ---- */
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

    if (exec_present) {
        /* Full positive proof: a real DCL child ran SHOW TIME, executive-
         * registered, and its redirected SYS$OUTPUT carries the current year. */
        CHECK(r == SS$_NORMAL, "lib$spawn returns SS$_NORMAL for a created subprocess");
        CHECK(completion & 1, "subprocess completion status is success (odd)");
        CHECK(child_pid != 0, "lib$spawn returns a subprocess PID (executive VMS pid)");

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
    } else {
        /* B0 honest boundary (design §3d, INV-6): DCL.EXE resolves and is
         * executable (the preflight passed), but with no executive $CREPRC
         * cannot register the subprocess -- lib$spawn returns an even (failure)
         * status and creates NOTHING. It does NOT fall back to an unregistered
         * run and does NOT report success. */
        CHECK(!(r & 1),
              "lib$spawn fails honestly (even status) with no executive to register the subprocess");
        char body[8192];
        long n = slurp(out_linux, body, sizeof(body));
        CHECK(n <= 0,
              "no SYS$OUTPUT created when $CREPRC could not register the subprocess (no unregistered fallback)");
    }

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
