/*
 * test_syssvc_getdvi.c - sys$getdvi / sys$device_scan are READERS of the
 * executive's device table, proven by calling the SHIPPED PUBLIC SERVICES
 * (not vms_kif, not DCL) against a real /dev/vms inside QEMU (vms-dv1).
 *
 * WHY THIS IS NOT test_syssvc_showdev.c. That suite proves the USER COMMAND
 * SHOW DEVICE is a reader of the executive table, by driving DCL.EXE. DCL
 * reaches vms_kif_* directly; it does NOT go through sys$getdvi/sys$device_scan
 * at all. Those two C-API services were, until vms-dv1, a per-process fake:
 * classify_device()+statvfs() on the host plus a compiled-in scan_devices[]
 * table that reported the same devices on every system and returned
 * SS$_NORMAL for names the executive had never heard of. This suite proves the
 * CONVERTED services now answer from the one executive I/O database -- the
 * thing a program that never touches DCL sees.
 *
 * THE DECISIVE TEST IS A-WRITES / B-READS, which is the reason for the fork()
 * below. Reading OPA0: from one process proves only that the row exists; a
 * fake patched to return a per-process view would satisfy it. So a SECOND
 * process (the child) allocates the console through the executive, and then
 * THIS process -- which did nothing to the device -- must observe the change
 * THROUGH THE PUBLIC sys$getdvi:
 *
 *   1. Before:  sys$getdvi(OPA0:, DVI$_DEVCHAR) has no DEV$M_ALL bit
 *   2. Child $ALLOCs OPA0: and stays alive holding it
 *   3. During:  sys$getdvi(OPA0:, DVI$_DEVCHAR) shows DEV$M_ALL   <-- B reads A
 *   4. Child exits; the executive releases the allocation with it
 *   5. After:   sys$getdvi(OPA0:, DVI$_DEVCHAR) has no DEV$M_ALL again
 *
 * Steps 3 and 5 are both required: a reader that always set the bit would pass
 * 3, and one that never did would pass 1 and 5. This is the same shape and the
 * same oracle basis (docs/oracle/vax73-terminal-device.md section 4.1 -- the
 * allocation column) test_syssvc_showdev.c uses, exercised through the C API
 * this item converted instead of through DCL.
 *
 * NEGATIVE CONTROL RIG (NEGATIVE_CONTROL=1 in tests/qemu/Dockerfile boots
 * without insmod'ing vms.ko): the executive is absent, which is not a product
 * state (vms-0ff -- PID 1 refuses to boot without one). This program then
 * asserts the property that survives regardless: the converted services do NOT
 * fabricate -- sys$getdvi does not return SS$_NORMAL for a device there is no
 * executive to confirm, and sys$device_scan does not enumerate the old fixed
 * list. It exits EXIT_SKIP, never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <stdint.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "descrip.h"
#include "dvidef.h"
#include "dcdef.h"
#include "dvsdef.h"
#include "gen64def.h"
#include "lnmdef.h"       /* struct item_list_3 */
#include "starlet.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

/* DEV$M_ALL -- the "allocated" device-characteristic bit sys_device.c derives
 * from the executive's allocation state. Must match sys_device.c. */
#define DEV$M_ALL    0x00000008

/* Bound on the child reporting its $ALLOC verdict. A failure bound, not
 * pacing: the child answers in milliseconds. Well inside run_tests.sh's QEMU
 * timeout, so a wedged child is a NAMED failure, not a VM timeout. */
#define REPORT_TIMEOUT_MS 30000

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Read one longword DVI item for a device by NAME through the PUBLIC service.
 * Returns the sys$getdvi status; *out receives the item value on success. */
static uint32_t getdvi_l(const char *devnam, uint16_t item_code, uint32_t *out)
{
    struct dsc$descriptor_s dev_d = {
        (uint16_t)strlen(devnam), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)devnam
    };
    uint32_t val = 0;
    struct item_list_3 il[2] = {
        { sizeof(uint32_t), item_code, &val, NULL },
        { 0, 0, NULL, NULL }
    };
    uint32_t st = sys$getdvi(0, 0, &dev_d, il, NULL, NULL, 0, 0);
    if (out) *out = val;
    return st;
}

/*
 * Read OPA0:'s allocation state through the PUBLIC sys$getdvi. Returns the
 * status; on success *allocated reports whether the DEV$M_ALL bit is set. The
 * status is returned SEPARATELY so a caller asserting "not allocated" cannot be
 * satisfied vacuously by a getdvi that FAILED -- a read that did not happen is
 * not the same fact as a device that is not allocated.
 */
static uint32_t opa0_alloc_state(int *allocated)
{
    uint32_t devchar = 0;
    uint32_t st = getdvi_l("OPA0:", (uint16_t)DVI$_DEVCHAR, &devchar);
    if (allocated) *allocated = (st == SS$_NORMAL) && (devchar & DEV$M_ALL);
    return st;
}

/* Does sys$device_scan('*') yield OPA0: and then terminate at SS$_NOMOREDEV?
 * Enumerates through the PUBLIC service only -- the test never builds a device
 * list of its own. */
static int scan_finds_opa0(int *terminated_nomore)
{
    char namebuf[64];
    struct dsc$descriptor_s name_d = {
        sizeof(namebuf) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, namebuf
    };
    struct dsc$descriptor_s wild_d = {
        1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"*"
    };
    GENERIC_64 ctx = { 0 };
    int found = 0, guard = 0;
    uint32_t st;

    *terminated_nomore = 0;
    while ((st = sys$device_scan(&name_d, &name_d.dsc$w_length,
                                 &wild_d, NULL, &ctx)) == SS$_NORMAL) {
        namebuf[name_d.dsc$w_length] = '\0';
        if (strncmp(namebuf, "OPA0", 4) == 0)
            found = 1;
        name_d.dsc$w_length = sizeof(namebuf) - 1;
        if (++guard >= 100) break;
    }
    *terminated_nomore = (st == SS$_NOMOREDEV);
    return found;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int pipefd[2], stopfd[2];
    pid_t child;

    printf("=== test_syssvc_getdvi (sys$getdvi/sys$device_scan read the executive table) ===\n");

    if (vms_kif_open() < 0) {
        /*
         * NEGATIVE-CONTROL RIG ONLY (no vms.ko). Not a product state. Assert
         * the property that holds regardless: the converted services do not
         * fabricate. Deliberately pins no status value.
         */
        uint32_t devclass = 0xdead;
        uint32_t st = getdvi_l("SYS$DISK:", (uint16_t)DVI$_DEVCLASS, &devclass);
        CHECK(st != SS$_NORMAL,
              "sys$getdvi does not fabricate a SYS$DISK answer with no executive");

        int term = 0;
        int found = scan_finds_opa0(&term);
        CHECK(!found,
              "sys$device_scan does not enumerate a fabricated device list with no executive");

        printf("=== test_syssvc_getdvi: %d passed, %d failed (SKIPPED: no /dev/vms -- the A-writes/B-reads case was not exercised, the no-fabrication checks WERE) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ---- 1. The executive's real device is enumerated and readable ---- */
    int term = 0;
    /* negctl-knockon: devtab-devscan-found-status-wrong */
    /* negctl-knockon: bind-client-no-register */
    CHECK(scan_finds_opa0(&term),
          "device scan lists the console terminal");
    /* negctl: devtab-devscan-found-status-wrong */
    /* negctl-knockon: bind-client-no-register */
    CHECK(term,
          "device scan terminates with SS$_NOMOREDEV");

    uint32_t devclass = 0;
    uint32_t gst = getdvi_l("OPA0:", (uint16_t)DVI$_DEVCLASS, &devclass);
    /* negctl-knockon: devtab-getdvi-devnam-status-wrong */
    /* negctl-knockon: bind-client-no-register */
    CHECK(gst == SS$_NORMAL && devclass == DC$_TERM,
          "this process can still read the device");

    /* A device the executive does not have is refused, not fabricated. (This
     * assertion holds in every rig -- ZZA0: is never a real row -- so it is
     * anchored to no defect: nothing can make it go red, and that is fine.) */
    uint32_t zzclass = 0;
    CHECK(getdvi_l("ZZA0:", (uint16_t)DVI$_DEVCLASS, &zzclass) != SS$_NORMAL,
          "sys$getdvi refuses a device the executive does not have");

    /* ---- 2. Before: the console reads back, and it is not allocated ---- */
    int alloc_before = 0;
    uint32_t st_before = opa0_alloc_state(&alloc_before);
    /* negctl-knockon: devtab-getdvi-devnam-status-wrong */
    /* negctl-knockon: bind-client-no-register */
    CHECK(st_before == SS$_NORMAL && !alloc_before,
          "the console is not allocated before any process allocates it");

    /* ---- 3. A SECOND PROCESS allocates the console ---- */
    if (pipe(pipefd) != 0 || pipe(stopfd) != 0) {
        printf("  FAIL: pipe() failed\n");
        printf("=== test_syssvc_getdvi: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    child = fork();
    if (child == 0) {
        /*
         * PROCESS A. A distinct Linux process, therefore a distinct process to
         * the executive (vms_kif.c keys registration on the tgid), so anything
         * it changes is visible to this process's sys$getdvi only if the device
         * table is genuinely shared. It holds the allocation by staying alive;
         * the executive releases it on exit (vms_proc_release_channels).
         */
        uint32_t st;
        char c;
        close(pipefd[0]);
        close(stopfd[1]);
        st = vms_kif_alloc("OPA0:");
        if (write(pipefd[1], (st & 1) ? "Y" : "N", 1) != 1)
            _exit(2);
        while (read(stopfd[0], &c, 1) < 0)
            ;
        _exit(0);
    }
    if (child < 0) {
        printf("  FAIL: fork() failed\n");
        printf("=== test_syssvc_getdvi: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }
    close(pipefd[1]);
    close(stopfd[0]);

    {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        char verdict = '?';
        int pr = poll(&pfd, 1, REPORT_TIMEOUT_MS);
        /* negctl-knockon: bind-client-no-register */
        if (pr > 0 && read(pipefd[0], &verdict, 1) == 1)
            CHECK(verdict == 'Y',
                  "the second process allocated OPA0: through the executive ($ALLOC)");
        else
            CHECK(0, "the second process never reported whether $ALLOC succeeded");
    }

    /* ---- 4. During: this process observes A's allocation via sys$getdvi ---- */
    int alloc_during = 0;
    uint32_t st_during = opa0_alloc_state(&alloc_during);
    /* negctl: devtab-alloc-not-recorded */
    /* negctl: devtab-getdvi-userspace-alloc-dropped */
    /* negctl-knockon: devtab-getdvi-devnam-status-wrong */
    /* negctl-knockon: bind-client-no-register */
    CHECK(st_during == SS$_NORMAL && alloc_during,
          "B sees that A allocated the device");

    /* ---- 5. After: the allocation ends and this process observes THAT ---- */
    close(stopfd[1]);
    waitpid(child, NULL, 0);
    close(pipefd[0]);

    int alloc_after = 0;
    uint32_t st_after = opa0_alloc_state(&alloc_after);
    /* negctl-knockon: devtab-getdvi-devnam-status-wrong */
    /* negctl-knockon: bind-client-no-register */
    CHECK(st_after == SS$_NORMAL && !alloc_after,
          "the allocation is gone once the other process exits (not a constant)");

    printf("=== test_syssvc_getdvi: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
