/*
 * test_login_logicals.c - per-user identity logicals are real (vms-e48)
 *
 * Proves the fix for the fakery this item removed: SYS$LOGIN / SYS$LOGIN_DEVICE
 * / SYS$SCRATCH used to be Linux env vars (vms_login setenv, DCL getenv), so
 * F$TRNLNM("SYS$LOGIN") never saw the user's home and returned the generic
 * SYS$SYSDEVICE:[USERS] default -- and the SYSUAF LGICMD field was ignored in
 * favour of a hardcoded /LOGIN.COM.
 *
 * FAILS-ON-FACADE. The central assertion is that after login establishment,
 * F$TRNLNM("SYS$LOGIN") resolves to the SYSUAF home and NOT the generic
 * default. On origin/main there is no lnm_define_login_logicals() and the home
 * lived only in an env var F$TRNLNM never consults, so SYS$LOGIN stays the
 * generic default -- exactly what the "not the generic default" assertions here
 * reject. (The test also re-asserts the generic default IS what a bare
 * lnm_setup_defaults() yields, so the flip it later demands is a real change,
 * not a tautology.) The LGICMD half asserts the SYSUAF field is honoured and
 * the documented default (SYS$LOGIN:LOGIN.COM), not "/LOGIN.COM", is the
 * fallback -- both of which origin/main gets wrong.
 *
 * TABLE SCOPE. In production (dcl_main.c) lnm_define_login_logicals() targets
 * LNM$JOB, so the whole login job -- DCL plus every image it activates -- sees
 * the same SYS$LOGIN/SYS$SCRATCH (this is what OpenVMS does, and a process-
 * scope version broke the PARTS 0.2 demo: the forked PARTS.EXE could not see a
 * process-scope SYS$SCRATCH). LNM$JOB is executive-resident and needs
 * /dev/vms, which a host ctest does not have; that path is proven by the PARTS
 * demo e2e (tests/qemu). This host test drives the SAME helper against
 * LNM$PROCESS_TABLE -- identical establishment, supersede and LNM$FILE_DEV
 * search-list resolution, minus the executive routing -- so the value/LGICMD
 * logic is covered here and the JOB residency is covered by the e2e.
 *
 * Doc pins (VSI OpenVMS): DCL Dictionary, SYS$LOGIN / SYS$LOGIN_DEVICE /
 * SYS$SCRATCH; System Manager's Manual, SYSUAF default device/directory and
 * AUTHORIZE /LGICMD default.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "vms/logical.h"
#include "sysuaf.h"
#include "ssdef.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* F$TRNLNM the way DCL's dcl_translate_logical() does: through the
 * LNM$FILE_DEV search list (process -> job -> group -> system). */
static int trnlnm(lnm_manager_t *mgr, const char *name, char *out, size_t sz)
{
    uint16_t rlen = 0;
    uint32_t st = lnm_translate(mgr, LNM_FILE_DEV, name, out, sz, &rlen, NULL);
    return (st == SS$_NORMAL || st == SS$_SUPERSEDE) ? 0 : -1;
}

int main(void)
{
    printf("test_login_logicals: per-user identity logicals are real (vms-e48)\n");

    lnm_manager_t *mgr = lnm_get_manager();
    check(mgr != NULL, "lnm manager available");
    if (!mgr)
        return 1;

    /* Seed the standard defaults, including the generic
     * SYS$LOGIN -> SYS$SYSDEVICE:[USERS] every account shares before login. */
    lnm_setup_defaults(mgr, "/tmp/ovmx-test-root");

    char val[LNM_MAX_VALUE + 1];

    /* --- Baseline: before login establishment SYS$LOGIN is the generic
     *     default. This is the facade the fix flips. --- */
    check(trnlnm(mgr, "SYS$LOGIN", val, sizeof(val)) == 0,
          "SYS$LOGIN resolves before login");
    check(strcmp(val, "SYS$SYSDEVICE:[USERS]") == 0,
          "baseline SYS$LOGIN is the generic [USERS] default");

    /* --- Establish this user's identity logicals from a SYSUAF default
     *     device/directory (as LOGINOUT does at login). --- */
    const char *home = "VDA100:[SMITH]";
    /* Host ctest: exercise the establishment against LNM$PROCESS_TABLE (no
     * executive needed). Production passes LNM_JOB_TABLE -- see the file header
     * and the PARTS demo e2e. */
    uint32_t est = lnm_define_login_logicals(mgr, LNM_PROCESS_TABLE, home);
    /* Success is SS$_NORMAL or SS$_SUPERSEDE (the establishment supersedes the
     * generic defaults lnm_setup_defaults() seeded; SS$_SUPERSEDE is an even
     * success code). */
    check(est == SS$_NORMAL || est == SS$_SUPERSEDE,
          "lnm_define_login_logicals succeeded");

    /* SYS$LOGIN now the REAL home, not the generic default. */
    check(trnlnm(mgr, "SYS$LOGIN", val, sizeof(val)) == 0,
          "SYS$LOGIN resolves after login");
    check(strcmp(val, home) == 0,
          "F$TRNLNM(SYS$LOGIN) == real SYSUAF home (not [USERS])");
    check(strcmp(val, "SYS$SYSDEVICE:[USERS]") != 0,
          "F$TRNLNM(SYS$LOGIN) is NOT the generic default");

    /* SYS$LOGIN_DEVICE == the device field of the default directory. */
    check(trnlnm(mgr, "SYS$LOGIN_DEVICE", val, sizeof(val)) == 0,
          "SYS$LOGIN_DEVICE resolves");
    check(strcmp(val, "VDA100:") == 0,
          "F$TRNLNM(SYS$LOGIN_DEVICE) == device of SYSUAF home");

    /* SYS$SCRATCH is DELIBERATELY left at OVMX's system-wide scratch
     * ([SYSTMP]) -- lnm_define_login_logicals() does not redefine it (see its
     * doc: pointing SYS$SCRATCH at each account's not-necessarily-writable home
     * broke the PARTS demo; OVMX ships a dedicated writable scratch instead).
     * So it must NOT have become the login home. */
    check(trnlnm(mgr, "SYS$SCRATCH", val, sizeof(val)) == 0,
          "SYS$SCRATCH resolves");
    check(strcmp(val, home) != 0,
          "SYS$SCRATCH is NOT overridden to the login home");
    check(strcmp(val, "SYS$SYSDEVICE:[SYSTMP]") == 0,
          "SYS$SCRATCH stays the system-wide [SYSTMP] scratch");

    /* --- LGICMD is a real, sourced-from-SYSUAF field, honoured over the
     *     hardcoded default. (vms-d92: the record is the binary $UAFDEF view;
     *     the LGICMD field is set directly rather than parsed from an ASCII
     *     row, but the decision under test -- sysuaf_login_command_file honours
     *     the field, empty falls back to the documented default -- is
     *     unchanged.) --- */

    /* An account WITH an LGICMD field. */
    sysuaf_record_t rec;
    memset(&rec, 0, sizeof(rec));
    strncpy(rec.username, "SMITH", sizeof(rec.username) - 1);
    strncpy(rec.default_dir, "VDA100:[SMITH]", sizeof(rec.default_dir) - 1);
    strncpy(rec.lgicmd, "VDA100:[SMITH]LOGIN_CUSTOM.COM",
            sizeof(rec.lgicmd) - 1);
    check(strcmp(rec.default_dir, "VDA100:[SMITH]") == 0, "default_dir set");
    check(strcmp(rec.lgicmd, "VDA100:[SMITH]LOGIN_CUSTOM.COM") == 0,
          "LGICMD field set");

    char cmdfile[256];
    sysuaf_login_command_file(&rec, cmdfile, sizeof(cmdfile));
    check(strcmp(cmdfile, "VDA100:[SMITH]LOGIN_CUSTOM.COM") == 0,
          "login command file honours the SYSUAF LGICMD field");

    /* An account with an EMPTY LGICMD falls back to the documented default
     * (SYS$LOGIN:LOGIN.COM), NOT a hardcoded /LOGIN.COM. */
    sysuaf_record_t rec7;
    memset(&rec7, 0, sizeof(rec7));
    strncpy(rec7.username, "JONES", sizeof(rec7.username) - 1);
    strncpy(rec7.default_dir, "VDA100:[JONES]", sizeof(rec7.default_dir) - 1);
    check(rec7.lgicmd[0] == '\0', "record with no LGICMD has empty field");
    sysuaf_login_command_file(&rec7, cmdfile, sizeof(cmdfile));
    check(strcmp(cmdfile, "SYS$LOGIN:LOGIN.COM") == 0,
          "empty LGICMD falls back to documented SYS$LOGIN:LOGIN.COM");
    check(strcmp(cmdfile, "/LOGIN.COM") != 0,
          "fallback is NOT the old hardcoded /LOGIN.COM");

    printf("\n%s: %d failure(s)\n",
           failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
