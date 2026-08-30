/*
 * test_sysroot_pernode.c - vms-a01 mechanism proof: the SYS$SYSROOT / SYS$COMMON
 * seeds DERIVE the per-node system-root token instead of a hardcoded SYS0.
 *
 * Companion to test_sysroot_composed.c, which proves the DEFAULT (no
 * OVMX_SYSROOT_INDEX) still yields [SYS0.] and the concealed-rooted composition
 * is intact (the no-regression half). This program proves the FORCED half: with
 * the boot-root index published as OVMX_SYSROOT_INDEX=11 (the OVMX analog of the
 * VMS SYSBOOT R5 root; a real cluster derives it at boot from CLUSTER_CONFIG, not
 * an env var -- that production wiring is the deferred vms-a01 follow-on), the
 * node's SYS$SYSROOT member 0 becomes [SYS11.] and SYS$COMMON becomes
 * [SYS11.SYSCOMMON.], while the composition machinery (SYS$SYSTEM = SYS$SYSROOT:
 * [SYSEXE]) still resolves. Oracle: docs/oracle/vax73-system-root-logicals.md
 * (live VAX V7.3: node [SYS1] shows [SYS1.] + [SYS1.SYSCOMMON.]).
 *
 * The env MUST be set before lnm_setup_defaults() seeds -- lnm_defaults.c reads
 * OVMX_SYSROOT_INDEX at seed time (beside the existing OVMX_SYSDEVICE read).
 * Host-runnable: no /dev/vms, seeds into LNM$PROCESS_TABLE (disclosed fallback).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ssdef.h"
#include "vms/logical.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return 1;
    return 0;
}

static int ci_endswith(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strncasecmp(s + (sl - fl), suffix, fl) == 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_sysroot_pernode (SYS$SYSROOT derives the per-node root token, vms-a01) ===\n");

    /* Publish the boot-root index BEFORE seeding -- the mechanism under test. */
    setenv("OVMX_SYSROOT_INDEX", "11", 1);

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }
    lnm_setup_defaults(mgr, NULL);

    /* ---- SYS$SYSROOT: member 0 is the node root [SYS11.], and the token
     * really REPLACED the literal (no stray [SYS0.]). ---- */
    {
        char members[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        uint32_t sl_attr = 0;
        uint32_t st = lnm_translate_searchlist(mgr, "SYS$SYSROOT", members,
                                               LNM_MAX_SEARCHLIST, &n, &sl_attr);
        printf("  INFO: SYS$SYSROOT n=%u attr=0x%02x m0=\"%s\" m1=\"%s\"\n",
               n, sl_attr, n > 0 ? members[0] : "", n > 1 ? members[1] : "");
        CHECK($VMS_STATUS_SUCCESS(st) && n >= 2,
              "SYS$SYSROOT is still a >=2-member search list (structure intact)");
        CHECK(sl_attr & LNM_ATTR_CONCEALED,
              "SYS$SYSROOT still carries LNM$M_CONCEALED (concealed device)");
        CHECK(n >= 1 && ci_endswith(members[0], "[SYS11.]"),
              "MECHANISM: SYS$SYSROOT member 0 is the per-node root [SYS11.]");
        CHECK(n >= 2 && ci_contains(members[1], "[SYS11.SYSCOMMON."),
              "MECHANISM: SYS$SYSROOT member 1 common root carries the token [SYS11.SYSCOMMON.]");
        CHECK(!(n >= 1 && ci_contains(members[0], "[SYS0.")) &&
              !(n >= 2 && ci_contains(members[1], "[SYS0.")),
              "MECHANISM: the SYS0 literal was REPLACED, not appended (no [SYS0.] remains)");
    }

    /* ---- SYS$COMMON carries the token too: [SYS11.SYSCOMMON.] ---- */
    {
        char v[256]; uint16_t vl = 0; uint32_t attrs = 0;
        uint32_t st = lnm_translate(mgr, LNM_FILE_DEV, "SYS$COMMON", v,
                                    sizeof(v), &vl, &attrs);
        if (vl < sizeof(v)) v[vl] = '\0';
        printf("  INFO: SYS$COMMON -> \"%s\"\n", v);
        CHECK($VMS_STATUS_SUCCESS(st) && ci_contains(v, "[SYS11.SYSCOMMON.") &&
              !ci_contains(v, "[SYS0."),
              "MECHANISM: SYS$COMMON is [SYS11.SYSCOMMON.] (token, not SYS0)");
    }

    /* ---- No regression: SYS$SYSTEM still composes THROUGH SYS$SYSROOT
     * (not a pre-flattened path), proving the concealed-rooted machinery still
     * works with a forced token. ---- */
    {
        char v[256]; uint16_t vl = 0; uint32_t attrs = 0;
        uint32_t st = lnm_translate(mgr, LNM_FILE_DEV, "SYS$SYSTEM", v,
                                    sizeof(v), &vl, &attrs);
        if (vl < sizeof(v)) v[vl] = '\0';
        printf("  INFO: SYS$SYSTEM -> \"%s\"\n", v);
        CHECK($VMS_STATUS_SUCCESS(st) && ci_contains(v, "SYS$SYSROOT") &&
              ci_contains(v, "SYSEXE") && !ci_contains(v, "SYSCOMMON"),
              "NO-REGRESSION: SYS$SYSTEM still composes through SYS$SYSROOT:[SYSEXE]");
    }

    unsetenv("OVMX_SYSROOT_INDEX");
    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
