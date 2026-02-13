/*
 * test_string.c - Test string primitives
 */

#include "vmssys.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        vms_printf("  OK: %s\n", name);
    } else {
        vms_printf("  FAIL: %s\n", name);
        failures++;
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    vms_printf("=== libvmssys string test ===\n");

    /* strlen */
    check(vms_strlen("hello") == 5, "strlen");
    check(vms_strlen("") == 0, "strlen empty");

    /* strcmp */
    check(vms_strcmp("abc", "abc") == 0, "strcmp equal");
    check(vms_strcmp("abc", "abd") < 0, "strcmp less");
    check(vms_strcmp("abd", "abc") > 0, "strcmp greater");

    /* strncmp */
    check(vms_strncmp("abcdef", "abcxxx", 3) == 0, "strncmp equal prefix");
    check(vms_strncmp("abc", "abd", 3) < 0, "strncmp diff");

    /* strcpy/strncpy */
    {
        char buf[32];
        vms_strcpy(buf, "hello");
        check(vms_strcmp(buf, "hello") == 0, "strcpy");

        vms_strncpy(buf, "world", 3);
        check(vms_strncmp(buf, "wor", 3) == 0, "strncpy");
    }

    /* strchr/strrchr */
    check(vms_strchr("hello", 'l') != NULL, "strchr found");
    check(vms_strchr("hello", 'l') - "hello" == 2, "strchr position");
    check(vms_strrchr("hello", 'l') - "hello" == 3, "strrchr position");
    check(vms_strchr("hello", 'z') == NULL, "strchr not found");

    /* strstr */
    check(vms_strstr("hello world", "world") != NULL, "strstr found");
    check(vms_strstr("hello world", "xyz") == NULL, "strstr not found");

    /* memcpy/memset/memcmp */
    {
        char a[16], b[16];
        vms_memset(a, 'A', 8);
        a[8] = '\0';
        check(vms_strcmp(a, "AAAAAAAA") == 0, "memset");

        vms_memcpy(b, a, 8);
        b[8] = '\0';
        check(vms_strcmp(b, "AAAAAAAA") == 0, "memcpy");

        check(vms_memcmp(a, b, 8) == 0, "memcmp equal");
        b[0] = 'B';
        check(vms_memcmp(a, b, 8) < 0, "memcmp diff");
    }

    /* toupper/tolower */
    check(vms_toupper('a') == 'A', "toupper");
    check(vms_tolower('Z') == 'z', "tolower");

    /* strcasecmp */
    check(vms_strcasecmp("Hello", "hello") == 0, "strcasecmp");
    check(vms_strcasecmp("abc", "ABD") < 0, "strcasecmp less");

    /* strtoul */
    check(vms_strtoul("42", NULL, 10) == 42, "strtoul decimal");
    check(vms_strtoul("0xFF", NULL, 0) == 255, "strtoul hex auto");
    check(vms_strtoul("077", NULL, 0) == 63, "strtoul octal auto");

    /* atoi */
    check(vms_atoi("123") == 123, "atoi positive");
    check(vms_atoi("-42") == -42, "atoi negative");

    /* memmove (overlapping) */
    {
        char buf[16] = "abcdefgh";
        vms_memmove(buf + 2, buf, 6);
        check(vms_strncmp(buf, "ababcdef", 8) == 0, "memmove overlap forward");
    }

    /* isdigit/isalpha */
    check(vms_isdigit('5'), "isdigit");
    check(!vms_isdigit('a'), "isdigit negative");
    check(vms_isalpha('A'), "isalpha");

    if (failures == 0)
        vms_printf("All string tests passed.\n");
    else
        vms_printf("Some string tests FAILED (%d).\n", failures);

    return failures;
}
