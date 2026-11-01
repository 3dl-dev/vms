/*
 * test_syssvc_dcl_library_quals.c - DCL LIBRARY validates its qualifiers
 * (vms-097), driven through the shipped DCL.EXE in the guest.
 *
 * THE BUG THIS GUARDS. LIBRARY was an accept-all verb: LIBRARY/MACRO x.OLB or
 * a typo'd /CREAT silently dropped the qualifier and listed the library
 * instead. cmd_library now declares exactly the qualifiers its handlers act
 * on; /COMPRESS really rewrites an object library (to /OUTPUT), /VAX is honoured
 * only on the VAX rail; anything else is %DCL-W-IVQUAL.
 *
 * The object library lives on the ODS-2 volume (VDA0:[OVMXDIR]), written and
 * read back through RMS: LIBRARY's object I/O goes through the Files-11 ACP
 * (vms-8ca), not a POSIX path a logical names.
 *
 * With no executive (the negative-control rig) it exits EXIT_SKIP (77), the
 * honest-skip code that job requires of every public-sys$ suite.
 * NEGATIVE CONTROLS: tests/qemu/defects.d/dcl-library-quals-accept-all.defect and
 * dcl-library-compress-noop.defect.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <strings.h>

#include "starlet.h"
#include "ssdef.h"
#include "rms/rms.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"
#define RW_UNIT  "VDA0:"
#define LBDIR    RW_UNIT "[OVMXDIR]"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char *const dcl_env[] = { (char *)"PATH=/bin", NULL };

static int run_dcl(const char *script, char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];
    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) return -1;
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execle(DCL_PATH, "DCL.EXE", (char *)NULL, dcl_env);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* best effort */ }
    close(in_pipe[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/* Write `len` bytes as a new file through RMS (FIX, no record size: each $PUT
 * lands exactly its bytes). 0 on success. */
static int put_bytes(const char *spec, const char *buf, size_t len)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_FIX;
    fab.fab$b_fac = FAB$M_PUT;
    if (sys$create(&fab, 0, 0) != RMS$_NORMAL) return -1;
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    if (sys$connect(&rab, 0, 0) != RMS$_NORMAL) { sys$close(&fab, 0, 0); return -1; }
    rab.rab$l_rbf = (char *)buf;
    rab.rab$w_rsz = (uint16_t)len;
    uint32_t st = sys$put(&rab, 0, 0);
    uint32_t cst = sys$close(&fab, 0, 0);
    return (st == RMS$_NORMAL && (cst & 1)) ? 0 : -1;
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
    printf("=== test_syssvc_dcl_library_quals (LIBRARY qualifiers validated) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_library_quals: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_mount(RW_UNIT))) {
        printf("  FAIL: MOUNT " RW_UNIT "\n");
        return 1;
    }
    unsigned pid = (unsigned)getpid() % 100000u;
    char xobj[64], zolb[64], polb[64];
    snprintf(xobj, sizeof xobj, LBDIR "LBX%u.OBJ", pid);
    snprintf(zolb, sizeof zolb, LBDIR "LBZ%u.OLB", pid);
    snprintf(polb, sizeof polb, LBDIR "LBP%u.OLB", pid);
    if (put_bytes(xobj, "member\n", 7) != 0) { printf("  FAIL: write X.OBJ\n"); return 1; }

    static char out[16384];
    char script[1024];
    snprintf(script, sizeof script,
             "LIBRARY/ZQONE NOSUCH.TLB\nLIBRARY/MACRO NOSUCH.TLB\n"
             "LIBRARY/CREATE/OBJECT %s %s\n"
             "LIBRARY/OBJECT/COMPRESS=BLOCK=0/OUTPUT=%s %s\n"
             "LIBRARY/TEXT/COMPRESS NOSUCH.TLB\n"
             "LIBRARY/OBJECT/VAX NOSUCH.OLB\n", zolb, xobj, polb, zolb);
    CHECK(run_dcl(script, out, sizeof out) == 0, "DCL ran the LIBRARY probes");
    printf("  ---- LIBRARY probes ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-library-quals-accept-all */
    CHECK(strstr(out, "\\ZQONE\\") != NULL, "LIBRARY/ZQONE is %DCL-W-IVQUAL");
    /* negctl: dcl-library-quals-accept-all */
    CHECK(strstr(out, "\\MACRO\\") != NULL,
          "LIBRARY/MACRO (not implemented) is %DCL-W-IVQUAL, not silently dropped");
    CHECK(strstr(out, "LIBRARIAN-S-COMPRESSED") != NULL, "LIBRARY/OBJECT/COMPRESS reports the rewrite");
    uint8_t *pimg = NULL;
    uint64_t plen = 0;
    int found = rms_read_over_acp(polb, &pimg, &plen) == RMS$_NORMAL &&
                plen > 8 && memcmp(pimg, "!<arch>\n", 8) == 0;
    free(pimg);
    /* negctl: dcl-library-compress-noop */
    CHECK(found, "LIBRARY/COMPRESS/OUTPUT=P.OLB wrote the output library");
    CHECK(strstr(out, "\\COMPRESS\\") != NULL,
          "/COMPRESS on a TEXT library is %DCL-W-IVQUAL, not silently accepted");
    CHECK(strstr(out, "\\VAX\\") != NULL,
          "/VAX off the VAX rail is %DCL-W-IVQUAL, not a silently non-VAX library");

    erase(xobj); erase(zolb); erase(polb);
    printf("=== test_syssvc_dcl_library_quals: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
