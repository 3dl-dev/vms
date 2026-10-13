/*
 * test_syssvc_establish_once.c - the system identity is constructed once per
 * boot, for the boot task, and never again (rd vms-a2c, design note 3.1's
 * bootstrap exception).
 *
 * VMS system initialization creates the one system process; a process that
 * exists later gets its identity from its creator, never by asking for
 * SYSTEM. The harness boot task (ovmx_boot_task) has already made the one
 * call by the time this suite runs, and this suite is a process the boot
 * task created. So the call here is a second call: the executive refuses it
 * with SS$_NOPRIV, and the caller's row is unchanged.
 *
 * negctl: establish-system-not-once
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_establish_once: the system identity is constructed once per boot (rd vms-a2c) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_establish_once: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    struct vms_procinfo before, after;
    memset(&before, 0, sizeof(before));
    memset(&after, 0, sizeof(after));
    CHECK(vms_kif_getjpi_self(&before) & 1, "this process has a row its creator made");

    uint32_t st = vms_kif_establish_system();
    printf("  second ESTABLISH_SYSTEM: status %08X\n", (unsigned)st);
    /* negctl: establish-system-not-once */
    CHECK(st == SS$_NOPRIV,
          "a second ESTABLISH_SYSTEM (the boot task already made the one call) is refused SS$_NOPRIV");

    CHECK(vms_kif_getjpi_self(&after) & 1, "the row is still readable after the refused call");
    CHECK(after.uic == before.uic &&
              strncmp(after.username, before.username, sizeof(after.username)) == 0,
          "the refused call left the caller's user name and UIC as its creator gave them");

    printf("=== test_syssvc_establish_once: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
