/*
 * test_term_mapping.c - Unit tests for SSH TERM -> OVMX device-type mapping
 *
 * Tests the REAL product function vmsssh_map_term_to_device_type()
 * (src/vmsssh/term_map.c), linked directly into this test binary — not a
 * copy. A drift in the product function now fails this test.
 *
 * RULE 8 / RULE 10 (do not remove): the expected strings below
 * ("VT100"/"VT200"/"VT300"/"VT400") and the TERM->family rules are an
 * OVMX design choice, NOT a reproduction of documented OpenVMS behavior.
 * OpenVMS has no mechanism that infers a device type from a Unix TERM
 * string (device type is set explicitly via SET TERMINAL/DEVICE_TYPE),
 * and its real $TTDEF-derived SHOW TERMINAL output looks like
 * "VT400_Series", not "VT400". This test asserts internal consistency of
 * the OVMX heuristic only — it does NOT certify VMS correctness.
 */

#include <stdio.h>
#include <string.h>

#include "term_map.h"

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *term_input, const char *expected)
{
    const char *result = vmsssh_map_term_to_device_type(term_input);
    const char *display_input = term_input ? term_input : "(NULL)";

    if (strcmp(result, expected) == 0) {
        printf("  PASS: %-20s -> %s\n", display_input, result);
        pass_count++;
    } else {
        printf("  FAIL: %-20s -> %s (expected %s)\n",
               display_input, result, expected);
        fail_count++;
    }
}

int main(void)
{
    printf("Testing SSH TERM -> OVMX device-type mapping "
           "(OVMX design choice, not VMS-authentic — see term_map.h):\n\n");

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
