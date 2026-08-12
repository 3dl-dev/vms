/*
 * test_sysroot_composed.c - VERACITY test proving the SYS$ system-disk logical
 * chain is COMPOSED LIVE through the concealed rooted search-list machinery,
 * not memorized as pre-flattened single-hop paths (vms-69e7, Engine B /
 * vms-ed7 composition depth).
 *
 * ============================================================
 * THE GAP THIS PROVES FIXED. On OpenVMS the system disk presents its logicals
 * as a composed chain rooted on a CONCEALED, ROOTED, SEARCH-LIST logical
 * (VSI OpenVMS System Manager's Manual, Vol. 1, "The System Disk"):
 *
 *   SYS$SYSROOT = dev:[SYS0.], dev:[SYS0.SYSCOMMON.]   (concealed rooted list)
 *   SYS$SYSTEM  = SYS$SYSROOT:[SYSEXE]
 *   SYS$LIBRARY = SYS$SYSROOT:[SYSLIB]   ... etc.
 *
 * A file under SYS$SYSTEM is looked up by MERGING [SYSEXE] onto each member of
 * SYS$SYSROOT and trying them in order: the node-specific root [SYS0.SYSEXE]
 * first, the common root [SYS0.SYSCOMMON.SYSEXE] second. Because every SYS$xxx
 * directory logical composes onto SYS$SYSROOT, redefining SYS$SYSROOT (or the
 * SYS$SYSDEVICE under it) RELOCATES all of them at once -- the observable proof
 * that the chain is derived at translation time, not baked in.
 *
 * Before vms-69e7, lnm_defaults.c seeded SYS$SYSROOT as a single, non-concealed
 * value and spelled SYS$SYSTEM out as "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]" --
 * a pre-flattened path that does NOT reference SYS$SYSROOT at all. On that
 * build the STRUCTURE assertions below fail (SYS$SYSROOT is neither a >=2-member
 * search list nor concealed; SYS$SYSTEM does not compose through SYS$SYSROOT),
 * and the LIVE-RELOCATION assertion fails (redefining SYS$SYSROOT cannot move a
 * pre-flattened SYS$SYSTEM). The assertions marked VERACITY are the
 * discriminators: they FAIL on the pre-fix build and PASS once the chain is
 * composed.
 *
 * Host-runnable: with no /dev/vms, lnm_setup_defaults() seeds into
 * LNM$PROCESS_TABLE (its disclosed host-tooling fallback), genuinely
 * process-private on VMS too, so the composition logic under test is exercised
 * without the executive.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"

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

/* Does `s` end with `suffix` (case-insensitive)? */
static int ci_endswith(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strncasecmp(s + (sl - fl), suffix, fl) == 0;
}

static int mkpath(const char *base, const char *rel)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    /* create each component */
    for (char *p = path + strlen(base) + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(path, 0755); *p = '/'; }
    }
    return mkdir(path, 0755);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_sysroot_composed (SYS$SYSROOT/SYS$SYSTEM composed live, vms-69e7) ===\n");

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* Seed the real shipped defaults (host fallback -> LNM$PROCESS_TABLE). */
    lnm_setup_defaults(mgr, NULL);

    char v[256];
    uint16_t vl = 0;
    uint32_t attrs = 0;

    /* ---- STRUCTURE 1: SYS$SYSROOT is a CONCEALED ROOTED SEARCH LIST whose
     * members are the node-specific root [SYS0.] and the common root
     * [SYS0.SYSCOMMON.]. Pre-flattened defaults make this a single,
     * non-concealed value -> FAIL. (VERACITY) */
    {
        char members[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        uint32_t sl_attr = 0;
        uint32_t st = lnm_translate_searchlist(mgr, "SYS$SYSROOT", members,
                                               LNM_MAX_SEARCHLIST, &n, &sl_attr);
        printf("  INFO: SYS$SYSROOT n=%u attr=0x%02x m0=\"%s\" m1=\"%s\"\n",
               n, sl_attr, n > 0 ? members[0] : "", n > 1 ? members[1] : "");
        CHECK($VMS_STATUS_SUCCESS(st) && n >= 2,
              "VERACITY: SYS$SYSROOT is a >=2-member search list");
        CHECK(sl_attr & LNM_ATTR_CONCEALED,
              "VERACITY: SYS$SYSROOT carries LNM$M_CONCEALED (a concealed device)");
        CHECK(n >= 1 && ci_endswith(members[0], "[SYS0.]"),
              "VERACITY: SYS$SYSROOT member 0 is the node-specific rooted root [SYS0.]");
        CHECK(n >= 2 && ci_contains(members[1], "SYSCOMMON") &&
              ci_endswith(members[1], ".]"),
              "VERACITY: SYS$SYSROOT member 1 is the common rooted root [...SYSCOMMON.]");
    }

    /* ---- STRUCTURE 2: SYS$SYSTEM / SYS$LIBRARY compose ONTO SYS$SYSROOT,
     * not a spelled-out [SYS0.SYSCOMMON.xxx] path. (VERACITY) */
    {
        uint32_t st = lnm_translate(mgr, LNM_FILE_DEV, "SYS$SYSTEM", v,
                                    sizeof(v), &vl, &attrs);
        v[vl] = '\0';
        printf("  INFO: SYS$SYSTEM -> \"%s\"\n", v);
        CHECK($VMS_STATUS_SUCCESS(st) && ci_contains(v, "SYS$SYSROOT") &&
              ci_contains(v, "SYSEXE"),
              "VERACITY: SYS$SYSTEM composes through SYS$SYSROOT:[SYSEXE]");
        CHECK(!ci_contains(v, "SYSCOMMON"),
              "VERACITY: SYS$SYSTEM is NOT a pre-flattened [SYS0.SYSCOMMON.SYSEXE] path");
    }
    {
        uint32_t st = lnm_translate(mgr, LNM_FILE_DEV, "SYS$LIBRARY", v,
                                    sizeof(v), &vl, &attrs);
        v[vl] = '\0';
        CHECK($VMS_STATUS_SUCCESS(st) && ci_contains(v, "SYS$SYSROOT") &&
              ci_contains(v, "SYSLIB"),
              "VERACITY: SYS$LIBRARY composes through SYS$SYSROOT:[SYSLIB]");
    }

    /* ---- STRUCTURE 3: DECC$LIBRARY_INCLUDE exists as a real search list. */
    {
        char members[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        uint32_t st = lnm_translate_searchlist(mgr, "DECC$LIBRARY_INCLUDE",
                                               members, LNM_MAX_SEARCHLIST,
                                               &n, NULL);
        CHECK($VMS_STATUS_SUCCESS(st) && n >= 2,
              "VERACITY: DECC$LIBRARY_INCLUDE is present as a >=2-member search list");
    }

    /* ---- LIVE RELOCATION: redefine SYS$SYSROOT to a fresh physical device and
     * show SYS$SYSTEM follows it, resolving through the concealed rooted
     * search-list fan-out to the COMMON member. Impossible with a pre-flattened
     * SYS$SYSTEM (which ignores SYS$SYSROOT entirely). (VERACITY) */
    {
        char base[] = "/tmp/ovmx69e_XXXXXX";
        if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }

        /* Physical device OVMX69E: -> base. The marker exists ONLY in the
         * COMMON member's SYSEXE ([RELO.SYSCOMMON.SYSEXE]), NOT the node member
         * ([RELO.SYSEXE]) -- so a correct fan-out must SKIP member 0 and land on
         * member 1. */
        (void)vmsfs_device_add("OVMX69E", base);
        mkpath(base, "relo/syscommon/sysexe");
        char marker[1024];
        snprintf(marker, sizeof(marker), "%s/relo/syscommon/sysexe/marker.dat", base);
        { FILE *f = fopen(marker, "w"); if (f) { fputs("x\n", f); fclose(f); } }

        /* Redefine SYS$SYSROOT (process scope, supersede) to point under the new
         * device -- a concealed rooted 2-member search list. SYS$SYSTEM is left
         * exactly as lnm_setup_defaults seeded it. */
        (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "SYS$SYSROOT", LNM_MODE_EXEC);
        static const char *relo[] = {
            "OVMX69E:[RELO.]", "OVMX69E:[RELO.SYSCOMMON.]"
        };
        uint32_t rc = lnm_create_multi(mgr, LNM_PROCESS_TABLE, "SYS$SYSROOT",
                                       relo, 2, LNM_ATTR_CONCEALED, LNM_MODE_EXEC);
        CHECK(rc == SS$_NORMAL || rc == SS$_SUPERSEDE,
              "redefined SYS$SYSROOT to the relocation device");

        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("SYS$SYSTEM:MARKER.DAT",
                                          resolved, sizeof(resolved));
        printf("  INFO: SYS$SYSTEM:MARKER.DAT -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st),
              "SYS$SYSTEM:MARKER.DAT translated after relocation");
        CHECK(ci_contains(resolved, base),
              "VERACITY: SYS$SYSTEM relocated LIVE with SYS$SYSROOT (composed, not pre-flattened)");
        CHECK(ci_contains(resolved, "relo/syscommon/sysexe/marker.dat"),
              "VERACITY: fan-out skipped the empty node root and resolved through the COMMON member");
        struct stat sb;
        CHECK(stat(resolved, &sb) == 0,
              "VERACITY: the composed physical path names the file that actually exists");

        /* device-only resolution of SYS$SYSTEM lands in the common SYSEXE dir. */
        char devdir[1024] = "";
        (void)vmsfs_resolve_device("SYS$SYSTEM", devdir, sizeof(devdir));
        printf("  INFO: resolve_device(SYS$SYSTEM) -> %s\n", devdir);
        CHECK(ci_contains(devdir, "relo/syscommon/sysexe"),
              "SYS$SYSTEM device resolves through the concealed rooted search list to the common root");

        (void)vmsfs_device_remove("OVMX69E");
    }

    printf("=== test_sysroot_composed: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
