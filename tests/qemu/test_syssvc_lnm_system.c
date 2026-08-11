/*
 * test_syssvc_lnm_system.c - the vmslnm-MANAGER's LNM$SYSTEM path, against a
 * real /dev/vms (vms-48ab).
 *
 * ============================================================
 * WHY THIS SUITE EXISTS, AND WHAT test_syssvc_lnm_crossproc.c DOES NOT COVER.
 *
 * vms-96e2 made LNM$SYSTEM executive-resident for TWO independent client
 * implementations:
 *   (1) src/libvms/syssvc/sys_logical.c -- sys$crelnm/$trnlnm/$dellnm, the
 *       public system-service API. Proven against a real executive by
 *       test_syssvc_lnm_crossproc.c. This implementation NEVER had a
 *       host-tooling fallback.
 *   (2) src/vmslnm/lnm_client.c + lnm_translate.c -- the vmslnm MANAGER API
 *       (lnm_create/lnm_translate/lnm_delete), which DCL's DEFINE/SYSTEM,
 *       SHOW LOGICAL, F$TRNLNM and vmsfs filespec resolution all call
 *       (vmsfs cannot call sys$* directly -- see docs/design-lnm-executive-
 *       surface.md). Until vms-48ab, THIS implementation had a transitional
 *       fallback to a process-local table when /dev/vms returned
 *       SS$_NOSUCHDEV, so host ctest could still exercise DEFINE/SYSTEM --
 *       exactly the silent per-process fake CLAUDE.md Rule 9 / INV-6
 *       forbids. vms-48ab removed it.
 *
 * No existing QEMU suite called the vmslnm-manager API directly against a
 * real executive -- test_syssvc_lnm_crossproc.c only exercises path (1). A
 * regression that broke (2)'s executive routing while leaving (1) intact
 * would have gone undetected. This suite closes that gap and is the
 * regression proof for vms-48ab's fix.
 *
 * WHAT'S PROVEN HERE, MIGRATED FROM HOST TESTS THAT USED TO LEAN ON THE
 * REMOVED FALLBACK:
 *   - tests/vmslnm/test_vmslnm.c's old SYSTEM-table hierarchy scenario (now
 *     re-targeted at LNM$GROUP on the host, since GROUP stays process-local)
 *     is repeated here against the real LNM$SYSTEM: a manager-created SYSTEM
 *     name is visible to a manager translate, a process-table override
 *     shadows it in the default search list, and the SYSTEM value itself is
 *     unchanged underneath the override.
 *   - tests/libvms/test_identity.c's SYS$WELCOME / SYS$ANNOUNCE "defined"
 *     scenarios (undefined-vs-built-in stayed host-side; "defined" could not,
 *     since DEFINE/SYSTEM now honestly fails with no executive) -- including
 *     the '@file' multi-line site-banner form.
 *
 * NO STATUS CONSTANT IS ASSERTED BY VALUE for the success path (VMS odd/even
 * convention only), matching test_syssvc_lnm_crossproc.c's convention.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * anchored by the lnm-manager-system-bypass defect, which makes
 * lnm_client.c's is_system_table() branch in lnm_create() a dead branch (an
 * `if (0)` guard) so a manager-level DEFINE/SYSTEM silently falls through to
 * the process-local table instead of reaching vms.ko -- reintroducing
 * exactly the INV-6 shape vms-48ab removed, scoped to this suite's
 * manager-roundtrip and banner-override assertions only.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/logical.h"
#include "ovmx_banner.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* sys$trnlnm wrapper, used only to CROSS-CHECK that the manager API and the
 * public sys$ API read the SAME executive arena (design "consistent by
 * construction", docs/design-lnm-executive-surface.md). */
static uint32_t sys_trn(const char *table, const char *name, char *out, size_t outsz)
{
    struct dsc$descriptor_s td = mkdsc(table);
    struct dsc$descriptor_s nd = mkdsc(name);
    struct item_list_3 il[2];
    uint16_t rl = 0;
    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)(outsz - 1);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = out;
    il[0].retlen    = &rl;
    out[0] = '\0';
    uint32_t st = sys$trnlnm(NULL, &td, &nd, NULL, il);
    if (rl >= outsz) rl = (uint16_t)(outsz - 1);
    out[rl] = '\0';
    return st;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Run a banner function against a temp stream and capture what it wrote
 * (same technique as tests/libvms/test_identity.c). */
static void capture_banner(void (*fn)(FILE *), char *out, size_t out_len)
{
    out[0] = '\0';
    FILE *tmp = tmpfile();
    if (!tmp) { fprintf(stderr, "tmpfile() failed\n"); exit(1); }
    fn(tmp);
    fflush(tmp);
    rewind(tmp);
    size_t n = fread(out, 1, out_len - 1, tmp);
    out[n] = '\0';
    fclose(tmp);
}

/*
 * (a) No executive: the vmslnm MANAGER API must fail LNM$SYSTEM operations
 * honestly, exactly like the sys$ API already does (test_syssvc_lnm_
 * crossproc.c's own no-executive branch). This is the direct INV-6
 * regression proof for vms-48ab, at the layer that actually had the
 * fallback.
 */
static void run_no_executive(void)
{
    lnm_manager_t *mgr = lnm_get_manager();
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    printf("  FAIL: cannot open /dev/vms\n");

    st = lnm_create(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$NOEXEC", "x", 0, LNM_MODE_EXEC);
    CHECK(st == SS$_NOSUCHDEV,
          "no executive: lnm_create against LNM$SYSTEM (manager API) fails SS$_NOSUCHDEV, never a local fallback");

    st = lnm_translate(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$NOEXEC", val, sizeof(val), &rlen, &attrs);
    CHECK(st == SS$_NOSUCHDEV,
          "no executive: lnm_translate against LNM$SYSTEM (manager API) fails SS$_NOSUCHDEV, never a local fallback");

    st = lnm_delete(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$NOEXEC", LNM_MODE_EXEC);
    CHECK(st == SS$_NOSUCHDEV,
          "no executive: lnm_delete against LNM$SYSTEM (manager API) fails SS$_NOSUCHDEV, never a local fallback");
}

/*
 * (b) With a real executive: the manager-level SYSTEM roundtrip and table
 * hierarchy, mirroring what tests/vmslnm/test_vmslnm.c could prove on the
 * host before vms-48ab moved LNM$SYSTEM off the host fallback.
 */
static void run_manager_system_and_hierarchy(void)
{
    lnm_manager_t *mgr = lnm_get_manager();
    char val[256];
    uint16_t rlen = 0;
    uint32_t attrs = 0;
    uint32_t st;

    /* Clean slate: a prior suite in this same booted guest may have left
     * these names defined (LNM$SYSTEM persists across suites). */
    (void)lnm_delete(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$VOL", LNM_MODE_EXEC);

    st = lnm_create(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$VOL", "/vms/dka0", 0, LNM_MODE_EXEC);
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "manager: lnm_create in LNM$SYSTEM against a real executive reports success");

    st = lnm_translate(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$VOL", val, sizeof(val), &rlen, &attrs);
    CHECK((st & 1) && strcmp(val, "/vms/dka0") == 0,
          "manager: a name lnm_create'd in LNM$SYSTEM is visible to lnm_translate (the manager reaches the same executive arena)");

    /* Cross-check against the public sys$ API: same arena, same name. */
    st = sys_trn("LNM$SYSTEM", "OVMX48AB$VOL", val, sizeof(val));
    CHECK((st & 1) && strcmp(val, "/vms/dka0") == 0,
          "consistency: sys$trnlnm sees the SAME LNM$SYSTEM value the manager API created (one arena, two clients)");

    /* Table hierarchy: a process-table entry of the same name shadows the
     * SYSTEM one in the default search list, but LNM$SYSTEM itself is
     * unchanged underneath it -- exactly the property test_vmslnm.c's
     * pre-vms-48ab host test asserted, now proven where it actually holds. */
    st = lnm_create(mgr, LNM_PROCESS_TABLE, "OVMX48AB$VOL", "/override", 0, LNM_MODE_USER);
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "hierarchy: create OVMX48AB$VOL override in LNM$PROCESS_TABLE succeeds");

    st = lnm_translate(mgr, LNM_FILE_DEV, "OVMX48AB$VOL", val, sizeof(val), &rlen, &attrs);
    CHECK((st & 1) && strcmp(val, "/override") == 0,
          "hierarchy: the default search list finds the process override, not the SYSTEM value");

    st = lnm_translate(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$VOL", val, sizeof(val), &rlen, &attrs);
    CHECK((st & 1) && strcmp(val, "/vms/dka0") == 0,
          "hierarchy: LNM$SYSTEM's own value is unchanged underneath the process override");

    (void)lnm_delete(mgr, LNM_PROCESS_TABLE, "OVMX48AB$VOL", LNM_MODE_USER);

    /* Delete through the manager API; both the manager and sys$ must agree
     * the name is gone. */
    st = lnm_delete(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$VOL", LNM_MODE_EXEC);
    CHECK(st & 1, "manager: lnm_delete in LNM$SYSTEM against a real executive reports success");

    st = lnm_translate(mgr, LNM_SYSTEM_TABLE, "OVMX48AB$VOL", val, sizeof(val), &rlen, &attrs);
    /* negctl: lnm-manager-delete-noop */
    CHECK(st == SS$_NOLOGNAM,
          "manager: OVMX48AB$VOL is gone from LNM$SYSTEM after the manager's own delete");
}

/*
 * (c) With a real executive: SYS$WELCOME / SYS$ANNOUNCE via DEFINE/SYSTEM
 * through the manager API -- migrated from tests/libvms/test_identity.c's
 * "defined" scenarios, which could no longer be proven on the host once
 * vms-48ab removed the fallback ovmx_banner_display's translate rode on.
 */
static void run_banner_override(void)
{
    lnm_manager_t *mgr = lnm_get_manager();
    char got[OVMX_BANNER_MAXLEN];
    uint32_t st;

    /* Clean slate. */
    (void)lnm_delete(mgr, LNM_SYSTEM_TABLE, "SYS$WELCOME", LNM_MODE_EXEC);
    (void)lnm_delete(mgr, LNM_SYSTEM_TABLE, "SYS$ANNOUNCE", LNM_MODE_EXEC);

    /* Undefined -> built-in, same as the host test. */
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    CHECK(strstr(got, OVMX_PRODUCT_NAME) != NULL,
          "banner: undefined SYS$WELCOME falls back to the built-in OpenVMX banner");

    /* Defined via DEFINE/SYSTEM (the manager API DCL actually calls) ->
     * that string, verbatim, against a REAL executive. */
    st = lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$WELCOME", "Welcome to the lab system",
                    0, LNM_MODE_EXEC);
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "banner: DEFINE/SYSTEM SYS$WELCOME succeeds against a real executive");
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    CHECK(strcmp(got, "Welcome to the lab system\n") == 0,
          "banner: defined SYS$WELCOME overrides the built-in banner (real executive)");
    CHECK(strstr(got, OVMX_PRODUCT_NAME) == NULL,
          "banner: an overridden welcome does not also print the built-in");

    /* '@file' form: multi-line site banner. */
    char path[] = "/tmp/ovmx48ab_welcome_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { fprintf(stderr, "mkstemp failed\n"); exit(1); }
    const char *body = "  Line one\n  Line two\n";
    if (write(fd, body, strlen(body)) != (ssize_t)strlen(body)) {
        fprintf(stderr, "write failed\n"); exit(1);
    }
    close(fd);

    char equiv[256];
    snprintf(equiv, sizeof(equiv), "@%s", path);
    st = lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$WELCOME", equiv, 0, LNM_MODE_EXEC);
    CHECK((st & 1) || st == SS$_SUPERSEDE, "banner: DEFINE/SYSTEM SYS$WELCOME '@file' succeeds");
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    CHECK(strcmp(got, body) == 0,
          "banner: '@file' displays the file's contents verbatim (real executive)");
    unlink(path);

    /* Deassign restores the built-in. */
    st = lnm_delete(mgr, LNM_SYSTEM_TABLE, "SYS$WELCOME", LNM_MODE_EXEC);
    CHECK(st & 1, "banner: DEASSIGN/SYSTEM SYS$WELCOME succeeds");
    capture_banner(ovmx_banner_welcome, got, sizeof(got));
    /* negctl-knockon: lnm-manager-delete-noop */
    CHECK(strstr(got, OVMX_PRODUCT_NAME) != NULL,
          "banner: deassigning SYS$WELCOME restores the built-in banner");

    /* SYS$ANNOUNCE: silence by default, displayed once defined. */
    capture_banner(ovmx_banner_announce, got, sizeof(got));
    CHECK(got[0] == '\0',
          "banner: undefined SYS$ANNOUNCE prints nothing (VMS default is silence)");

    st = lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$ANNOUNCE",
                    "Unauthorized access is prohibited.", 0, LNM_MODE_EXEC);
    CHECK((st & 1) || st == SS$_SUPERSEDE, "banner: DEFINE/SYSTEM SYS$ANNOUNCE succeeds");
    capture_banner(ovmx_banner_announce, got, sizeof(got));
    CHECK(strcmp(got, "Unauthorized access is prohibited.\n") == 0,
          "banner: defined SYS$ANNOUNCE is displayed before the Username: prompt (real executive)");

    (void)lnm_delete(mgr, LNM_SYSTEM_TABLE, "SYS$ANNOUNCE", LNM_MODE_EXEC);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_lnm_system (vmslnm-manager LNM$SYSTEM path, vms-48ab) ===\n");

    if (!executive_present()) {
        run_no_executive();
        printf("=== test_syssvc_lnm_system: %d passed, %d failed (SKIPPED: no /dev/vms -- executive-present scenarios not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    run_manager_system_and_hierarchy();
    run_banner_override();

    printf("=== test_syssvc_lnm_system: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
