/*
 * test_rightslist.c - F$IDENTIFIER's source is the rights database (vms-2f8)
 *
 * WHAT THIS ASSERTS, AND WHY IT ASSERTS IT AGAINST THE SHIPPED FILES.
 *
 * OVMX shipped SYS$SYSTEM:RIGHTSLIST.DAT from the day the boot path
 * provisioned it, and nothing read it: F$IDENTIFIER answered from two
 * hardcoded names. This test drives the reader that replaced them, against
 * the ACTUAL SHIPPED RIGHTSLIST.DAT and SYSUAF.DAT (argv[1], argv[2]) rather
 * than a fixture invented here, so the assertion always runs against what
 * boots and cannot drift from it.
 *
 * THE ORACLE is docs/oracle/vax73-rights-database.md -- OpenVMS VAX V7.3,
 * lab node VAX1, 2026-08-05, live DCL plus AUTHORIZE SHOW/IDENTIFIER/FULL.
 * Every value below is measured there. None is chosen here.
 *
 * THE MISSES ON 1..5 ARE ASSERTIONS, NOT FILLER. The shipped RIGHTSLIST.DAT
 * used to read
 *
 *     INTERACTIVE:1:RESOURCE  BATCH:2  NETWORK:3  LOCAL:4  REMOTE:5
 *
 * and on real VMS not one of 1..5 is an identifier at all -- each answers
 * the null string. They distinguish "reads the rights database" from "reads
 * the rights database and the rights database is right".
 *
 * WHICH ASSERTION CATCHES WHICH MUTATION WAS MEASURED, NOT ASSUMED, and the
 * first guess at it was wrong. Two mutations were run against this file:
 *
 *   A. the OLD shipped file verbatim  ->  8 FAIL, and NOT the 1..5 misses.
 *      Its rows carry bare decimal values, which parse_value() rejects
 *      outright, so no row resolves and the POSITIVE checks are what fail.
 *      (The first draft of this comment claimed the 1..5 misses caught this
 *      case. They do not. Measured.)
 *   B. the old NUMBERING carried into the new notation -- LOCAL:%X00000004
 *      and so on  ->  17 FAIL, INCLUDING all five 1..5 misses.
 *
 * B is the mutation the misses exist for: someone wires the reader up, keeps
 * the invented numbers, and every "is it reading the file" check passes.
 * Neither mutation is caught by both populations, which is why both
 * populations are here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "rightslist.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vms/logical.h"

static int failures = 0;

static void check(int cond, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void check(int cond, const char *fmt, ...)
{
    char label[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(label, sizeof(label), fmt, ap);
    va_end(ap);

    if (cond) {
        printf("  OK: %s\n", label);
    } else {
        printf("  FAIL: %s\n", label);
        failures++;
    }
}

/* name -> value, and the value -> name round trip the oracle shows. */
static void check_roundtrip(const char *name, uint32_t want, const char *note)
{
    uint32_t got = 0;
    if (rightslist_name_to_value(name, &got) != 0) {
        check(0, "%s: resolved at all (%s)", name, note);
        return;
    }
    if (got != want)
        printf("    got %u (%%X%08X), wanted %u (%%X%08X)\n",
               got, got, want, want);
    check(got == want, "%s -> %u (%%X%08X) -- %s", name, want, want, note);

    char back[RIGHTSLIST_NAME_MAX] = {0};
    if (rightslist_value_to_name(want, back, sizeof(back)) != 0) {
        check(0, "%u -> \"%s\": reverse resolves (oracle round-trips it)",
              want, name);
        return;
    }
    check(strcmp(back, name) == 0,
          "%u -> \"%s\" (reverse; got \"%s\")", want, name, back);
}

static void check_miss_value(uint32_t value, const char *why)
{
    char buf[RIGHTSLIST_NAME_MAX] = {0};
    int rc = rightslist_value_to_name(value, buf, sizeof(buf));
    if (rc == 0)
        printf("    resolved to \"%s\"\n", buf);
    check(rc != 0, "%u is NOT an identifier -- %s", value, why);
}

static void copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) {
        fprintf(stderr, "test_rightslist: cannot read %s\n", from);
        exit(1);
    }
    FILE *out = fopen(to, "wb");
    if (!out) {
        fprintf(stderr, "test_rightslist: cannot write %s\n", to);
        exit(1);
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

int main(int argc, char **argv)
{
    printf("test_rightslist: F$IDENTIFIER reads the rights database (vms-2f8)\n");

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <shipped RIGHTSLIST.DAT> <shipped SYSUAF.DAT>\n",
                argv[0]);
        return 1;
    }

    /* Stage the SHIPPED files under a private root and point DKA0: at it, so
     * this test reads the product's own data rather than whatever the host
     * happens to have mounted at /vms. Same arrangement as
     * test_sysuaf_uic_base.c. */
    char root[] = "/tmp/ovmx_rights_XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "test_rightslist: mkdtemp failed\n");
        return 1;
    }

    char dir[512], rights[640], uaf[640];
    snprintf(dir, sizeof(dir), "%s/SYS0", root);                  mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/SYS0/SYSCOMMON", root);        mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/SYS0/SYSCOMMON/SYSEXE", root); mkdir(dir, 0755);
    snprintf(rights, sizeof(rights),
             "%s/SYS0/SYSCOMMON/SYSEXE/RIGHTSLIST.DAT", root);
    snprintf(uaf, sizeof(uaf),
             "%s/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT", root);
    copy_file(argv[1], rights);
    copy_file(argv[2], uaf);

    /* Both halves of filespec resolution, the same pair AUTHORIZE's main()
     * sets up: the device table for DKA0:, the logical name table for
     * SYS$SYSTEM. */
    vmsfs_device_add(SYSDISK_DEVICE, root);
    lnm_setup_defaults(lnm_get_manager(), root);

    /* --- general identifiers, from RIGHTSLIST.DAT --------------------- */
    printf("\n general identifiers (RIGHTSLIST.DAT)\n");
    check_roundtrip("BATCH",       0x80000001u, "oracle: DCL prints -2147483647");
    check_roundtrip("DIALUP",      0x80000002u, "oracle: absent from OVMX entirely before this");
    check_roundtrip("INTERACTIVE", 0x80000003u, "oracle: DCL prints -2147483645");
    check_roundtrip("LOCAL",       0x80000004u, "oracle: DCL prints -2147483644");
    check_roundtrip("NETWORK",     0x80000005u, "oracle: DCL prints -2147483643");
    check_roundtrip("REMOTE",      0x80000006u, "oracle: DCL prints -2147483642");

    /* --- UIC identifiers, derived from SYSUAF ------------------------- */
    printf("\n UIC identifiers (derived from SYSUAF.DAT)\n");

    /* DEFAULT DISCRIMINATES THE BASE as well as the source: [200,200] read
     * as octal is 8388736, the oracle's answer; read as decimal it is
     * 13107400 and matches nothing (vms-e60). */
    check_roundtrip("DEFAULT", 8388736u,
                    "oracle %X00800080 = [200,200] OCTAL; decimal gives 13107400");

    /* SYSTEM is a LIVENESS ANCHOR here, not a discriminator: [1,4] is the
     * same number in both bases and cannot distinguish anything about how
     * the UIC was read. Labelled so nobody later mistakes it for the check
     * that discriminates and deletes the one that does. */
    check_roundtrip("SYSTEM", 65540u,
                    "LIVENESS ONLY: identical in both bases, proves no base");

    /* An account with no hardcode anywhere, which is the point: before this
     * change GUEST had no identifier at all in either direction. */
    check_roundtrip("GUEST", (0200u << 16) | 0201u,
                    "a shipped account that was never in any hardcoded table");

    /* --- THE DISCRIMINATING CHECKS ------------------------------------ */
    printf("\n the values the OLD shipped RIGHTSLIST.DAT assigned\n");
    check_miss_value(1, "was INTERACTIVE:1; oracle answers the null string");
    check_miss_value(2, "was BATCH:2; oracle answers the null string");
    check_miss_value(3, "was NETWORK:3; oracle answers the null string");
    check_miss_value(4, "was LOCAL:4; oracle answers the null string");
    check_miss_value(5, "was REMOTE:5; oracle answers the null string");

    /* --- misses, pinned ----------------------------------------------- */
    printf("\n misses\n");
    check_miss_value(1000, "oracle: F$IDENTIFIER(1000,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(0, "oracle: F$IDENTIFIER(0,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(77777, "oracle: F$IDENTIFIER(77777,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(196609, "oracle: F$IDENTIFIER(196609,\"NUMBER_TO_NAME\") -> \"\"");
    check_miss_value(0x80010004u,
                     "oracle: F$IDENTIFIER(%X80010004,\"NUMBER_TO_NAME\") -> \"\"");

    uint32_t v = 0xDEADBEEFu;
    check(rightslist_name_to_value("NOSUCHIDENT", &v) != 0,
          "\"NOSUCHIDENT\" does not resolve (caller renders this as 0)");

    /* Case-insensitive, as DCL upcases before asking. */
    uint32_t lower = 0;
    check(rightslist_name_to_value("local", &lower) == 0 &&
          lower == 0x80000004u,
          "lookup is case-insensitive (\"local\" resolves like \"LOCAL\")");

    /* --- name the old defect explicitly, so a regression says so ------- */
    printf("\n regression naming\n");
    uint32_t loc = 0;
    if (rightslist_name_to_value("LOCAL", &loc) == 0) {
        check(loc != 4u,
              "LOCAL is NOT 4 (the value the shipped file invented for it)");
    } else {
        check(0, "LOCAL resolves, for the regression check");
    }

    unlink(rights);
    unlink(uaf);
    printf("\ntest_rightslist: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
