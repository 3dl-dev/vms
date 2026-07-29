/*
 * test_priv_display.c - VMS privilege name/description rendering
 *
 * Guards the defect that DCL's SHOW PROCESS/PRIVILEGES carried: a display
 * table with its OWN bit numbering, unrelated to the prvdef.h bits every
 * other part of the system (and the executive) uses. A process holding
 * TMPMBX was reported as holding DETACH; SYSPRV, BYPASS and READALL could
 * not be reported at all. And when the mask was empty the command
 * substituted a hard-coded "TMPMBX NETMBX" -- reporting two privileges that
 * nothing had granted.
 *
 * Both directions are asserted, deliberately:
 *   POSITIVE - a non-empty mask renders the RIGHT names and the pinned text.
 *              Without this, the negative assertions below would pass just
 *              as well against a renderer that had been gutted into printing
 *              nothing at all.
 *   NEGATIVE - an empty mask invents nothing.
 *
 * The expected strings are NOT authored here. They are the SHOW
 * PROCESS/PRIVILEGES output captured verbatim from the reference lab
 * (~/vax/cluster, OpenVMS VAX V7.3, node VAX1).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vms/priv_display.h"
#include "vms/privs.h"

static int passed = 0, failed = 0;

static void check(int cond, const char *msg)
{
    if (cond) { printf("  PASS: %s\n", msg); passed++; }
    else      { printf("  FAIL: %s\n", msg); failed++; }
}

/* Render into a memory buffer so the exact bytes can be asserted. */
static char *render_detail(uint64_t mask, int *count)
{
    static char buf[8192];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return NULL;
    int n = vms_priv_render_detail(mask, f);
    fclose(f);
    if (count) *count = n;
    return buf;
}

static char *render_summary(uint64_t mask, int *count)
{
    static char buf[4096];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return NULL;
    int n = vms_priv_render_summary(mask, f);
    fclose(f);
    if (count) *count = n;
    return buf;
}

/* --------------------------------------------------------------- */
/* POSITIVE: real privileges render with the real VMS text          */
/* --------------------------------------------------------------- */
static void test_renders_held_privileges(void)
{
    int n = 0;
    char *out;

    printf("\n--- detail block renders held privileges (oracle text) ---\n");

    out = render_detail(PRV$M_TMPMBX, &n);
    check(n == 1, "TMPMBX alone renders exactly one privilege");
    /* Verbatim from the lab: name in a 20-column field, then the text. */
    check(strstr(out, " TMPMBX               may create temporary mailbox\n")
              != NULL,
          "TMPMBX renders the pinned name, column layout and description");

    out = render_detail(PRV$M_SETPRV, &n);
    check(strstr(out, " SETPRV               may set any privilege bit\n")
              != NULL,
          "SETPRV renders the pinned description");

    out = render_detail(PRV$M_CMKRNL, &n);
    check(strstr(out, " CMKRNL               may change mode to kernel\n")
              != NULL,
          "CMKRNL renders the pinned description");

    /*
     * SYSPRV is prvdef bit 28. The old table topped out at bit 24, so this
     * privilege was invisible no matter what the process held.
     */
    out = render_detail(PRV$M_SYSPRV, &n);
    check(n == 1 && strstr(out,
              " SYSPRV               may access objects via system protection\n")
              != NULL,
          "SYSPRV (bit 28) renders -- it was undisplayable before");
    check(strstr(out, "TMPMBX") == NULL && strstr(out, "DETACH") == NULL,
          "SYSPRV does NOT drag in an unrelated privilege name");

    /* BYPASS is bit 29, READALL bit 35 -- also beyond the old table. */
    out = render_detail(PRV$M_BYPASS | PRV$M_READALL, &n);
    check(n == 2, "BYPASS (bit 29) and READALL (bit 35) both render");
    check(strstr(out, " BYPASS               may bypass all object access controls\n")
              != NULL &&
          strstr(out, " READALL              may read anything as the owner\n")
              != NULL,
          "both render their pinned descriptions");

    out = render_detail(PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV, &n);
    check(n == 3, "a three-privilege mask renders exactly three lines");

    out = render_summary(PRV$M_TMPMBX | PRV$M_NETMBX, &n);
    check(n == 2 && strstr(out, "TMPMBX") && strstr(out, "NETMBX"),
          "summary line names both held privileges");
    check(strstr(out, "(none)") == NULL,
          "summary does not say (none) when privileges are held");
}

/* --------------------------------------------------------------- */
/* NEGATIVE: nothing is invented                                    */
/* --------------------------------------------------------------- */
static void test_invents_nothing(void)
{
    int n = 0;
    char *out;

    printf("\n--- empty mask invents no privileges ---\n");

    out = render_detail(0, &n);
    check(n == 0, "empty mask renders zero privileges");
    check(strstr(out, " (no privileges enabled)\n") != NULL,
          "empty mask says so explicitly");
    check(strstr(out, "TMPMBX") == NULL && strstr(out, "NETMBX") == NULL,
          "empty mask does NOT substitute the old hard-coded TMPMBX/NETMBX");

    out = render_summary(0, &n);
    check(n == 0 && strstr(out, "(none)") != NULL,
          "summary line reports (none) for an empty mask");
    check(strstr(out, "TMPMBX") == NULL,
          "summary invents nothing for an empty mask");
}

/* --------------------------------------------------------------- */
/* The actual bug: parse and display must agree on the bits         */
/* --------------------------------------------------------------- */
static void test_parse_display_agree(void)
{
    printf("\n--- parse_privilege_string and the display agree on bits ---\n");

    /*
     * This is the round trip that used to fail. parse_privilege_string uses
     * prvdef.h bits; the display used its own. Feeding a parsed name into
     * the renderer therefore produced a DIFFERENT privilege name -- e.g.
     * "TMPMBX" in, "DETACH" out.
     */
    static const char *names[] = {
        "TMPMBX", "NETMBX", "SYSPRV", "SETPRV", "CMKRNL", "CMEXEC",
        "BYPASS", "OPER", "SYSNAM", "GRPNAM", "LOG_IO", "PHY_IO",
        "WORLD", "GROUP", "ALTPRI", "DETACH", NULL
    };

    for (int i = 0; names[i]; i++) {
        uint64_t mask = parse_privilege_string(names[i]);
        int n = 0;
        char *out = render_detail(mask, &n);
        char needle[64];

        snprintf(needle, sizeof(needle), " %s", names[i]);

        char msg[128];
        snprintf(msg, sizeof(msg),
                 "parse(\"%s\") renders back as %s", names[i], names[i]);
        check(mask != 0 && n == 1 && strstr(out, needle) != NULL, msg);
    }
}

int main(void)
{
    printf("=== test_priv_display ===\n");

    test_renders_held_privileges();
    test_invents_nothing();
    test_parse_display_agree();

    printf("\n=== test_priv_display: %d passed, %d failed ===\n",
           passed, failed);
    return failed > 0 ? 1 : 0;
}
