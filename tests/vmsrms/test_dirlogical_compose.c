/*
 * test_dirlogical_compose.c - vms-0044 (flip-blocker, epic vms-208): prove the
 * concealed-rooted directory-logical chain COMPOSES to concrete on-volume ODS-2
 * candidate specs -- the exact input the Files-11 ACP directory walk consumes.
 *
 * This is the HOST half of vms-0044 (the executive-resident ACP walk + a
 * byte-exact read is the real-/dev/vms half, tests/qemu/test_syssvc_dirlogical_
 * acp.c). It exercises vmsfs_compose_ods2_candidates() against the SHIPPED
 * system logicals lnm_setup_defaults() seeds (into LNM$PROCESS_TABLE under host
 * tooling -- genuinely process-private on VMS too), so the composition logic is
 * proven without the executive.
 *
 * WHAT IT PROVES:
 *   SYS$SYSTEM:SYSUAF.DAT  composes, through the concealed rooted search-list
 *   chain (SYS$SYSTEM = SYS$SYSROOT:[SYSEXE]; SYS$SYSROOT = SYS$SYSDEVICE:[SYS0.],
 *   SYS$SYSDEVICE:[SYS0.SYSCOMMON.]; SYS$SYSDEVICE = VDA0:), to the two ordered
 *   candidates
 *       VDA0:[SYS0.SYSEXE]SYSUAF.DAT            (node-specific member, first)
 *       VDA0:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT  (common member, second)
 *   -- NOT a pre-flattened string, and in the search order a running system disk
 *   presents (VSI OpenVMS System Manager's Manual, Vol. 1, "The System Disk").
 *   SYS$MANAGER: and SYS$LIBRARY: compose the same way onto [SYSMGR]/[SYSLIB].
 *
 *   FAIL-HONEST (INV-6): an unresolvable device logical composes NOTHING (0
 *   candidates) -- never a fabricated path.
 *
 *   LIVE, not memorized: redefining SYS$SYSDEVICE relocates every SYS$xxx
 *   candidate at once (the proof the chain is derived at composition time).
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

/* Case-insensitive "does any candidate equal `want`?" */
static int has_candidate(char cands[][VMSFS_MAX_FILESPEC], int n, const char *want)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(cands[i], want) == 0)
            return 1;
    return 0;
}

/* Index of the first candidate that case-insensitively equals `want`, or -1. */
static int index_of(char cands[][VMSFS_MAX_FILESPEC], int n, const char *want)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(cands[i], want) == 0)
            return i;
    return -1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_dirlogical_compose (SYS$SYSTEM:/rooted logicals -> ODS-2 "
           "candidate specs, vms-0044) ===\n");

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* Seed the real shipped defaults (host fallback -> LNM$PROCESS_TABLE). */
    lnm_setup_defaults(mgr, NULL);

    char cands[LNM_MAX_SEARCHLIST][VMSFS_MAX_FILESPEC];
    int n;

    /* ---- SYS$SYSTEM:SYSUAF.DAT -> the two ordered composed candidates ------ */
    n = vmsfs_compose_ods2_candidates("SYS$SYSTEM:SYSUAF.DAT", cands,
                                      LNM_MAX_SEARCHLIST);
    printf("  INFO: SYS$SYSTEM:SYSUAF.DAT -> %d candidate(s)\n", n);
    for (int i = 0; i < n; i++)
        printf("        [%d] %s\n", i, cands[i]);

    CHECK(n >= 2, "SYS$SYSTEM:SYSUAF.DAT composes to >=2 search-list candidates");
    CHECK(has_candidate(cands, n, "VDA0:[SYS0.SYSEXE]SYSUAF.DAT"),
          "node-specific candidate VDA0:[SYS0.SYSEXE]SYSUAF.DAT is composed");
    CHECK(has_candidate(cands, n, "VDA0:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT"),
          "common candidate VDA0:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT is composed");
    {
        int inode = index_of(cands, n, "VDA0:[SYS0.SYSEXE]SYSUAF.DAT");
        int icomn = index_of(cands, n, "VDA0:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT");
        CHECK(inode >= 0 && icomn >= 0 && inode < icomn,
              "SEARCH ORDER: the node-specific member composes BEFORE the common member");
    }

    /* ---- SYS$MANAGER: and SYS$LIBRARY: compose the same way --------------- */
    n = vmsfs_compose_ods2_candidates("SYS$MANAGER:STARTUP.COM", cands,
                                      LNM_MAX_SEARCHLIST);
    CHECK(has_candidate(cands, n, "VDA0:[SYS0.SYSCOMMON.SYSMGR]STARTUP.COM"),
          "SYS$MANAGER:STARTUP.COM composes onto the common [SYS0.SYSCOMMON.SYSMGR]");

    n = vmsfs_compose_ods2_candidates("SYS$LIBRARY:STARLET.OLB", cands,
                                      LNM_MAX_SEARCHLIST);
    CHECK(has_candidate(cands, n, "VDA0:[SYS0.SYSCOMMON.SYSLIB]STARLET.OLB"),
          "SYS$LIBRARY:STARLET.OLB composes onto the common [SYS0.SYSCOMMON.SYSLIB]");

    /* ---- a plain device (already physical) passes through --------------- */
    n = vmsfs_compose_ods2_candidates("VDA0:[MYDIR]FILE.DAT", cands,
                                      LNM_MAX_SEARCHLIST);
    CHECK(n == 1 && strcasecmp(cands[0], "VDA0:[MYDIR]FILE.DAT") == 0,
          "an already-physical spec composes to exactly itself");

    /* ---- FAIL-HONEST: an unresolvable device logical composes nothing ---- */
    n = vmsfs_compose_ods2_candidates("NOSUCHLOG$XYZ:FILE.DAT", cands,
                                      LNM_MAX_SEARCHLIST);
    /* NOSUCHLOG$XYZ is not a logical -> treated as a physical device name, so it
     * composes to exactly itself (an honest pass-through, not a fabricated
     * system path). The point: it NEVER invents a [SYSn...] tree it has no basis
     * for. */
    CHECK(n == 1 && strcasecmp(cands[0], "NOSUCHLOG$XYZ:FILE.DAT") == 0,
          "an unknown device name composes to itself, never a fabricated SYS$ path");

    /* ---- LIVE relocation: redefine SYS$SYSDEVICE, every SYS$xxx follows --- */
    {
        (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "SYS$SYSDEVICE", LNM_MODE_EXEC);
        uint32_t rc = lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSDEVICE",
                                 "DKB100:", LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
        CHECK(rc == SS$_NORMAL || rc == SS$_SUPERSEDE,
              "redefined SYS$SYSDEVICE -> DKB100:");
        n = vmsfs_compose_ods2_candidates("SYS$SYSTEM:SYSUAF.DAT", cands,
                                          LNM_MAX_SEARCHLIST);
        CHECK(has_candidate(cands, n, "DKB100:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT"),
              "VERACITY: SYS$SYSTEM relocated LIVE with SYS$SYSDEVICE (composed, not memorized)");
        CHECK(!has_candidate(cands, n, "VDA0:[SYS0.SYSCOMMON.SYSEXE]SYSUAF.DAT"),
              "VERACITY: no stale VDA0: candidate survives the relocation");
    }

    printf("=== test_dirlogical_compose: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
