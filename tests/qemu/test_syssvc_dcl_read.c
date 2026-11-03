/*
 * test_syssvc_dcl_read.c - DCL READ validates its qualifiers and honours the
 * ones it accepts (vms-097), driven through the shipped DCL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. READ was an accept-all verb: any /QUALIFIER was
 * silently taken, so READ/DELETE (not implemented) or a typo did something
 * other than what was asked with no diagnostic, and /END_OF_FILE=label /
 * /ERROR=label -- which real VMS programs use -- were swallowed unhonoured.
 * Now an unimplemented or unknown qualifier is %DCL-W-IVQUAL, and the two
 * label qualifiers branch like GOTO.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROLS: tests/qemu/defects.d/dcl-read-quals-accept-all.defect and
 * dcl-read-labels-ignored.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "rms/rms.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"
/* The procedure and its data live on the ODS-2 volume, written through RMS:
 * DCL reaches a file -- @ and a file SYS$INPUT alike -- only through RMS, never
 * a host path (libc-drop vms-20c94, vms-5cc). */
#define RW_UNIT   "VDA0:"
#define DIR       RW_UNIT "[OVMXDIR]"

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

/* A text file of variable-length carriage-return records, one $PUT a line. */
static int put_lines(const char *spec, const char *const *lines)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_fac = FAB$M_PUT;
    if (!(sys$create(&fab, 0, 0) & 1)) return -1;
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    if (!(sys$connect(&rab, 0, 0) & 1)) { sys$close(&fab, 0, 0); return -1; }
    uint32_t st = RMS$_NORMAL;
    for (; *lines && (st & 1); lines++) {
        rab.rab$l_rbf = (char *)*lines;
        rab.rab$w_rsz = (uint16_t)strlen(*lines);
        st = sys$put(&rab, 0, 0);
    }
    uint32_t cst = sys$close(&fab, 0, 0);
    return ((st & 1) && (cst & 1)) ? 0 : -1;
}

static void erase(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_read (READ qualifiers validated and honoured) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_read: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount(RW_UNIT)), "MOUNT " RW_UNIT " (writable)");
    unsigned pid = (unsigned)getpid() % 100000u;
    char fspec[64], tspec[64], fname[32], tname[32];
    snprintf(fname, sizeof fname, "RDF%u.TXT", pid);
    snprintf(tname, sizeof tname, "RDT%u.COM", pid);
    snprintf(fspec, sizeof fspec, DIR "%s", fname);
    snprintf(tspec, sizeof tspec, DIR "%s", tname);
    static const char *const data[] = { "one", NULL };
    char define[96];
    snprintf(define, sizeof define, "$ DEFINE SYS$INPUT \"%s\"", fname);
    const char *const com[] = {
        define,
        "$ READ/END_OF_FILE=EOFL/ERROR=ERRL SYS$INPUT REC",
        "$ WRITE SYS$OUTPUT \"first=''REC'\"",
        "$ READ/END_OF_FILE=EOFL/ERROR=ERRL SYS$INPUT REC",
        "$ WRITE SYS$OUTPUT \"NOT-REACHED\"",
        "$ EXIT",
        "$ EOFL:",
        "$ WRITE SYS$OUTPUT \"at-eof\"",
        "$ EXIT",
        "$ ERRL:",
        "$ WRITE SYS$OUTPUT \"wrong-err\"",
        NULL };
    CHECK(put_lines(fspec, data) == 0, "data file written through RMS");
    CHECK(put_lines(tspec, com) == 0, "procedure written through RMS");

    static char out1[16384], out2[16384];
    char script[512];

    snprintf(script, sizeof script,
             "SET DEFAULT " DIR "\n@%s\n", tname);
    CHECK(run_dcl(script, out1, sizeof out1) == 0, "DCL ran the READ label procedure");
    printf("  ---- label procedure ----\n%s  ---- end ----\n", out1);
    CHECK(strstr(out1, "first=one") != NULL, "READ/END_OF_FILE=.. /ERROR=.. still reads a record normally");
    /* negctl: dcl-read-labels-ignored */
    CHECK(strstr(out1, "at-eof") != NULL,
          "READ/END_OF_FILE=EOFL branched to EOFL at end of file");
    /* negctl: dcl-read-labels-ignored */
    CHECK(strstr(out1, "NOT-REACHED") == NULL,
          "control did not fall through past the READ that hit end of file");
    CHECK(strstr(out1, "wrong-err") == NULL, "end of file went to /END_OF_FILE, not /ERROR");

    snprintf(script, sizeof script,
             "SET DEFAULT " DIR "\nREAD/BOGUS SYS$INPUT X\nREAD/DELETE SYS$INPUT X\n");
    CHECK(run_dcl(script, out2, sizeof out2) == 0, "DCL ran READ with unknown qualifiers");
    printf("  ---- bogus qualifiers ----\n%s  ---- end ----\n", out2);
    /* negctl: dcl-read-quals-accept-all */
    CHECK(strstr(out2, "IVQUAL") != NULL && strstr(out2, "\\BOGUS\\") != NULL,
          "READ/BOGUS is rejected as %DCL-W-IVQUAL naming the qualifier");
    /* negctl: dcl-read-quals-accept-all */
    CHECK(strstr(out2, "\\DELETE\\") != NULL,
          "READ/DELETE (not implemented) is rejected as %DCL-W-IVQUAL, not silently accepted");

    erase(fspec);
    erase(tspec);
    printf("=== test_syssvc_dcl_read: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
