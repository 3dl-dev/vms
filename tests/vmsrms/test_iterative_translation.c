/*
 * test_iterative_translation.c - VERACITY test for iterative logical-name
 * translation of the device field through $PARSE and the unified,
 * LNM$M_TERMINAL-honoring driver (vms-240, Engine B / vms-ed7, composition
 * depth).
 *
 * ============================================================
 * THE GAP THIS PROVES FIXED. OVMX had TWO iterative-translation mechanisms
 * that did not meet, and NEITHER honored the LNM$M_TERMINAL translation
 * attribute: a dead, filespec-blind iterative translator in src/vmslnm,
 * and a by-hand device recursion in src/vmsfs. $PARSE did NO logical-name
 * resolution of the device field at all -- a device logical was left verbatim
 * in the expanded specification.
 *
 * vms-240 unifies them on ONE filespec-aware, TERMINAL-honoring driver
 * (lnm_translate_filespec) and routes $PARSE through it. VMS semantics
 * (VSI OpenVMS User's Manual, "Logical Name Translation"; VSI OpenVMS System
 * Services Reference, $TRNLNM item LNM$_ATTRIBUTES / LNM$M_TERMINAL):
 *
 *   (a) a NON-terminal chain composes through EVERY hop -- TOP -> MID -> a
 *       physical device is fully substituted in the expanded spec; and
 *   (b) a TERMINAL logical STOPS iterative translation at itself -- the
 *       terminal equivalence is final and is NOT re-translated as a further
 *       logical name.
 *
 * The discriminating assertions (marked VERACITY) FAIL on the pre-fix build:
 *   - (a) fails because $PARSE left the device logical untranslated (the
 *     expanded spec still names TOP, never the resolved physical device);
 *   - (b) fails two ways on the pre-fix code -- either the device was left
 *     untranslated (no resolution), or, once resolution is added but TERMINAL
 *     is IGNORED, the terminal hop wrongly chains through to the physical
 *     device instead of stopping at the terminal equivalence.
 *
 * This suite uses LNM$PROCESS -- genuinely process-private on VMS too, so it
 * is authentic on the host with no /dev/vms (INV-6 concerns the SHARED
 * executive tables, not a process table).
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

/* Run $PARSE on `spec`, returning the expanded string in `esa`. */
static uint32_t parse_expand(const char *spec, char *esa, size_t esa_size)
{
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;

    esa[0] = '\0';
    nam.nam$l_esa = esa;
    nam.nam$b_ess = (uint8_t)(esa_size - 1);
    fab.fab$l_nam = &nam;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);

    uint32_t st = sys$parse(&fab, 0, 0);
    if (nam.nam$b_esl < esa_size)
        esa[nam.nam$b_esl] = '\0';
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_iterative_translation ($PARSE device LNM resolution, TERMINAL-honoring, vms-240) ===\n");

    char base[] = "/tmp/ovmx240_XXXXXX";
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }

    /* Physical device OVMX240: -> base. */
    (void)vmsfs_device_add("OVMX240", base);

    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  FAIL: no LNM manager\n"); return 2; }

    /* ---- Direct driver: filespec-aware stripping preserves the tail. */
    {
        uint32_t c1 = lnm_create(mgr, LNM_PROCESS_TABLE, "ITER_MID",
                                 "OVMX240:", 0, LNM_MODE_USER);
        uint32_t c2 = lnm_create(mgr, LNM_PROCESS_TABLE, "ITER_TOP",
                                 "ITER_MID", 0, LNM_MODE_USER);
        CHECK((c1 == SS$_NORMAL || c1 == SS$_SUPERSEDE) &&
              (c2 == SS$_NORMAL || c2 == SS$_SUPERSEDE),
              "DEFINE non-terminal chain ITER_TOP -> ITER_MID -> OVMX240:");

        char out[256];
        uint16_t olen = 0;
        uint32_t attr = 0;
        uint32_t st = lnm_translate_filespec(mgr, LNM_FILE_DEV,
                                             "ITER_TOP:[MYDIR]FOO.DAT",
                                             out, sizeof(out), &olen, &attr);
        out[olen < sizeof(out) ? olen : sizeof(out) - 1] = '\0';
        printf("  INFO: lnm_translate_filespec ITER_TOP:[MYDIR]FOO.DAT -> %s\n", out);
        CHECK($VMS_STATUS_SUCCESS(st), "driver translate succeeded");
        CHECK(strcmp(out, "OVMX240:[MYDIR]FOO.DAT") == 0,
              "VERACITY: chain composes device AND preserves [MYDIR]FOO.DAT tail");
    }

    char esa[256];

    /* ---- (a) $PARSE substitutes the full non-terminal chain. Before vms-240
     * the expanded spec kept ITER_TOP untranslated (VERACITY). */
    {
        uint32_t st = parse_expand("ITER_TOP:[MYDIR]FOO.DAT", esa, sizeof(esa));
        printf("  INFO: $PARSE ITER_TOP:[MYDIR]FOO.DAT -> \"%s\"\n", esa);
        CHECK(st == RMS$_NORMAL, "$PARSE succeeded");
        CHECK(ci_contains(esa, "OVMX240:"),
              "VERACITY: $PARSE resolves the device chain to OVMX240:");
        CHECK(!ci_contains(esa, "ITER_TOP"),
              "VERACITY: the unresolved logical ITER_TOP is gone from the expanded spec");
        CHECK(ci_contains(esa, "FOO.DAT"),
              "the name/type tail survives resolution");
    }

    /* ---- (b) a TERMINAL logical stops the chain at itself. TERM_TOP is
     * terminal and translates to TERM_MID, which itself translates to the
     * physical device OVMX240:. Honoring LNM$M_TERMINAL, resolution stops at
     * TERM_MID and does NOT chain through to OVMX240:. */
    {
        uint32_t c3 = lnm_create(mgr, LNM_PROCESS_TABLE, "TERM_MID",
                                 "OVMX240:", 0, LNM_MODE_USER);
        uint32_t c4 = lnm_create(mgr, LNM_PROCESS_TABLE, "TERM_TOP",
                                 "TERM_MID", LNM_ATTR_TERMINAL, LNM_MODE_USER);
        CHECK((c3 == SS$_NORMAL || c3 == SS$_SUPERSEDE) &&
              (c4 == SS$_NORMAL || c4 == SS$_SUPERSEDE),
              "DEFINE terminal TERM_TOP -> TERM_MID (terminal), TERM_MID -> OVMX240:");

        uint32_t st = parse_expand("TERM_TOP:[MYDIR]FOO.DAT", esa, sizeof(esa));
        printf("  INFO: $PARSE TERM_TOP:[MYDIR]FOO.DAT -> \"%s\"\n", esa);
        CHECK(st == RMS$_NORMAL, "$PARSE succeeded");
        CHECK(ci_contains(esa, "TERM_MID:"),
              "VERACITY: $PARSE stops the terminal chain at TERM_MID:");
        CHECK(!ci_contains(esa, "OVMX240"),
              "VERACITY: a TERMINAL translation does NOT chain through to the physical device");
    }

    /* ---- End-to-end: the non-terminal chain resolves to the physical
     * directory, proving vmsfs_resolve_device_r follows the chain too. */
    {
        char resolved[1024] = "";
        uint32_t st = vmsfs_to_linux_path("ITER_TOP:[000000]", resolved,
                                          sizeof(resolved));
        printf("  INFO: vmsfs_to_linux_path ITER_TOP:[000000] -> %s\n", resolved);
        CHECK($VMS_STATUS_SUCCESS(st), "device chain resolves to a Linux path");
        CHECK(ci_contains(resolved, base),
              "VERACITY: ITER_TOP -> ITER_MID -> OVMX240: resolves to the physical volume");
    }

    /* Cleanup best-effort. */
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "ITER_TOP", LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "ITER_MID", LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "TERM_TOP", LNM_MODE_USER);
    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "TERM_MID", LNM_MODE_USER);
    (void)vmsfs_device_remove("OVMX240");

    printf("=== test_iterative_translation: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
