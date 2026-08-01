/*
 * test_term_mapping.c - Unit tests for SSH TERM → VMS device type mapping
 *
 * Tests the map_term_to_vms_device_type() function from vmssshd.
 * Since the function is static in vmssshd.c, we include a copy here
 * for unit testing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp, strcasecmp */

/* ---- Copy of map_term_to_vms_device_type from vmssshd.c ---- */

static const char *map_term_to_vms_device_type(const char *term)
{
    if (!term || !term[0])
        return "VT100";

    /* vt400/vt420 family */
    if (strncasecmp(term, "vt4", 3) == 0)
        return "VT400";

    /* vt300/vt320 family */
    if (strncasecmp(term, "vt3", 3) == 0)
        return "VT300";

    /* vt200/vt220 family */
    if (strncasecmp(term, "vt2", 3) == 0)
        return "VT200";

    /* vt100 family */
    if (strncasecmp(term, "vt1", 3) == 0)
        return "VT100";

    /* xterm-256color → VT300 (color capable) */
    if (strcasecmp(term, "xterm-256color") == 0)
        return "VT300";

    /* xterm* → VT100 */
    if (strncasecmp(term, "xterm", 5) == 0)
        return "VT100";

    /* Fallback: dumb, unknown, anything else → VT100 */
    return "VT100";
}

/* ---- Test harness ---- */

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *term_input, const char *expected)
{
    const char *result = map_term_to_vms_device_type(term_input);
    const char *display_input = term_input ? term_input : "(NULL)";

    if (strcmp(result, expected) == 0) {
        printf("  PASS: %-20s → %s\n", display_input, result);
        pass_count++;
    } else {
        printf("  FAIL: %-20s → %s (expected %s)\n",
               display_input, result, expected);
        fail_count++;
    }
}

int main(void)
{
    printf("Testing SSH TERM → VMS device type mapping:\n\n");

    /* xterm family */
    check("xterm",           "VT100");
    check("xterm-color",     "VT100");
    check("xterm-256color",  "VT300");
    check("XTERM-256COLOR",  "VT300");

    /* VT100 family */
    check("vt100",           "VT100");
    check("VT100",           "VT100");
    check("vt102",           "VT100");

    /* VT200 family */
    check("vt200",           "VT200");
    check("vt220",           "VT200");
    check("VT220",           "VT200");

    /* VT300 family */
    check("vt300",           "VT300");
    check("vt320",           "VT300");

    /* VT400 family */
    check("vt400",           "VT400");
    check("vt420",           "VT400");
    check("VT420",           "VT400");

    /* Fallback cases */
    check("dumb",            "VT100");
    check("unknown",         "VT100");
    check("linux",           "VT100");
    check("screen",          "VT100");
    check("",                "VT100");
    check(NULL,              "VT100");

    printf("\nResults: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
