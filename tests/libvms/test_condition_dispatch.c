/*
 * test_condition_dispatch.c - vms-2e72 rung-1: real VMS condition-handling
 * dispatch.
 *
 * Proves the authentic Condition Handling Facility (CHF) search that
 * replaces the old handler-stack emulation:
 *
 *   - SYS$SETEXV establishes/clears the primary/secondary/last-chance
 *     software exception vectors and returns the previous handler.
 *   - lib$signal searches PRIMARY vector -> frame-handler chain (innermost
 *     to outermost) -> SECONDARY vector -> LAST-CHANCE vector, honouring
 *     SS$_CONTINUE / SS$_RESIGNAL at each stage.
 *   - the mechanism array handed to a frame handler carries the REAL
 *     establisher frame pointer and a real chain depth (not NULL / a
 *     fabricated count).
 *
 * Host-buildable (arch-generic): the dispatch order and mechanism-array
 * construction are pure C, no /dev/vms needed. The hardware-exception
 * arm of the same dispatcher is exercised on the Alpha rig (arith_signal,
 * SS$_HPARITH) and is out of scope for this unit.
 *
 * Reference: OpenVMS Programming Concepts Manual, ch. 9; OpenVMS Calling
 * Standard, "Exception Vectors"; docs/design-chf-condition-handling.md.
 */

#include <stdio.h>
#include <stdint.h>
#include "ssdef.h"
#include "chfdef.h"
#include "starlet.h"
#include "lib$routines.h"

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

/* ---- shared call-order instrumentation -------------------------------- */

static int seq;                 /* monotonic call counter */
static int order_primary;
static int order_secondary;
static int order_lastchance;
static int order_inner;
static int order_outer;

static void reset_orders(void)
{
    seq = 0;
    order_primary = order_secondary = order_lastchance = 0;
    order_inner = order_outer = 0;
}

/* ---- handlers --------------------------------------------------------- */

static uint32_t prim_resignal(struct chf$signal_array *s, struct chf$mech_array *m)
{
    (void)s; (void)m;
    order_primary = ++seq;
    return SS$_RESIGNAL;
}

static uint32_t sec_continue(struct chf$signal_array *s, struct chf$mech_array *m)
{
    (void)s; (void)m;
    order_secondary = ++seq;
    return SS$_CONTINUE;
}

static uint32_t lc_continue(struct chf$signal_array *s, struct chf$mech_array *m)
{
    (void)s; (void)m;
    order_lastchance = ++seq;
    return SS$_CONTINUE;
}

/* captures the mechanism array it was handed */
static void  *captured_frame;
static uint32_t captured_depth;
static uint32_t captured_cond;

static uint32_t frame_continue(struct chf$signal_array *s, struct chf$mech_array *m)
{
    order_inner = ++seq;
    captured_frame = m->chf$ph_mch_frame;
    captured_depth = m->chf$is_mch_depth;
    captured_cond  = s->chf$is_sig_name;
    return SS$_CONTINUE;
}

static uint32_t inner_resignal(struct chf$signal_array *s, struct chf$mech_array *m)
{
    (void)s; (void)m;
    order_inner = ++seq;
    return SS$_RESIGNAL;
}

static uint32_t outer_continue(struct chf$signal_array *s, struct chf$mech_array *m)
{
    (void)s; (void)m;
    order_outer = ++seq;
    return SS$_CONTINUE;
}

/* ---- tests ------------------------------------------------------------ */

static void test_setexv_api(void)
{
    printf("Testing sys$setexv establish / clear / read-back...\n");
    void *old = (void *)0x1;

    uint32_t st = sys$setexv(CHF$K_PRIMARY_VECTOR, (void *)prim_resignal, 0, &old);
    check(st == SS$_NORMAL, "sys$setexv(primary) returns SS$_NORMAL");
    check(old == NULL, "first establish reports no previous handler");

    st = sys$setexv(CHF$K_PRIMARY_VECTOR, (void *)sec_continue, 0, &old);
    check(st == SS$_NORMAL, "re-establish returns SS$_NORMAL");
    check(old == (void *)prim_resignal, "re-establish returns the previous handler");

    st = sys$setexv(CHF$K_PRIMARY_VECTOR, NULL, 0, &old);
    check(st == SS$_NORMAL, "clear returns SS$_NORMAL");
    check(old == (void *)sec_continue, "clear returns the previous handler");

    st = sys$setexv(99, (void *)prim_resignal, 0, &old);
    check(st == SS$_BADPARAM, "out-of-range vector selector returns SS$_BADPARAM");

    /* NULL prvhnd must be tolerated */
    st = sys$setexv(CHF$K_PRIMARY_VECTOR, NULL, 0, NULL);
    check(st == SS$_NORMAL, "sys$setexv tolerates a NULL prvhnd");
}

static void test_primary_before_frame(void)
{
    printf("Testing dispatch order: primary vector before frame chain...\n");
    reset_orders();

    (void)lib$establish((void *)frame_continue);
    sys$setexv(CHF$K_PRIMARY_VECTOR, (void *)prim_resignal, 0, NULL);

    captured_frame = (void *)0x1;
    captured_depth = 0xdeadbeef;
    uint32_t st = lib$signal(SS$_NORMAL, 0);

    check(st == SS$_NORMAL, "lib$signal returns SS$_NORMAL after a handler continues");
    check(order_primary != 0 && order_inner != 0, "both primary and frame handler ran");
    check(order_primary < order_inner,
          "primary exception vector is searched BEFORE the frame chain");
    check(captured_cond == SS$_NORMAL, "frame handler saw the signalled condition");
    check(captured_frame != NULL,
          "mechanism array carries a REAL establisher frame pointer (not NULL)");
    check(captured_depth == 0,
          "mechanism array reports the establisher's real chain depth (0)");

    sys$setexv(CHF$K_PRIMARY_VECTOR, NULL, 0, NULL);
    lib$revert();
}

static void test_chain_inner_to_outer(void)
{
    printf("Testing frame chain search order: innermost to outermost...\n");
    reset_orders();

    (void)lib$establish((void *)outer_continue);   /* depth 0 (outer) */
    (void)lib$establish((void *)inner_resignal);   /* depth 1 (inner) */

    uint32_t st = lib$signal(SS$_NORMAL, 0);
    check(st == SS$_NORMAL, "lib$signal returns SS$_NORMAL");
    check(order_inner != 0 && order_outer != 0, "both frame handlers ran");
    check(order_inner < order_outer,
          "innermost frame handler is searched before the outer one");

    lib$revert();
    lib$revert();
}

static void test_secondary_after_frame(void)
{
    printf("Testing secondary vector: searched AFTER the frame chain...\n");
    reset_orders();

    (void)lib$establish((void *)inner_resignal);   /* resignals */
    sys$setexv(CHF$K_SECONDARY_VECTOR, (void *)sec_continue, 0, NULL);

    uint32_t st = lib$signal(SS$_NORMAL, 0);
    check(st == SS$_NORMAL, "lib$signal returns SS$_NORMAL");
    check(order_inner != 0 && order_secondary != 0,
          "frame handler and secondary vector both ran");
    check(order_inner < order_secondary,
          "secondary exception vector is searched AFTER the frame chain");

    sys$setexv(CHF$K_SECONDARY_VECTOR, NULL, 0, NULL);
    lib$revert();
}

static void test_last_chance(void)
{
    printf("Testing last-chance vector claims an otherwise-unhandled condition...\n");
    reset_orders();

    /* No frame handlers, no primary/secondary: only last-chance established. */
    sys$setexv(CHF$K_LAST_CHANCE_VECTOR, (void *)lc_continue, 0, NULL);

    uint32_t st = lib$signal(SS$_NORMAL, 0);
    check(st == SS$_NORMAL, "lib$signal returns SS$_NORMAL");
    check(order_lastchance != 0, "last-chance vector was invoked");

    sys$setexv(CHF$K_LAST_CHANCE_VECTOR, NULL, 0, NULL);
}

int main(void)
{
    printf("=== vms-2e72 rung-1: CHF condition-dispatch tests ===\n");
    test_setexv_api();
    test_primary_before_frame();
    test_chain_inner_to_outer();
    test_secondary_after_frame();
    test_last_chance();

    if (failures == 0) {
        printf("\nAll condition-dispatch tests passed.\n");
        return 0;
    }
    printf("\n%d assertion(s) FAILED.\n", failures);
    return 1;
}
