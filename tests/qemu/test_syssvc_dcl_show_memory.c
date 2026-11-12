/*
 * test_syssvc_dcl_show_memory.c - SHOW MEMORY reads physical memory from the
 * executive's $GETSYI memory reply, and the figures are the system's real
 * ones (rd vms-b14e4, vms-fe21), driven through the shipped DCL.EXE in the
 * guest against a real /dev/vms.
 *
 * THE BUG THIS GUARDS. SHOW MEMORY on Linux read /proc/meminfo and
 * /proc/swaps itself -- host libc file I/O behind the executive's back. It now
 * calls vms_kif_getsyi_memory (VMS_IOCTL_GETSYIMEM), which Linux vms.ko
 * answers from si_meminfo. This suite proves that path on the real runtime:
 *   1. vms_kif_getsyi_memory succeeds and marks the figures real;
 *   2. its total is the guest's memory size, cross-checked independently via
 *      sysinfo(2) within 2% (a hardcoded or stubbed value misses by an
 *      arbitrary amount -- the vms-fe21 check, moved here from the host test,
 *      which has no executive);
 *   3. SHOW MEMORY's Main Memory row carries that same total, and
 *      In Use = Total - Free;
 *   4. the Paging File Usage section is not fabricated (rd vms-0d0).
 *
 * With no executive it exits EXIT_SKIP (77), the honest-skip code that job
 * requires of every public-sys$ suite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

#define EXIT_SKIP 77
#define DCL_PATH "/bin/DCL.EXE"

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

static long long llabs_diff(long long a, long long b) { return a > b ? a - b : b - a; }

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_show_memory (SHOW MEMORY reads the executive's $GETSYI memory reply) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_show_memory: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    /* 1. The executive answers. */
    struct vms_syi_meminfo mi;
    memset(&mi, 0, sizeof mi);
    uint32_t st = vms_kif_getsyi_memory(&mi);
    printf("  getsyi_memory: status=0x%x valid=0x%x total=%llu free=%llu\n", st,
           mi.fields_valid, (unsigned long long)mi.total_bytes,
           (unsigned long long)mi.free_bytes);
    CHECK((st & 1) && (mi.fields_valid & VMS_SYIMEM_V_PHYS),
          "the executive answers VMS_IOCTL_GETSYIMEM with real physical-memory figures");
    CHECK(mi.free_bytes > 0 && mi.free_bytes < mi.total_bytes,
          "free memory is nonzero and below total");

    /* 2. Independent cross-check of the total (2%). */
    struct sysinfo si;
    CHECK(sysinfo(&si) == 0, "sysinfo(2) reads the guest's memory size");
    long long real_pages = (long long)si.totalram * si.mem_unit / 512;
    long long exec_pages = (long long)(mi.total_bytes / 512ULL);
    /* negctl: exec-getsyimem-fabricated-total */
    CHECK(real_pages > 0 && llabs_diff(exec_pages, real_pages) <= real_pages / 50,
          "the executive's total matches the guest's memory size within 2%");

    /* 3. SHOW MEMORY renders the executive's figures. */
    static char out[16384];
    CHECK(run_dcl("SHOW MEMORY\n", out, sizeof out) == 0, "DCL ran SHOW MEMORY");
    printf("  ---- SHOW MEMORY ----\n%s  ---- end ----\n", out);
    const char *row = strstr(out, "Main Memory (");
    long long total = -1, freep = -1, inuse = -1;
    if (row) {
        const char *p = strchr(row, ')');
        if (p && sscanf(p + 1, "%lld %lld %lld", &total, &freep, &inuse) != 3)
            total = -1;
    }
    CHECK(row != NULL && total > 0, "SHOW MEMORY prints a Main Memory row with a page total");
    /* negctl: exec-getsyimem-fabricated-total */
    CHECK(total > 0 && llabs_diff(total, real_pages) <= real_pages / 50,
          "the Main Memory total is the guest's memory size within 2% (vms-fe21)");
    CHECK(total > 0 && inuse == total - freep && freep > 0,
          "In Use = Total - Free, Free nonzero");

    /* 4. No fabricated sections. */
    CHECK(strstr(out, "Paging File Usage") == NULL,
          "no Paging File Usage section (the executive keeps no page-file accounting, rd vms-0d0)");
    CHECK(strstr(out, "Modified List Size") == NULL,
          "no Modified List Size line (the retired buffers+cached masquerade, vms-31a)");

    printf("=== test_syssvc_dcl_show_memory: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
