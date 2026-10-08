/*
 * test_syssvc_spawn_complete.c - a /NOWAIT lib$spawn completion is NEVER lost, even
 * when the subprocess finishes (and is reclaimed) before the arm runs (vms-f45).
 *
 * THE BUG. lib$spawn(CLI$M_NOWAIT, efn) arms the completion in the executive AFTER
 * $CREPRC returns. A subprocess whose command ends at once (a DCL that reads an
 * empty/unreadable SYS$INPUT and exits) can be dead AND reclaimed by the executive's
 * lazy reaper before the arm ioctl looks it up; the ioctl then answered SS$_NONEXPR,
 * lib$spawn ignored it, and the caller's $WAITFR on its completion event flag hung
 * forever (corpus sys_forcex: rc=124 on ~half the boots of a loaded rail).
 *
 * THE PROPERTY. Whatever the subprocess's lifetime, the caller's completion event flag
 * is set. This suite spawns a command that ends immediately, N times, and requires the
 * flag to be set within a bounded wait every time. It can fail: against the unfixed
 * lib$spawn, any iteration that loses the race leaves the flag clear.
 *
 * Requires a real, insmod'd vms.ko; without one it SKIPs (77), never a fake pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>

#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "clidef.h"
#include "starlet.h"
#include "vms_kif.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

#define EXIT_SKIP 77
#define ITERATIONS 30
#define COMPLETION_EFN 11
#define WAIT_MS 20000

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static struct dsc$descriptor_s dsc(const char *s)
{
    struct dsc$descriptor_s d = { (unsigned short)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)s };
    return d;
}

int main(void)
{
    printf("=== test_syssvc_spawn_complete (vms-f45: /NOWAIT completion is never lost) ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    close(devfd);
    if (access("/bin/DCL.EXE", X_OK) != 0) { printf("  SKIP: no /bin/DCL.EXE staged\n"); return EXIT_SKIP; }

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  SKIP: no lnm manager\n"); return EXIT_SKIP; }
    CHECK(lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM", "/bin", 0, LNM_MODE_SUPER) & 1,
          "define SYS$SYSTEM -> /bin (stage DCL.EXE for lib$spawn)");
    if (!(vms_kif_open() >= 0 && (vms_kif_register(NULL) & 1))) { printf("  FAIL: register\n"); return 1; }

    int lost = 0;
    for (int i = 0; i < ITERATIONS; i++) {
        struct dsc$descriptor_s cmd = dsc("EXIT");
        uint32_t flags = CLI$M_NOWAIT, efn = COMPLETION_EFN, pid = 0;
        (void)sys$clref(efn);
        uint32_t r = lib$spawn(&cmd, NULL, NULL, &flags, NULL, &pid, NULL, &efn,
                               NULL, NULL, NULL, NULL, NULL);
        if (!(r & 1)) { printf("  FAIL: iteration %d: lib$spawn returned %08x\n", i, r); fail++; continue; }
        int set = 0;
        for (int waited = 0; waited < WAIT_MS && !set; waited += 50) {
            uint32_t st = sys$readef(efn, &(uint32_t){0});
            set = (st == SS$_WASSET);
            if (!set) { struct pollfd nothing = { .fd = -1, .events = 0 }; poll(&nothing, 1, 50); }
        }
        if (!set) lost++;
    }
    /* negctl: spawn-arm-gone-subprocess-not-completed */
    CHECK(lost == 0, "every /NOWAIT lib$spawn of an instantly-finishing command set its completion event flag");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
