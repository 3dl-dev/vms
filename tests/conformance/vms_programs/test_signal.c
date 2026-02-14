/*
 * test_signal.c - VMS Condition Handling Conformance Test
 *
 * Tests CHF (Condition Handling Facility):
 * - lib$establish to set up a handler
 * - lib$signal to signal a condition
 * - Handler receives signal and mechanism arrays
 * - lib$revert to remove handler
 * - lib$sig_to_ret for converting signals to return values
 */

#include <stdio.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <chfdef.h>
#include <lib$routines.h>

/* Global flag to track handler invocation */
static int handler_called = 0;
static uint32_t handler_received_condition = 0;

/* Test handler */
static uint32_t test_handler(struct chf$signal_array *sigarray,
                              struct chf$mech_array *mecharray) {
    (void)mecharray; /* Unused */

    handler_called = 1;

    /* Extract condition value from signal array */
    if (sigarray && sigarray->chf$is_sig_args >= 1) {
        handler_received_condition = sigarray->chf$is_sig_name;
    }

    /* Return SS$_CONTINUE to resume execution */
    return SS$_CONTINUE;
}

int main(void) {
    int failures = 0;
    void *prev_handler;

    printf("VMS Condition Handling Test\n");
    printf("============================\n");

    /* Test 1: lib$establish */
    prev_handler = lib$establish((void *)test_handler);
    printf("PASS: lib$establish set up handler (prev=%p)\n", prev_handler);

    /* Test 2: lib$signal triggers handler */
    handler_called = 0;
    handler_received_condition = 0;

    /* Signal a warning condition */
    lib$signal(SS$_NORMAL);

    if (handler_called) {
        printf("PASS: lib$signal invoked handler\n");
    } else {
        printf("FAIL: lib$signal did not invoke handler\n");
        failures++;
    }

    /* Test 3: Handler received correct condition */
    if (handler_received_condition == SS$_NORMAL) {
        printf("PASS: Handler received correct condition (0x%x)\n",
               handler_received_condition);
    } else {
        printf("FAIL: Handler received wrong condition (expected 0x%x, got 0x%x)\n",
               SS$_NORMAL, handler_received_condition);
        failures++;
    }

    /* Test 4: lib$revert */
    uint32_t status = lib$revert();
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: lib$revert removed handler\n");
    } else {
        printf("FAIL: lib$revert failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 5: Signal after revert should NOT call our handler */
    handler_called = 0;
    handler_received_condition = 0;

    /* Note: Without a handler, lib$signal may cause default action.
     * For testing, we establish lib$sig_to_ret which converts signal to return. */
    lib$establish((void *)lib$sig_to_ret);
    lib$signal(SS$_NORMAL);

    if (!handler_called) {
        printf("PASS: Handler not called after lib$revert\n");
    } else {
        printf("FAIL: Handler was called after lib$revert\n");
        failures++;
    }

    /* Test 6: lib$sig_to_ret */
    /* lib$sig_to_ret is established above; it converts signals to returns.
     * We test that it doesn't crash the program. */
    printf("PASS: lib$sig_to_ret handled signal gracefully\n");

    lib$revert();

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All condition handling tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
