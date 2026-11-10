/*
 * test_syssvc_convert_dcl.c - DCL CONVERT does not claim a conversion it did
 * not perform (vms-558e), driven through the shipped DCL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. CONVERT accepted /FDL, printed "accepted but ignored",
 * then line-copied the file and printed "%CONVERT-S-CONVERTED, N records
 * converted" -- claiming a record-format conversion it never did. Now: a plain
 * CONVERT is a record-for-record copy and silent on success (as on VMS); /FDL,
 * which asks for a real conversion, is refused with %DCL-W-IVQUAL and writes
 * nothing.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-convert-claims-converted.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "starlet.h"            /* $VMS_STATUS_SUCCESS */
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"
#define RW_UNIT  "VDA0:"
#define DIR      RW_UNIT "[OVMXDIR]"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

/* DCL's output comes back from its SYS$OUTPUT mailbox (rd vms-2f39). */
static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2];
    struct ovmx_dcl_sysout so;
    out[0] = '\0';
    if (!(ovmx_dcl_sysout_open(&so) & 1)) return -1;
    if (pipe(in_pipe) < 0) { ovmx_dcl_sysout_collect(&so, NULL, 0); return -1; }
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) { ovmx_dcl_sysout_collect(&so, NULL, 0); return -1; }
    if (pid == 0) {
        ovmx_dcl_sysout_child(&so);
        dup2(in_pipe[0], STDIN_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    close(in_pipe[1]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    ovmx_dcl_sysout_collect(&so, out, outsz);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_convert_dcl (CONVERT claims no conversion it did not do) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_convert_dcl: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    /* The files live on the writable ODS-2 volume (VDA0:[OVMXDIR], pid-unique
     * names, deleted after): CONVERT reads and writes only through RMS
     * (libc-drop vms-0504), never a host path, so the input is written and the
     * output read back through DCL (OPEN/WRITE, TYPE) -- RMS on the volume. */
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount(RW_UNIT)), "MOUNT " RW_UNIT " (writable)");
    unsigned pid = (unsigned)getpid() % 100000u;

    static char out1[16384], out2[16384];
    char script[2048];

    snprintf(script, sizeof script,
             "SET DEFAULT " DIR "\n"
             "OPEN/WRITE F CVI%1$u.TXT\nWRITE F \"alpha\"\nWRITE F \"beta\"\nWRITE F \"gamma\"\nCLOSE F\n"
             "CONVERT CVI%1$u.TXT CVO%1$u.TXT\n"
             "WRITE SYS$OUTPUT \"<T1>\"\nTYPE CVO%1$u.TXT\nWRITE SYS$OUTPUT \"</T1>\"\n", pid);
    CHECK(run_dcl(script, out1, sizeof out1) == 0, "DCL ran the plain CONVERT");
    printf("  ---- plain CONVERT ----\n%s  ---- end ----\n", out1);

    const char *t1 = strstr(out1, "<T1>");
    const char *t1e = t1 ? strstr(t1, "</T1>") : NULL;
    char got[256] = "";
    if (t1 && t1e) {
        /* the records TYPE shows, one per line, between the markers */
        const char *p = strchr(t1, '\n');
        size_t gl = 0;
        for (p = p ? p + 1 : t1e; p < t1e && gl < sizeof got - 1; p++)
            if (*p != '\r') got[gl++] = *p;
        got[gl] = '\0';
    }
    CHECK(strcmp(got, "alpha\nbeta\ngamma\n") == 0,
          "plain CONVERT wrote a record-for-record copy of the input");
    CHECK(strstr(out1, "CONVERTED") == NULL,
          "plain CONVERT is silent on success: no invented \"CONVERTED\" claim");

    snprintf(script, sizeof script,
             "SET DEFAULT " DIR "\n"
             "CONVERT/FDL=x.fdl CVI%1$u.TXT CVF%1$u.TXT\n"
             "WRITE SYS$OUTPUT \"F=<\" + F$SEARCH(\"CVF%1$u.TXT;*\") + \">\"\n"
             "DELETE CVI%1$u.TXT;*\nDELETE CVO%1$u.TXT;*\n"
             "IF F$SEARCH(\"CVF%1$u.TXT;*\") .NES. \"\" THEN DELETE CVF%1$u.TXT;*\n", pid);
    CHECK(run_dcl(script, out2, sizeof out2) == 0, "DCL ran CONVERT/FDL");
    printf("  ---- CONVERT/FDL ----\n%s  ---- end ----\n", out2);
    /* negctl: dcl-convert-claims-converted */
    CHECK(strstr(out2, "DCL-W-IVQUAL") != NULL,
          "CONVERT/FDL is rejected (%DCL-W-IVQUAL), not accepted-and-ignored");
    /* negctl: dcl-convert-claims-converted */
    CHECK(strstr(out2, "F=<>") != NULL,
          "CONVERT/FDL wrote no output file (no unconverted copy passed off as converted)");

    printf("=== test_syssvc_convert_dcl: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
