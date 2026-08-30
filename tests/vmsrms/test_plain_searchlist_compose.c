/*
 * test_plain_searchlist_compose.c - vms-656 (V0.5-2 boot blocker): prove a
 * PLAIN (non-rooted) multi-member search-list device logical -- STARTUP.COM's
 * SYS$STARTUP -- composes its FIRST member to the concrete on-volume ODS-2
 * candidate the Files-11 ACP directory walk consumes, so STARTUP.COM can open
 * SYS$STARTUP:VMS$PHASES.DAT during x86_64 boot.
 *
 * SCOPE. The actual V0.5-2 boot regression was NOT in this composition layer:
 * compose_ods2_candidates() already emits the correct member-0 candidate for a
 * plain search list (this test proves it). The regression was that the SHIPPED
 * native-link LIBVMSRMS$SHR.EXE was compiled WITHOUT -DOVMX_HAVE_ACP (mk_vmsrms_
 * shr.sh drifted when vms-d5d re-keyed rms_core.c's ACP arm from __linux__ to
 * OVMX_HAVE_ACP), so sys$open fell to the POSIX #else branch that cannot see a
 * file living only on the genuine ODS-2 volume -> %RMS-E-FNF. That build-flag
 * drift is guarded by tests/vmsrms/test_native_acp_arm_defined.sh; the boot
 * itself is proven by tests/qemu/test_distrib_boot.sh reaching Username:.
 *
 * This test guards the composition layer the ACP arm then consumes: a plain
 * (non-concealed, non-rooted) search list must still resolve its first member's
 * device/directory chain to the real on-volume path -- the member-0 chain the
 * boot open depends on.
 *
 * Runs on the host with no /dev/vms: the search list is defined in LNM$PROCESS,
 * which is genuinely process-private on VMS too (INV-6 is about not faking the
 * SHARED executive tables). The compose logic under test is table-agnostic.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int has_candidate(char cands[][VMSFS_MAX_FILESPEC], int n, const char *want)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(cands[i], want) == 0)
            return 1;
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_plain_searchlist_compose (SYS$STARTUP plain search-list -> "
           "ODS-2 member-0 candidate, vms-656) ===\n");

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* Seed the shipped system logicals (SYS$SYSDEVICE=VDA0:, SYS$MANAGER, ...) */
    lnm_setup_defaults(mgr, NULL);

    /* Reproduce STARTUP.COM:82 exactly: a PLAIN (non-concealed) two-member
     * search list -- neither member carries the CONCEALED attribute, so this is
     * NOT the rooted fan-out path test_dirlogical_compose already covers. */
    const char *members[2] = {
        "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYS$STARTUP]",
        "SYS$MANAGER"
    };
    uint32_t cst = lnm_create_multi(mgr, LNM_PROCESS_TABLE, "SYS$STARTUP",
                                    members, 2, 0, LNM_MODE_EXEC);
    CHECK(cst == SS$_NORMAL || cst == SS$_SUPERSEDE,
          "DEFINE SYS$STARTUP as a plain two-member search list succeeded");

    char cands[LNM_MAX_SEARCHLIST][VMSFS_MAX_FILESPEC];
    int n;

    /* ---- SYS$STARTUP:VMS$PHASES.DAT -> member-0 on-volume candidate -------- */
    n = vmsfs_compose_ods2_candidates("SYS$STARTUP:VMS$PHASES.DAT", cands,
                                      LNM_MAX_SEARCHLIST);
    printf("  INFO: SYS$STARTUP:VMS$PHASES.DAT -> %d candidate(s)\n", n);
    for (int i = 0; i < n; i++)
        printf("        [%d] %s\n", i, cands[i]);

    /* Member 0's chain (SYS$SYSDEVICE -> VDA0:, [SYS0.SYSCOMMON.SYS$STARTUP])
     * resolves to the file the boot OPEN targets -- note the '$' in both the
     * logical NAME and the directory COMPONENT resolve through cleanly. */
    CHECK(n >= 1,
          "a plain search list composes at least the first member");
    CHECK(has_candidate(cands, n, "VDA0:[SYS0.SYSCOMMON.SYS$STARTUP]VMS$PHASES.DAT"),
          "member-0 composes to VDA0:[SYS0.SYSCOMMON.SYS$STARTUP]VMS$PHASES.DAT (boot open target)");
    CHECK(n >= 1 &&
          strcasecmp(cands[0], "VDA0:[SYS0.SYSCOMMON.SYS$STARTUP]VMS$PHASES.DAT") == 0,
          "member-0 is FIRST (search order: the node/common member the file lives in wins)");

    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "SYS$STARTUP", LNM_MODE_EXEC);

    printf("=== test_plain_searchlist_compose: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
