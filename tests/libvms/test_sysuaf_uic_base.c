/*
 * test_sysuaf_uic_base.c - SYSUAF.DAT's UIC fields are OCTAL (vms-e60)
 *
 * WHY THIS TEST EXISTS, AND WHY IT ASSERTS THE ACCOUNT IT DOES.
 *
 * OVMX gave two different UICs for one account. LOGINOUT stamped the UIC it
 * got from SYSUAF into the executive, while F$IDENTIFIER answered a different
 * number for the same name, because SYSUAF.DAT's UIC fields were parsed with
 * strtoul(..., 10) and every VMS UIC is written in OCTAL.
 *
 * THE ORACLE, measured on OpenVMS VAX V7.3 (lab nodes vax3 and vax2, twice)
 * and already asserted against the real runtime in
 * tests/qemu/test_syssvc_ident.c:
 *
 *     F$IDENTIFIER("DEFAULT","NAME_TO_NUMBER") -> 8388736  (%X00800080)
 *     F$IDENTIFIER("SYSTEM","NAME_TO_NUMBER")  ->   65540  (%X00010004)
 *
 * SYSUAF.DAT ships 'DEFAULT||200|200|...'. Read as octal that is 128/128 and
 * reproduces the oracle exactly; read as decimal it is 13107400, which matches
 * nothing.
 *
 * DEFAULT IS THE WHOLE POINT. SYSTEM's UIC is [1,4], and 1 and 4 are the same
 * number in both bases -- so a test that only checks SYSTEM passes whichever
 * base the parser uses and proves nothing. That coincidence is precisely what
 * hid this defect for as long as SYSTEM was the only account anyone logged in
 * as. SYSTEM is still checked below, but as a LIVENESS ANCHOR (the parser ran
 * and produced a record at all), and it is labelled as such so that nobody
 * later reads it as the discriminating check and deletes the one that is.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "sysuaf.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vms/logical.h"

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

int main(void)
{
    printf("test_sysuaf_uic_base: SYSUAF.DAT UIC fields are OCTAL (vms-e60)\n");

    /* Stage a SYSUAF.DAT under a private root and point DKA0: at it, so this
     * test reads its own fixture rather than whatever the host has at /vms. */
    char root[] = "/tmp/ovmx_uicbase_XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "test_sysuaf_uic_base: mkdtemp failed\n");
        return 1;
    }

    char dir[512], path[640];
    snprintf(dir, sizeof(dir), "%s/SYS0", root);              mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/SYS0/SYSCOMMON", root);    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/SYS0/SYSCOMMON/SYSEXE", root); mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT", root);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr, "test_sysuaf_uic_base: cannot write %s\n", path);
        return 1;
    }
    /* The shipped rows, verbatim -- this is the product's own data, not a
     * fixture invented to make the test pass. */
    fputs("# staged fixture\n", fp);
    fputs("SYSTEM|x|1|4|SYS$SYSDEVICE:[SYSMGR]||ALL\n", fp);
    fputs("DEFAULT||200|200|SYS$SYSDEVICE:[USERS.DEFAULT]||TMPMBX,NETMBX\n", fp);
    fputs("GUEST|y|200|201|SYS$SYSDEVICE:[USERS.GUEST]||TMPMBX\n", fp);
    fclose(fp);

    /* SYSUAF_PATH is "SYS$SYSTEM:SYSUAF.DAT" -- a LOGICAL NAME, so the
     * device table alone does not resolve it. Both halves are pointed at the
     * fixture root, the same pair AUTHORIZE's main() sets up. */
    vmsfs_device_add(SYSDISK_DEVICE, root);
    lnm_setup_defaults(lnm_get_manager(), root);

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
              "DEFAULT is NOT 13107400 (the decimal misreading this item filed)");
    } else {
        check(0, "DEFAULT: record found for the regression check");
    }

    unlink(path);
    printf("test_sysuaf_uic_base: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
