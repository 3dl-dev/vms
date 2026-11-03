/*
 * test_syssvc_dcl_allocate.c - DCL ALLOCATE / DEALLOCATE, driven through the shipped
 * DCL.EXE in the guest, against the executive's device table ($ALLOC / $DALLOC).
 *
 * What the OpenVMS VAX V7.3 console showed (docs/oracle/vax73-terminal-device.md):
 *   ALLOCATE OPA0:     -> "%DCL-I-ALLOC, _VAX1$OPA0: allocated", and SHOW DEVICE then
 *                         reads "Online alloc" (section 4);
 *   ALLOCATE OPA0: while another process holds it
 *                      -> "%SYSTEM-W-DEVALLOC, device already allocated to another
 *                         user" (section 7.2).
 * The name echoed is the device's FULL name (rd vms-1f7): VMS prints _VAX1$OPA0: on a
 * standalone node as on a clustered one (docs/oracle/vax73-device-full-name.md), so
 * OVMX's is "_" + the executive's full name -- "_<SCSNODE>$OPA0:" once STARTUP has
 * loaded SCSNODE, "_OPA0:" on an executive that has none to prefix. The test reads
 * the expected node from the executive ($GETSYI SYI$_SCSNODE's source) and checks
 * the composition itself, so neither form passes for the other.
 * A logical-name parameter becomes a process logical name for the physical name;
 * DEALLOCATE gives the device back silently, and DEALLOCATE of a device this process
 * has not allocated is $DALLOC's SS$_DEVNOTALLOC.
 *
 * No /dev/vms -> honest SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"

/* src/libvms/syssvc/sys_misc.c: the SCSNODE the executive booted with. */
int ovmx_booted_scsnode(char *out, size_t out_len);

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

int main(void)
{
    static char out[16384];

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_allocate (DCL ALLOCATE / DEALLOCATE over $ALLOC / $DALLOC) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_allocate: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }

    /* 0. The executive's full name for OPA0: is <SCSNODE>$OPA0: (DEV$M_NNM). */
    char node[16] = "", full[64], alloc_line[96], lnm_line[80];
    struct vms_devinfo di;
    memset(&di, 0, sizeof di);
    CHECK(vms_kif_getdvi_devnam("OPA0:", &di) & 1, "$GETDVI reaches the executive's OPA0: row");
    di.fulldevnam[VMS_FULLDEVNAM_SIZE - 1] = '\0';
    if (!(ovmx_booted_scsnode(node, sizeof node) && node[0]))
        node[0] = '\0';
    snprintf(full, sizeof full, "%s%s%s", node, node[0] ? "$" : "", "OPA0:");
    printf("  executive SCSNODE '%s', OPA0: full name '%s'\n", node, di.fulldevnam);
    CHECK(strcmp(di.fulldevnam, full) == 0,
          "the executive's full name for OPA0: is <SCSNODE>$OPA0: (bare OPA0: only with no SCSNODE loaded)");
    snprintf(alloc_line, sizeof alloc_line, "%%DCL-I-ALLOC, _%s allocated", full);
    snprintf(lnm_line, sizeof lnm_line, "LNM=_%s", full);

    /* 1. Allocate, see it, name it, give it back. */
    CHECK(run_dcl("ALLOCATE OPA0: ALLOCTERM\n"
                  "SHOW DEVICE OPA0:\n"
                  "WRITE SYS$OUTPUT \"LNM=\" + F$TRNLNM(\"ALLOCTERM\")\n"
                  "DEALLOCATE OPA0:\n"
                  "WRITE SYS$OUTPUT \"--AFTER--\"\n"
                  "SHOW DEVICE OPA0:\n", out, sizeof out) == 0,
          "DCL ran the ALLOCATE / DEALLOCATE probe");
    printf("  ---- ALLOCATE probe ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-alloc-msg-to-substrate-stderr */
    CHECK(strstr(out, alloc_line) != NULL,
          "ALLOCATE OPA0: reports %DCL-I-ALLOC, _<full name> allocated");
    /* negctl: dcl-allocate-skips-alloc */
    CHECK(strstr(out, "Online alloc") != NULL,
          "SHOW DEVICE then reads Online alloc (the executive holds the allocation)");
    CHECK(strstr(out, lnm_line) != NULL,
          "the logical-name parameter names the device by its full name");
    {
        const char *after = strstr(out, "--AFTER--");
        /* negctl: dcl-deallocate-skips-dalloc */
        CHECK(after != NULL && strstr(after, "Online alloc") == NULL && strstr(after, "Online") != NULL,
              "after DEALLOCATE OPA0: SHOW DEVICE reads plain Online");
    }

    /* 2. Another process holds the device: ALLOCATE is refused with $ALLOC's condition. */
    CHECK(vms_kif_alloc("OPA0:") & 1, "this process allocates OPA0: ($ALLOC)");
    CHECK(run_dcl("ALLOCATE OPA0:\n", out, sizeof out) == 0,
          "DCL ran the refused-ALLOCATE probe");
    printf("  ---- refused ALLOCATE ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-allocate-skips-alloc */
    CHECK(strstr(out, "%SYSTEM-W-DEVALLOC, device already allocated to another user") != NULL &&
          strstr(out, "%DCL-I-ALLOC") == NULL,
          "ALLOCATE of a device another process holds is %SYSTEM-W-DEVALLOC");
    (void)vms_kif_dalloc("OPA0:");

    /* 3. DEALLOCATE of a device this process never allocated. */
    CHECK(run_dcl("DEALLOCATE OPA0:\n", out, sizeof out) == 0, "DCL ran the DEALLOCATE probe");
    printf("  ---- DEALLOCATE unallocated ----\n%s  ---- end ----\n", out);
    /* negctl: dcl-deallocate-skips-dalloc */
    CHECK(strstr(out, "DEVNOTALLOC, device not allocated") != NULL,
          "DEALLOCATE of a device not allocated is SS$_DEVNOTALLOC");

    printf("=== test_syssvc_dcl_allocate: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
