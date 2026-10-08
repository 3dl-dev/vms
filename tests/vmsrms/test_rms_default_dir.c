/*
 * test_rms_default_dir.c - RMS completes a file specification from the process
 * default directory (vms-872), including the relative directory forms (vms-0ae):
 * [.X] below the default, [-] one level up, [] the default itself. Pure string
 * merge (rms_merge_default_dir); the executive-held value it is applied with is
 * proven over /dev/vms by the $SETDDIR suites.
 */
#include <stdio.h>
#include <string.h>

int rms_merge_default_dir(const char *ddir, const char *spec, char *out, size_t outsz);

static int failures;

static void t(const char *ddir, const char *spec, const char *want)
{
    char out[512];
    int r = rms_merge_default_dir(ddir, spec, out, sizeof out);
    int ok = want ? (r == 0 && strcmp(out, want) == 0) : r != 0;
    printf("  %s: %s + %s -> %s (got %s)\n", ok ? "OK" : "FAIL", ddir, spec,
           want ? want : "<unchanged>", r == 0 ? out : "<unchanged>");
    if (!ok)
        failures++;
}

int main(void)
{
    printf("=== RMS default directory merge (vms-872, vms-0ae) ===\n");
    t("DKA0:[USER.SRC]", "HELLO.C", "DKA0:[USER.SRC]HELLO.C");
    t("DKA0:[USER.SRC]", "[OTHER]HELLO.C", "DKA0:[OTHER]HELLO.C");
    t("DKA0:[USER.SRC]", "[.SUB]HELLO.C", "DKA0:[USER.SRC.SUB]HELLO.C");
    t("DKA0:[USER.SRC]", "[.A.B]X.H", "DKA0:[USER.SRC.A.B]X.H");
    t("DKA0:[USER.SRC]", "[-]HELLO.C", "DKA0:[USER]HELLO.C");
    t("DKA0:[USER.SRC]", "[-.INC]STDIO.H", "DKA0:[USER.INC]STDIO.H");
    t("DKA0:[USER.SRC]", "[--]TOP.TXT", "DKA0:[000000]TOP.TXT");
    t("DKA0:[USER.SRC]", "[]HELLO.C", "DKA0:[USER.SRC]HELLO.C");
    t("DKA0:[USER]", "[---]X", NULL);
    t("DKA0:[USER.SRC]", "SYS$LOGIN:X.C", NULL);
    t("DKA0:[USER.SRC]", "NODE::DKA0:[A]X", NULL);
    t("DKA0:[000000]", "[.A]X", "DKA0:[A]X");
    printf(failures ? "%d FAILED\n" : "All default-directory merge tests passed.\n", failures);
    return failures != 0;
}
