/*
 * test_snprintf.c - Test minimal snprintf implementation
 */

#include "vmssys.h"

static int failures = 0;
static char buf[256];

static void check_str(const char *expected, const char *name)
{
    if (vms_strcmp(buf, expected) == 0) {
        vms_printf("  OK: %s\n", name);
    } else {
        vms_printf("  FAIL: %s (expected \"%s\", got \"%s\")\n", name, expected, buf);
        failures++;
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    vms_printf("=== libvmssys snprintf test ===\n");

    /* Basic integer formats */
    vms_snprintf(buf, sizeof(buf), "%d", 42);
    check_str("42", "%d positive");

    vms_snprintf(buf, sizeof(buf), "%d", -7);
    check_str("-7", "%d negative");

    vms_snprintf(buf, sizeof(buf), "%u", 4294967295U);
    check_str("4294967295", "%u max");

    vms_snprintf(buf, sizeof(buf), "%x", 255);
    check_str("ff", "%x");

    vms_snprintf(buf, sizeof(buf), "%X", 255);
    check_str("FF", "%X");

    vms_snprintf(buf, sizeof(buf), "%o", 255);
    check_str("377", "%o");

    /* Long variants */
    vms_snprintf(buf, sizeof(buf), "%ld", -100L);
    check_str("-100", "%ld");

    vms_snprintf(buf, sizeof(buf), "%lu", 999UL);
    check_str("999", "%lu");

    vms_snprintf(buf, sizeof(buf), "%lx", 0xDEADUL);
    check_str("dead", "%lx");

    /* String and char */
    vms_snprintf(buf, sizeof(buf), "%s", "hello");
    check_str("hello", "%s");

    vms_snprintf(buf, sizeof(buf), "%c", 'X');
    check_str("X", "%c");

    vms_snprintf(buf, sizeof(buf), "%%");
    check_str("%", "%%");

    /* Width and padding */
    vms_snprintf(buf, sizeof(buf), "%5d", 42);
    check_str("   42", "width 5");

    vms_snprintf(buf, sizeof(buf), "%-5d!", 42);
    check_str("42   !", "left-justify");

    vms_snprintf(buf, sizeof(buf), "%05d", 42);
    check_str("00042", "zero-pad");

    vms_snprintf(buf, sizeof(buf), "%+d", 42);
    check_str("+42", "plus flag");

    /* Precision with strings */
    vms_snprintf(buf, sizeof(buf), "%.3s", "hello");
    check_str("hel", "precision string");

    /* Width from argument */
    vms_snprintf(buf, sizeof(buf), "%*d", 6, 99);
    check_str("    99", "star width");

    /* Precision from argument */
    vms_snprintf(buf, sizeof(buf), "%.*s", 4, "hello");
    check_str("hell", "star precision");

    /* Pointer */
    vms_snprintf(buf, sizeof(buf), "%p", (void *)0x1234);
    check_str("0x1234", "%p");

    /* Null string */
    vms_snprintf(buf, sizeof(buf), "%s", (char *)NULL);
    check_str("(null)", "%s NULL");

    /* Combined */
    vms_snprintf(buf, sizeof(buf), "[%d %s %x]", 10, "test", 255);
    check_str("[10 test ff]", "combined");

    /* Buffer size limit */
    {
        char small[8];
        int ret = vms_snprintf(small, sizeof(small), "hello world");
        /* Should truncate to "hello w\0" */
        if (vms_strcmp(small, "hello w") == 0 && ret == 11)
            vms_printf("  OK: truncation\n");
        else {
            vms_printf("  FAIL: truncation (got \"%s\", ret=%d)\n", small, ret);
            failures++;
        }
    }

    if (failures == 0)
        vms_printf("All snprintf tests passed.\n");
    else
        vms_printf("Some snprintf tests FAILED (%d).\n", failures);

    return failures;
}
