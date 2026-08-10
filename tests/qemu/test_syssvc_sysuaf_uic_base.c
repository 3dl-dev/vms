/*
 * test_syssvc_sysuaf_uic_base.c - SYSUAF.DAT's UIC fields are OCTAL (vms-e60),
 * proven against a real /dev/vms (vms-d37).
 *
 * MIGRATED FROM tests/libvms/test_sysuaf_uic_base.c. The host version located
 * SYS$SYSTEM:SYSUAF.DAT by calling lnm_setup_defaults() and resolving the
 * SYS$SYSTEM logical IN THIS PROCESS -- which only worked because
 * lnm_seed_system_locating() carried a process-scope fallback that reseeded
 * the executive-resident LNM$SYSTEM names into LNM$PROCESS_TABLE when there was
 * no executive. That fallback is the host-LNM fake CLAUDE.md Rule 9 / INV-6
 * forbids and vms-fk1 deletes; nothing may depend on it. So this assertion now
 * runs where SYS$SYSTEM is genuinely executive-resident -- the booted QEMU
 * guest -- and honest-skips (77) with no /dev/vms, exactly like the other
 * tests/qemu/test_syssvc_* suites.
 *
 * WHY THIS TEST EXISTS, AND WHY IT ASSERTS THE ACCOUNT IT DOES.
 *
 * OVMX gave two different UICs for one account. LOGINOUT stamped the UIC it
 * got from SYSUAF into the executive, while F$IDENTIFIER answered a different
 * number for the same name, because SYSUAF.DAT's UIC fields were parsed with
 * strtoul(..., 10) and every VMS UIC is written in OCTAL.
 *
 * THE ORACLE, measured on OpenVMS VAX V7.3 (lab nodes vax3 and vax2, twice)
 * and also asserted end-to-end through DCL in tests/qemu/test_syssvc_ident.c:
 *
 *     F$IDENTIFIER("DEFAULT","NAME_TO_NUMBER") -> 8388736  (%X00800080)
 *     F$IDENTIFIER("SYSTEM","NAME_TO_NUMBER")  ->   65540  (%X00010004)
 *
 * SYS$SYSTEM:SYSUAF.DAT ships 'DEFAULT||200|200|...'. Read as octal that is
 * 128/128 and reproduces the oracle exactly; read as decimal it is 13107400,
 * which matches nothing. This suite proves it at the sysuaf_lookup() reader
 * itself, against the ACTUAL SHIPPED SYSUAF.DAT the guest boots with -- not a
 * fixture invented here, so the assertion cannot drift from what boots.
 *
 * DEFAULT IS THE WHOLE POINT. SYSTEM's UIC is [1,4], and 1 and 4 are the same
 * number in both bases -- so a test that only checks SYSTEM passes whichever
 * base the parser uses and proves nothing. That coincidence is precisely what
 * hid this defect for as long as SYSTEM was the only account anyone logged in
 * as. SYSTEM is still checked below, but as a LIVENESS ANCHOR (the parser ran
 * and produced a record at all), and it is labelled as such so that nobody
 * later reads it as the discriminating check and deletes the one that is.
 *
 * NO /dev/vms -> honest SKIP (77), never a fake pass: with no executive
 * SYS$SYSTEM cannot resolve (the fake is gone), so sysuaf_lookup() must FAIL
 * to find a record rather than fabricate one -- the no-fabrication check below
 * asserts exactly that before skipping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sysuaf.h"
#include "ssdef.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vms/logical.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  PASS: %s\n", name);
        pass++;
    } else {
        printf("  FAIL: %s\n", name);
        fail++;
    }
}

static void check_uic(const char *user, uint32_t want_group,
                      uint32_t want_member, const char *note)
{
    sysuaf_record_t rec;
    char label[192];

    if (sysuaf_lookup(user, &rec) != 0) {
        snprintf(label, sizeof(label), "%s: record found (%s)", user, note);
        check(0, label);
        return;
    }

    uint32_t uic = (rec.uic_group << 16) | rec.uic_member;
    uint32_t want = (want_group << 16) | want_member;

    snprintf(label, sizeof(label),
             "%s: UIC = %u (0%o,0%o) -- %s", user, want,
             want_group, want_member, note);
    if (uic != want)
        printf("    got group=%u member=%u (combined %u), wanted %u\n",
               rec.uic_group, rec.uic_member, uic, want);
    check(uic == want, label);
}

/* Probe only -- vms_kif_open()/close() decide skip-vs-run and nothing else. */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    printf("=== test_syssvc_sysuaf_uic_base: SYSUAF.DAT UIC fields are OCTAL "
           "(vms-e60/vms-d37) ===\n");

    /*
     * Bootstrap the VMS namespace the same way every shipped image does
     * (tools/vms_login.c, tools/vms_authorize.c, DCL.EXE, and
     * tests/qemu/test_syssvc_setuai.c): SYSUAF_PATH is a VMS filespec and
     * vmsfs_to_linux_path() cannot resolve it until the system device is in
     * this process's device table AND SYS$SYSTEM is in the logical name table.
     * VMS_SYSUAF_PATH is "SYS$SYSTEM:SYSUAF.DAT", so both halves are needed;
     * the SYS$SYSTEM half is executive-resident, so this resolves only in the
     * booted guest.
     */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    if (!executive_present()) {
        /*
         * No executive: SYS$SYSTEM is undefined (the host-LNM fake that used to
         * reseed it into LNM$PROCESS_TABLE is gone, vms-fk1), so the reader must
         * NOT fabricate a record. Prove that, then honest-skip.
         */
        sysuaf_record_t rec;
        check(sysuaf_lookup("SYSTEM", &rec) != 0,
              "no /dev/vms: sysuaf_lookup does NOT fabricate a record when "
              "SYS$SYSTEM cannot resolve (no host-LNM fake)");
        printf("=== test_syssvc_sysuaf_uic_base: %d passed, %d failed "
               "(SKIPPED: no /dev/vms -- the octal-UIC read was not exercised, "
               "the no-fabrication check WAS) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* --- THE DISCRIMINATING CHECK ---------------------------------------
     * 200|200 read as OCTAL is 128/128 -> 8388736, the oracle's answer.
     * Read as DECIMAL it is 200/200 -> 13107400. Only one base passes. */
    check_uic("DEFAULT", 0200, 0200,
              "the oracle's 8388736; decimal parsing gives 13107400");

    /* GUEST discriminates too, and in the member field rather than the group,
     * so a fix that changed only one of the two fields still fails here. */
    check_uic("GUEST", 0200, 0201, "member field is octal as well as group");

    /* --- LIVENESS ANCHOR, NOT A DISCRIMINATOR ---------------------------
     * [1,4] is the same in both bases. This proves the lookup ran and
     * produced a record; it CANNOT distinguish octal from decimal, and must
     * never be treated as if it could. */
    check_uic("SYSTEM", 01, 04,
              "LIVENESS ONLY: identical in both bases, proves nothing about base");

    /* State the defect's old value explicitly, so a regression names itself. */
    sysuaf_record_t d;
    if (sysuaf_lookup("DEFAULT", &d) == 0) {
        uint32_t uic = (d.uic_group << 16) | d.uic_member;
        check(uic != 13107400u,
              "DEFAULT is NOT 13107400 (the decimal misreading vms-e60 filed)");
    } else {
        check(0, "DEFAULT: record found for the regression check");
    }

    printf("=== test_syssvc_sysuaf_uic_base: %d passed, %d failed ===\n",
           pass, fail);
    return fail > 0 ? 1 : 0;
}
