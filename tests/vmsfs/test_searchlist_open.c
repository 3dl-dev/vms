/*
 * test_searchlist_open.c - VERACITY test for search-list device logicals on
 * file open (vms-b12, Engine B / vms-ed7).
 *
 * ============================================================
 * THE GAP THIS PROVES FIXED. A multi-valued (search-list) logical used in the
 * device position of a filespec must, on $OPEN/$SEARCH, be tried EQUIVALENCE
 * BY EQUIVALENCE, IN ORDER, until one names an existing file -- this is the
 * defining behavior of a VMS search list (VMS I/O User's Reference Manual,
 * "Logical Name Search Lists"; DCL Dictionary, DEFINE; it is how the C RTL
 * finds a header through DECC$LIBRARY_INCLUDE = a search list of include
 * roots). Before vms-b12, vmsfs_to_linux_path() collapsed the device logical
 * to equivalence index 0 (lnm_translate() returns only index 0), so a file
 * that lived in the SECOND member of the list was invisible: OPEN resolved to
 * the first member's path, where the file does not exist, and failed FNF.
 *
 * This suite uses the process-private table (LNM$PROCESS) so it runs on the
 * host with no /dev/vms -- and that is authentic, not a fake: LNM$PROCESS is
 * genuinely process-private on VMS too (INV-6 is about not faking the SHARED
 * executive tables; a process table is not one). The fan-out code under test
 * (vmsfs_to_linux_path -> lnm_translate_searchlist) is table-agnostic; the
 * executive-resident multi-value store/read it sits on is separately proven
 * cross-process against a real /dev/vms by tests/qemu/test_syssvc_lnm_
 * searchlist.c (vms-420).
 *
 * The discriminating assertions (marked VERACITY) FAIL on the pre-fix
 * collapse-to-index-0 and PASS only once each member is tried in order.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Does resolved end with the given suffix? (device-B path check) */
static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

static void touch(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs("x\n", f); fclose(f); }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_searchlist_open (search-list device logical, try-in-order on open, vms-b12) ===\n");

    /* Two real, distinct directory roots standing in for two disks. */
    char base[] = "/tmp/ovmxb12_XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }

    char dir_a[512], dir_b[512];
    snprintf(dir_a, sizeof(dir_a), "%s/a", base);
    snprintf(dir_b, sizeof(dir_b), "%s/b", base);
    mkdir(dir_a, 0755);
    mkdir(dir_b, 0755);

    /* Register them as two devices. */
    (void)vmsfs_device_add("OVMXB12A", dir_a);
    (void)vmsfs_device_add("OVMXB12B", dir_b);

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* DEFINE FOO OVMXB12A:,OVMXB12B:  -- a two-member search list. */
    const char *members[2] = { "OVMXB12A:", "OVMXB12B:" };
    uint32_t cst = lnm_create_multi(mgr, LNM_PROCESS_TABLE, "OVMXB12_SL",
                                    members, 2, 0, LNM_MODE_USER);
    CHECK(cst == SS$_NORMAL || cst == SS$_SUPERSEDE,
          "DEFINE of a two-member search list in LNM$PROCESS succeeded");

    /* ---- READ AT ALL INDICES: translation returns BOTH members, in order. */
    {
        char vals[LNM_MAX_SEARCHLIST][LNM_MAX_VALUE + 1];
        uint8_t n = 0;
        uint32_t st = lnm_translate_searchlist(mgr, "OVMXB12_SL", vals,
                                               LNM_MAX_SEARCHLIST, &n, NULL);
        CHECK(st == SS$_NORMAL && n == 2,
              "lnm_translate_searchlist reports BOTH equivalence strings (n=2)");
        CHECK(n >= 1 && strcmp(vals[0], "OVMXB12A:") == 0,
              "search-list index 0 = OVMXB12A: (first member)");
        /* VERACITY (pre-fix drops this): index 1 is the second member. */
        CHECK(n >= 2 && strcmp(vals[1], "OVMXB12B:") == 0,
              "VERACITY: search-list index 1 = OVMXB12B: (second member, previously collapsed to index 0)");
    }

    /* ---- TRIED IN ORDER ON OPEN: file lives ONLY in the SECOND member. */
    {
        char target_b[600];
        snprintf(target_b, sizeof(target_b), "%s/report.dat", dir_b);
        touch(target_b);   /* exists only under device B */

        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("OVMXB12_SL:REPORT.DAT",
                                          resolved, sizeof(resolved));
        printf("  INFO: OVMXB12_SL:REPORT.DAT (file in member 2) -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st), "translate succeeded");
        /* VERACITY: pre-fix resolves to member A (no file) and open would FNF;
         * post-fix walks to member B where the file actually is. */
        CHECK(ends_with(resolved, "/b/report.dat"),
              "VERACITY: open through the search list resolves to member B (where the file exists), not member A");
        struct stat sb;
        CHECK(stat(resolved, &sb) == 0,
              "VERACITY: the resolved path names a file that actually exists");
    }

    /* ---- ORDER MATTERS: when the FIRST member has the file, it wins. */
    {
        char target_a[600];
        snprintf(target_a, sizeof(target_a), "%s/first.dat", dir_a);
        char also_b[600];
        snprintf(also_b, sizeof(also_b), "%s/first.dat", dir_b);
        touch(target_a);
        touch(also_b);   /* present in BOTH members */

        char resolved[1024] = "";
        (void)vmsfs_to_linux_path("OVMXB12_SL:FIRST.DAT", resolved, sizeof(resolved));
        printf("  INFO: OVMXB12_SL:FIRST.DAT (file in BOTH) -> %s\n", resolved);
        CHECK(ends_with(resolved, "/a/first.dat"),
              "when both members have the file, the FIRST member wins (search order)");
    }

    /* ---- CREATE SEMANTICS: file in NEITHER member -> primary (first) path. */
    {
        char resolved[1024] = "";
        (void)vmsfs_to_linux_path("OVMXB12_SL:NEW.DAT", resolved, sizeof(resolved));
        printf("  INFO: OVMXB12_SL:NEW.DAT (file in neither) -> %s\n", resolved);
        CHECK(ends_with(resolved, "/a/new.dat"),
              "a file present in no member resolves to the PRIMARY member (create-in-first)");
    }

    /* ---- REGRESSION: a single-valued device logical is unchanged. */
    {
        (void)lnm_create_multi(mgr, LNM_PROCESS_TABLE, "OVMXB12_ONE",
                               (const char *[]){ "OVMXB12A:" }, 1, 0,
                               LNM_MODE_USER);
        char resolved[1024] = "";
        (void)vmsfs_to_linux_path("OVMXB12_ONE:REPORT.DAT", resolved, sizeof(resolved));
        CHECK(ends_with(resolved, "/a/report.dat"),
              "a single-valued device logical still resolves through the classic path");
    }

    /* Cleanup best-effort. */
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "OVMXB12_SL", LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "OVMXB12_ONE", LNM_MODE_USER);

    printf("=== test_searchlist_open: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
