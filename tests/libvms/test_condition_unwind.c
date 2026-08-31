/*
 * test_condition_unwind.c - vms-8802 rung-2: real machine-frame-transfer
 * SYS$UNWIND.
 *
 * Proves that SYS$UNWIND is no longer a pop-only stub: when a condition
 * handler calls sys$unwind() naming a target frame that armed a resume
 * anchor (VMS$UNWIND_ANCHOR), the CHF dispatcher
 *
 *   (a) DEFERS the transfer until the handler returns (as real VMS does),
 *   (b) calls each INTERVENING established handler once with CHF$V_UNWINDING
 *       set,
 *   (c) TRANSFERS control to the target frame's anchor, ABANDONING the
 *       intervening machine frames - their post-signal code never runs,
 *   (d) HONOURS newpc (readable at the resume site via vms$$unwind_newpc()).
 *
 * This is the executable-assertion port of tests/corpus/tier1-examples/
 * sys_unwind.c: the "abort" case must NOT fall through to the code after the
 * signal (the corpus's "After abort" that never prints), because control was
 * transferred to an earlier frame. The pop-only emulation could not achieve
 * that (it returned normally into the signalling frame).
 *
 * Host-buildable / arch-generic: frame transfer here is setjmp/longjmp over
 * the real machine call stack. Resuming into an ancestor frame that armed no
 * anchor (the genuine Alpha invocation-context walk) is rung-3 (vms-1fa).
 *
 * Reference: OpenVMS System Services Reference, $UNWIND; OpenVMS Programming
 * Concepts Manual ch. 9; docs/design-chf-condition-handling.md rung-2.
 */

#include <stdio.h>
#include <stdint.h>
#include <setjmp.h>
#include "ssdef.h"
#include "chfdef.h"
#include "starlet.h"
#include "lib$routines.h"

/* Internal libvms accessor (lib_signal.c): current frame-handler chain depth. */
extern int vms$$handler_depth(void);

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

/* ================================================================
 * Test 1: single-frame transfer (corpus "abort" case).
 *
 * A guarded routine establishes a handler, arms a resume anchor, then
 * signals. The handler unwinds to the guarded frame. Control must resume at
 * the anchor; the code AFTER the signal must NOT run.
 * ================================================================ */

static volatile int t1_after_signal_ran;
static volatile int t1_resumed;
static volatile void *t1_newpc_at_resume;

/* Arbitrary non-NULL newpc token to prove newpc is carried through. */
static char t1_newpc_token;

static uint32_t t1_handler(struct chf$signal_array *sig,
                           struct chf$mech_array *mech)
{
    (void)sig;
    /* Unwind to the establisher's frame (its own chain depth), passing a
     * newpc token - mirrors the corpus handler's sys$unwind(&depth, newpc). */
    uint32_t st = sys$unwind(&mech->chf$is_mch_depth, &t1_newpc_token);
    check(st == SS$_NORMAL, "t1: sys$unwind returns SS$_NORMAL (deferred)");
    return SS$_CONTINUE;
}

static void t1_guarded(void)
{
    (void)lib$establish((void *)t1_handler);

    if (VMS$UNWIND_ANCHOR()) {
        /* Transferred back here by SYS$UNWIND. */
        t1_resumed = 1;
        t1_newpc_at_resume = vms$$unwind_newpc();
        return;
    }

    lib$signal(SS$_ABORT);

    /* The corpus's "After abort": must never execute, control was
     * transferred to the anchor above. */
    t1_after_signal_ran = 1;
}

static void test_single_frame_transfer(void)
{
    printf("Testing single-frame SYS$UNWIND transfer (corpus abort case)...\n");
    t1_after_signal_ran = 0;
    t1_resumed = 0;
    t1_newpc_at_resume = NULL;

    t1_guarded();

    check(t1_resumed == 1,
          "control resumed at the target frame's VMS$UNWIND_ANCHOR");
    check(t1_after_signal_ran == 0,
          "code AFTER the signal did NOT run (frame transfer, not normal return)");
    check(t1_newpc_at_resume == (void *)&t1_newpc_token,
          "newpc was honoured and readable at the resume site");
}

/* ================================================================
 * Test 2: multi-frame transfer + intervening unwind-mode handler.
 *
 * outer() establishes a handler and arms an anchor, then calls middle(),
 * which establishes a (resignalling) handler and calls inner(), which
 * establishes the handler that signals and unwinds all the way out to
 * outer(). Proves:
 *   - the intervening middle handler is called once with CHF$V_UNWINDING,
 *   - the middle and inner frames are abandoned (their post-signal code
 *     never runs),
 *   - outer resumes at its anchor.
 * ================================================================ */

static volatile int t2_outer_resumed;
static volatile int t2_outer_fellthrough;
static volatile int t2_middle_fellthrough;
static volatile int t2_inner_fellthrough;
static volatile int t2_middle_unwind_calls;
static volatile int t2_middle_normal_calls;
static volatile uint32_t t2_middle_unwind_flags;
static volatile int t2_target_depth;   /* outer's chain depth, captured */

static uint32_t t2_middle_handler(struct chf$signal_array *sig,
                                  struct chf$mech_array *mech)
{
    (void)sig;
    if (mech->chf$is_mch_flags & CHF$M_UNWINDING) {
        t2_middle_unwind_calls++;
        t2_middle_unwind_flags = mech->chf$is_mch_flags;
        return SS$_CONTINUE;  /* return value ignored in unwind mode */
    }
    /* Normal delivery: resignal so the search continues... but note the
     * innermost (inner) handler claims first, so this should never run. */
    t2_middle_normal_calls++;
    return SS$_RESIGNAL;
}

static uint32_t t2_inner_handler(struct chf$signal_array *sig,
                                 struct chf$mech_array *mech)
{
    (void)sig; (void)mech;
    /* Unwind out to outer's frame (target depth captured when outer armed). */
    uint32_t depth = (uint32_t)t2_target_depth;
    sys$unwind(&depth, NULL);
    return SS$_CONTINUE;
}

static void t2_inner(void)
{
    (void)lib$establish((void *)t2_inner_handler);
    lib$signal(SS$_ABORT);
    t2_inner_fellthrough = 1;   /* must NOT run */
}

static void t2_middle(void)
{
    (void)lib$establish((void *)t2_middle_handler);
    t2_inner();
    t2_middle_fellthrough = 1;  /* must NOT run */
}

static void t2_outer(void)
{
    /* Establish outer's own handler (a resignaller is fine; it is the target
     * frame, so it is not called during the unwind). */
    (void)lib$establish((void *)t2_middle_handler);
    /* Capture outer's chain depth = index of the handler just established. */
    t2_target_depth = vms$$handler_depth() - 1;

    if (VMS$UNWIND_ANCHOR()) {
        t2_outer_resumed = 1;
        return;
    }

    t2_middle();
    t2_outer_fellthrough = 1;    /* must NOT run */
}

static void test_multi_frame_transfer(void)
{
    printf("Testing multi-frame SYS$UNWIND transfer + unwind-mode handler...\n");
    t2_outer_resumed = 0;
    t2_outer_fellthrough = 0;
    t2_middle_fellthrough = 0;
    t2_inner_fellthrough = 0;
    t2_middle_unwind_calls = 0;
    t2_middle_normal_calls = 0;
    t2_middle_unwind_flags = 0;

    t2_outer();

    check(t2_outer_resumed == 1, "outer frame resumed at its anchor");
    check(t2_outer_fellthrough == 0, "outer's post-call code did NOT run");
    check(t2_middle_fellthrough == 0, "middle frame abandoned (post-call skipped)");
    check(t2_inner_fellthrough == 0, "inner frame abandoned (post-signal skipped)");
    check(t2_middle_unwind_calls == 1,
          "intervening middle handler called exactly once in unwind mode");
    check((t2_middle_unwind_flags & CHF$M_UNWINDING) != 0,
          "intervening handler saw CHF$V_UNWINDING set");
}

/* ================================================================
 * Test 3: pop-only compatibility is preserved (no anchor / NULL depadr).
 *
 * A handler that calls sys$unwind(NULL, NULL) still pops one level and
 * returns normally (the test_lib_fb3 contract), with no frame transfer.
 * ================================================================ */

static volatile int t3_handler_ran;
static volatile int t3_after_signal_ran;

static uint32_t t3_handler(struct chf$signal_array *sig,
                           struct chf$mech_array *mech)
{
    (void)sig; (void)mech;
    t3_handler_ran = 1;
    /* NULL depadr: pop-only, must NOT transfer even though we are mid-dispatch
     * and no anchor is armed here anyway. */
    (void)sys$unwind(NULL, NULL);
    return SS$_CONTINUE;
}

static void test_pop_only_compat(void)
{
    printf("Testing pop-only compatibility (NULL depadr, no anchor)...\n");
    t3_handler_ran = 0;
    t3_after_signal_ran = 0;

    (void)lib$establish((void *)t3_handler);
    lib$signal(SS$_NORMAL, 0);
    /* No anchor armed and depadr NULL -> normal return, so this DOES run. */
    t3_after_signal_ran = 1;

    check(t3_handler_ran == 1, "handler ran");
    check(t3_after_signal_ran == 1,
          "pop-only unwind returns normally (no frame transfer)");

    /* The pop was durable: a second signal finds no handler. */
    t3_handler_ran = 0;
    lib$signal(SS$_NORMAL, 0);
    check(t3_handler_ran == 0, "sys$unwind(NULL) pop is durable");
}

int main(void)
{
    printf("=== vms-8802 rung-2: frame-transfer SYS$UNWIND tests ===\n");
    test_single_frame_transfer();
    test_multi_frame_transfer();
    test_pop_only_compat();

    if (failures == 0) {
        printf("\nAll frame-transfer unwind tests passed.\n");
        return 0;
    }
    printf("\n%d assertion(s) FAILED.\n", failures);
    return 1;
}
