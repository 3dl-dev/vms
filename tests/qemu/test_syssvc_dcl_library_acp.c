/*
 * test_syssvc_dcl_library_acp.c - DCL LIBRARY reads and writes an OBJECT
 * library, and the objects it is given, THROUGH the Files-11 ACP (vms-8ca),
 * driven through the shipped DCL.EXE against a real /dev/vms.
 *
 * THE BUG THIS GUARDS. The GCC port's `ar` (gcc/config/vms/vms-ar.c) runs
 *     $ library /replace/create libx.a -
 *     a.o,-
 *     b.o
 * over objects the compiler just wrote through RMS. LIBRARY's OBJECT path used a
 * POSIX fopen() of the translated path, which on the product runtime is not where
 * the volume's files are: it answered %LIBRARIAN-E-OPENIN for an object that
 * exists, and the native multi-file build stopped at the archive. DUMP had the
 * same bug (vms-5303e); LIBRARIAN.EXE and LINK.EXE already go through RMS.
 *
 * WHAT THIS PROVES (against real /dev/vms):
 *   1. LIBRARY/REPLACE/CREATE x.OLB a.O,b.O reads two objects written by RMS and
 *      writes the library through the ACP: read back over the ACP it is an `ar`
 *      container holding both objects' bytes exactly (odd length, a NUL).
 *   2. LIBRARY/LIST reads that library back through the ACP (both modules).
 *   3. LIBRARY/EXTRACT=m/OUTPUT=f writes the module through the ACP, byte-exact.
 *   4. FAIL-HONEST: a library naming an object that does not exist is
 *      %LIBRARIAN-E-OPENIN, and no library is created.
 * With no executive it exits EXIT_SKIP (77).
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-library-object-posix-io.defect.
 *
 * ISOLATION: files use a pid-unique name in VDA0:[OVMXDIR] and are ERASED.
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

#define EXIT_SKIP 77
#define DCL_PATH  "/bin/DCL.EXE"
#define RW_UNIT   "VDA0:"
#define DIR       RW_UNIT "[OVMXDIR]"

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
 * lands exactly its bytes) -- how GNU as / LINK.EXE put an object on the volume. */
static int put_bytes(const char *spec, const unsigned char *buf, size_t len)
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

static int has_bytes(const uint8_t *hay, uint64_t hlen, const unsigned char *n, size_t nlen)
{
    if (!hay || nlen > hlen) return 0;
    for (uint64_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, n, nlen) == 0) return 1;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_library_acp (LIBRARY object I/O over the ACP) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_library_acp: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount(RW_UNIT)), "MOUNT " RW_UNIT " (writable)");

    unsigned pid = (unsigned)getpid() % 100000u;
    char moda[16], modb[16], obja[64], objb[64], lib[64], xout[64], nolib[64];
    snprintf(moda, sizeof moda, "LBQA%u", pid);
    snprintf(modb, sizeof modb, "LBQB%u", pid);
    snprintf(obja, sizeof obja, DIR "%s.O", moda);
    snprintf(objb, sizeof objb, DIR "%s.O", modb);
    snprintf(lib, sizeof lib, DIR "LBQ%u.OLB", pid);
    snprintf(xout, sizeof xout, DIR "LBQX%u.O", pid);
    snprintf(nolib, sizeof nolib, DIR "LBQN%u.OLB", pid);

    /* Two "objects": distinct bytes, one odd-length, one carrying a NUL. */
    static const unsigned char pa[] = "OBJ-A payload through RMS\x01\x02\x03";
    static const unsigned char pb[] = { 'O','B','J','-','B',0,'n','u','l',0xFF,'b' };
    CHECK(put_bytes(obja, pa, sizeof pa - 1) == 0, "object A written through RMS");
    CHECK(put_bytes(objb, pb, sizeof pb) == 0, "object B written through RMS");

    static char script[1024], out[16384];
    snprintf(script, sizeof script,
             "LIBRARY/REPLACE/CREATE %s %s,%s\n"
             "LIBRARY/LIST %s\n"
             "LIBRARY/EXTRACT=%s/OUTPUT=%s %s\n"
             "LIBRARY/REPLACE/CREATE %s %sNOSUCH%u.O\n",
             lib, obja, objb, lib, modb, xout, lib, nolib, DIR, pid);
    CHECK(run_dcl(script, out, sizeof out) == 0, "DCL ran the LIBRARY build");
    printf("  ---- DCL ----\n%s  ---- end ----\n", out);

    /* negctl: dcl-library-object-posix-io */
    CHECK(strstr(out, "error opening object file - " DIR "LBQA") == NULL &&
          strstr(out, "error opening object file - " DIR "LBQB") == NULL,
          "LIBRARY opened both RMS-written objects (no OPENIN)");

    uint8_t *img = NULL;
    uint64_t ilen = 0;
    uint32_t rst = rms_read_over_acp(lib, &img, &ilen);
    /* negctl: dcl-library-object-posix-io */
    CHECK(rst == RMS$_NORMAL && ilen > 8 && memcmp(img, "!<arch>\n", 8) == 0,
          "the library is on the volume (read over the ACP) and is an ar container");
    /* negctl: dcl-library-object-posix-io */
    CHECK(has_bytes(img, ilen, pa, sizeof pa - 1) && has_bytes(img, ilen, pb, sizeof pb),
          "the library holds both objects' bytes exactly");
    free(img);

    /* negctl: dcl-library-object-posix-io */
    CHECK(strstr(out, moda) != NULL && strstr(out, modb) != NULL &&
          strstr(out, "2 modules in library") != NULL,
          "LIBRARY/LIST read the library back over the ACP: both modules");

    uint8_t *xb = NULL;
    uint64_t xlen = 0;
    rst = rms_read_over_acp(xout, &xb, &xlen);
    /* negctl: dcl-library-object-posix-io */
    CHECK(rst == RMS$_NORMAL && xlen == sizeof pb && memcmp(xb, pb, sizeof pb) == 0,
          "LIBRARY/EXTRACT/OUTPUT wrote the module over the ACP, byte-exact");
    free(xb);

    uint8_t *nb = NULL;
    uint64_t nlen = 0;
    rst = rms_read_over_acp(nolib, &nb, &nlen);
    CHECK(strstr(out, "error opening object file - " DIR "NOSUCH") != NULL &&
          rst != RMS$_NORMAL,
          "a missing object is %LIBRARIAN-E-OPENIN and no library is created (fail-honest)");
    free(nb);

    erase(obja); erase(objb); erase(lib); erase(xout); erase(nolib);
    printf("=== test_syssvc_dcl_library_acp: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
