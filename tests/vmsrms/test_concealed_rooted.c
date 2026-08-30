/*
 * test_concealed_rooted.c - VERACITY test for concealed + rooted-directory
 * device logicals through $PARSE and filespec composition (vms-d8e, Engine B
 * / vms-ed7, composition depth).
 *
 * ============================================================
 * THE GAP THIS PROVES FIXED. A logical defined
 * /TRANSLATION_ATTRIBUTES=CONCEALED (LNM$M_CONCEALED) is a *concealed device*;
 * when its equivalence names a *rooted directory* -- a '.' immediately before
 * the closing ']', e.g. DISK$USER -> VDA100:[USERS.] -- it is a rooted
 * concealed device (VSI OpenVMS DCL Dictionary, DEFINE
 * /TRANSLATION_ATTRIBUTES; VSI OpenVMS User's Manual, "Rooted Directories").
 * VMS $PARSE must:
 *   (1) read the LNM$M_CONCEALED attribute from the logical (NOT infer it
 *       from the device string) and set NAM$M_CNCL_DEV in the NAM block, plus
 *       NAM$M_ROOT_DIR when the concealed equivalence is rooted
 *       (VSI OpenVMS RMS Reference, NAM block nam$l_fnb); and
 *   (2) compose a subdirectory onto the root STRUCTURALLY per the rooted rule
 *       -- WORK:[SMITH]FOO.DAT (WORK -> VDA100:[USERS.]) resolves to
 *       VDA100:[USERS.SMITH]FOO.DAT (physical .../users/smith/foo.dat), a
 *       single contiguous merge, not a trailing-dot string coincidence.
 *
 * Before vms-d8e, $PARSE never consulted the logical's attributes, so it set
 * NEITHER NAM$M_CNCL_DEV nor NAM$M_ROOT_DIR (both 0), and composition merged
 * the root's trailing dot with the subdirectory by string luck (producing a
 * ".../users//smith/..." double-slash junction) rather than honoring the
 * rooted flag.
 *
 * The discriminating assertions (marked VERACITY) FAIL on the pre-fix build
 * and PASS once the concealed/rooted attributes are honored.
 *
 * This suite uses LNM$PROCESS -- genuinely process-private on VMS too, so it
 * is authentic on the host with no /dev/vms (INV-6 concerns the SHARED
 * executive tables, not a process table). The concealed/rooted logic under
 * test is table-agnostic.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "rms/rms.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"
#include "vmsfs/device.h"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Case-insensitive contiguous-substring test. */
static int ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (strncasecmp(p, needle, nl) == 0)
            return 1;
    return 0;
}

/* Run $PARSE on `spec` and return nam$l_fnb (component flags). */
static uint32_t parse_fnb(const char *spec, char *esa, size_t esa_size)
{
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;

    nam.nam$l_esa = esa;
    nam.nam$b_ess = (uint8_t)(esa_size - 1);
    fab.fab$l_nam = &nam;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);

    uint32_t st = sys$parse(&fab, 0, 0);
    if (st != RMS$_NORMAL)
        return 0;
    return nam.nam$l_fnb;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_concealed_rooted ($PARSE NAM$M_CNCL_DEV/ROOT_DIR + rooted composition, vms-d8e) ===\n");

    /* A real directory tree standing in for the physical device's volume. */
    char base[] = "/tmp/ovmxd8e_XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }

    /* Physical device OVMXD8E: -> base. */
    (void)vmsfs_device_add("OVMXD8E", base);

    /* Create the rooted subtree base/users/smith/foo.dat so the composed
     * physical path names a file that actually exists. */
    char dir_users[600], dir_smith[700], file_foo[800];
    snprintf(dir_users, sizeof(dir_users), "%s/users", base);
    snprintf(dir_smith, sizeof(dir_smith), "%s/users/smith", base);
    snprintf(file_foo,  sizeof(file_foo),  "%s/users/smith/foo.dat", base);
    mkdir(dir_users, 0755);
    mkdir(dir_smith, 0755);
    { FILE *f = fopen(file_foo, "w"); if (f) { fputs("x\n", f); fclose(f); } }

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* WORK  : concealed AND rooted   -> OVMXD8E:[USERS.] */
    uint32_t c1 = lnm_create(mgr, LNM_PROCESS_TABLE, "OVMXD8E_WORK",
                             "OVMXD8E:[USERS.]", LNM_ATTR_CONCEALED,
                             LNM_MODE_USER);
    /* DISK1 : concealed, NOT rooted  -> OVMXD8E: */
    uint32_t c2 = lnm_create(mgr, LNM_PROCESS_TABLE, "OVMXD8E_DISK1",
                             "OVMXD8E:", LNM_ATTR_CONCEALED, LNM_MODE_USER);
    /* PLAIN : rooted-LOOKING string but NOT concealed -> OVMXD8E:[USERS.] */
    uint32_t c3 = lnm_create(mgr, LNM_PROCESS_TABLE, "OVMXD8E_PLAIN",
                             "OVMXD8E:[USERS.]", 0, LNM_MODE_USER);
    CHECK((c1 == SS$_NORMAL || c1 == SS$_SUPERSEDE) &&
          (c2 == SS$_NORMAL || c2 == SS$_SUPERSEDE) &&
          (c3 == SS$_NORMAL || c3 == SS$_SUPERSEDE),
          "DEFINE of concealed-rooted, concealed-nonrooted and plain logicals succeeded");

    char esa[256];

    /* ---- $PARSE sets NAM$M_CNCL_DEV + NAM$M_ROOT_DIR for a rooted concealed
     * device. Both bits are 0 on the pre-fix build (VERACITY). */
    {
        uint32_t fnb = parse_fnb("OVMXD8E_WORK:[SMITH]FOO.DAT", esa, sizeof(esa));
        printf("  INFO: $PARSE OVMXD8E_WORK:[SMITH]FOO.DAT fnb=0x%08x esa=\"%.*s\"\n",
               fnb, (int)sizeof(esa), esa);
        CHECK(fnb & NAM$M_CNCL_DEV,
              "VERACITY: $PARSE sets NAM$M_CNCL_DEV for a concealed device logical");
        CHECK(fnb & NAM$M_ROOT_DIR,
              "VERACITY: $PARSE sets NAM$M_ROOT_DIR for a rooted concealed device logical");
    }

    /* ---- A concealed but NON-rooted device sets CNCL_DEV only, never
     * ROOT_DIR -- proves ROOT_DIR tracks the actual rooted equivalence,
     * not merely "the device is a concealed logical". */
    {
        uint32_t fnb = parse_fnb("OVMXD8E_DISK1:FOO.DAT", esa, sizeof(esa));
        CHECK(fnb & NAM$M_CNCL_DEV,
              "VERACITY: $PARSE sets NAM$M_CNCL_DEV for a concealed (non-rooted) device");
        CHECK(!(fnb & NAM$M_ROOT_DIR),
              "a concealed but NON-rooted device does NOT set NAM$M_ROOT_DIR");
    }

    /* ---- A logical that translates to a rooted-LOOKING string but is NOT
     * concealed sets NEITHER bit -- proves the flags honor LNM$M_CONCEALED,
     * not a "[...] ends in .]" string coincidence. */
    {
        uint32_t fnb = parse_fnb("OVMXD8E_PLAIN:[SMITH]FOO.DAT", esa, sizeof(esa));
        CHECK(!(fnb & NAM$M_CNCL_DEV) && !(fnb & NAM$M_ROOT_DIR),
              "a non-concealed logical (rooted-looking equivalence) sets NEITHER CNCL_DEV nor ROOT_DIR");
    }

    /* ---- COMPOSITION honors the rooted flag structurally: the root
     * [USERS.] and the subdirectory [SMITH] concatenate to [USERS.SMITH]
     * (physical .../users/smith/foo.dat), a single contiguous merge. */
    {
        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("OVMXD8E_WORK:[SMITH]FOO.DAT",
                                          resolved, sizeof(resolved));
        printf("  INFO: OVMXD8E_WORK:[SMITH]FOO.DAT -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st), "translate succeeded");
        CHECK(ci_contains(resolved, "/users/smith/foo.dat"),
              "VERACITY: rooted composition merges [USERS.]+[SMITH] into a single .../users/smith/foo.dat");
        CHECK(strstr(resolved, "//") == NULL,
              "VERACITY: the composed path has no '//' junction (structural merge, not trailing-dot string luck)");
        struct stat sb;
        CHECK(stat(resolved, &sb) == 0,
              "VERACITY: the composed physical path names the file that actually exists");
    }

    /* ---- A deeper subdirectory concatenates onto the root too. */
    {
        char sub2[800];
        snprintf(sub2, sizeof(sub2), "%s/users/smith/proj", base);
        mkdir(sub2, 0755);
        char resolved[1024] = "";
        (void)vmsfs_to_linux_path("OVMXD8E_WORK:[SMITH.PROJ]", resolved, sizeof(resolved));
        printf("  INFO: OVMXD8E_WORK:[SMITH.PROJ] -> %s\n", resolved);
        CHECK(ci_contains(resolved, "/users/smith/proj"),
              "rooted composition concatenates a multi-level subdirectory: [USERS.]+[SMITH.PROJ] -> users/smith/proj");
    }

    /* Cleanup best-effort. */
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "OVMXD8E_WORK",  LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "OVMXD8E_DISK1", LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "OVMXD8E_PLAIN", LNM_MODE_USER);
    (void)vmsfs_device_remove("OVMXD8E");

    printf("=== test_concealed_rooted: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
