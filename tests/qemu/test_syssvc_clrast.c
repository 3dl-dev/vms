/*
 * test_syssvc_clrast.c - $CLRAST lets an AST queued from inside an AST routine run NOW.
 *
 * With an AST routine running, a second $DCLAST'd AST waits for the first to finish
 * (VMS delivers one AST of a mode at a time). $CLRAST cancels that block, so the second
 * AST is delivered before $CLRAST returns. The control (no $CLRAST) shows the second AST
 * running only after the first returns, so the test fails if $CLRAST does nothing.
 * Requires /dev/vms; otherwise honest SKIP (77).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "starlet.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static volatile int ran_two, two_ran_before_one_returned_mark, use_clrast, in_prog_after_clr;

static void ast_two(uint32_t p) { (void)p; ran_two = 1; }

static void ast_one(uint32_t p)
{
    (void)p;
    sys$dclast(ast_two, 0, 3);
    if (use_clrast) {
        sys$clrast();
        in_prog_after_clr = (int)lib$ast_in_prog();
    }
    two_ran_before_one_returned_mark = ran_two;   /* 1 only if $CLRAST delivered it */
}

extern uint32_t lib$ast_in_prog(void);

int main(void)
{
    printf("=== test_syssvc_clrast ===\n");
    if (vms_kif_open() < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    if (!(vms_kif_register(NULL) & 1)) { printf("  FAIL: register\n"); return 1; }

    /* control: no $CLRAST */
    ran_two = two_ran_before_one_returned_mark = 0; use_clrast = 0;
    sys$setast(0);
    sys$dclast(ast_one, 0, 3);
    sys$setast(1);                       /* delivers ast_one, then ast_two */
    CHECK(ran_two == 1, "control: the second AST was delivered after the first returned");
    CHECK(two_ran_before_one_returned_mark == 0,
          "control: it did NOT run inside the first AST (one AST of a mode at a time)");

    /* with $CLRAST */
    ran_two = two_ran_before_one_returned_mark = 0; use_clrast = 1;
    sys$setast(0);
    sys$dclast(ast_one, 0, 3);
    sys$setast(1);
    CHECK(ran_two == 1, "with $CLRAST the second AST ran");
    /* negctl: clrast-no-delivery */
    CHECK(two_ran_before_one_returned_mark == 1,
          "with $CLRAST it ran before the first AST returned (delivered by $CLRAST)");
    CHECK(in_prog_after_clr == 0, "LIB$AST_IN_PROG reports 0 after $CLRAST");
    CHECK(sys$clrast() == SS$_NORMAL, "outside an AST routine $CLRAST just succeeds");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
