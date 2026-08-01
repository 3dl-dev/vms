/*
 * test_event_flags.c - VMS Event Flag Services Conformance Test
 *
 * Tests event flag system services:
 * - sys$setef (set event flag)
 * - sys$clref (clear event flag)
 * - sys$readef (read event flag state)
 * - sys$waitfr (wait for event flag)
 */

#include <stdio.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <starlet.h>
#include <vms/pcb.h>

int main(void) {
    uint32_t status;
    int failures = 0;
    uint32_t ef_state;

    /* OVMX requires process context initialization for event flags */
    vms_pcb_init(0);

    printf("VMS Event Flag Services Test\n");
    printf("=============================\n");

    /* Use event flag 1 for testing (local flag) */
    const uint32_t test_efn = 1;

    /* Test 1: sys$clref - clear event flag */
    status = sys$clref(test_efn);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$clref(1) cleared flag (status=0x%x)\n", status);
    } else {
        printf("FAIL: sys$clref(1) failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 2: sys$readef - read cleared flag */
    status = sys$readef(test_efn, &ef_state);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$readef(1) returned status 0x%x (state=0x%x)\n",
               status, ef_state);

        /* Status should be SS$_WASCLR since we just cleared it */
        if (status == SS$_WASCLR) {
            printf("PASS: Flag state is WASCLR as expected\n");
        } else {
            printf("FAIL: Expected SS$_WASCLR (0x%x), got 0x%x\n",
                   SS$_WASCLR, status);
            failures++;
        }
    } else {
        printf("FAIL: sys$readef(1) failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 3: sys$setef - set event flag */
    status = sys$setef(test_efn);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$setef(1) set flag (status=0x%x)\n", status);

        /* Status should be SS$_WASCLR since it was clear before */
        if (status == SS$_WASCLR) {
            printf("PASS: sys$setef returned WASCLR (flag was clear)\n");
        } else if (status == SS$_WASSET) {
            printf("PASS: sys$setef returned WASSET (flag was already set)\n");
        } else {
            printf("FAIL: sys$setef returned unexpected status 0x%x\n", status);
            failures++;
        }
    } else {
        printf("FAIL: sys$setef(1) failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 4: sys$readef - read set flag */
    status = sys$readef(test_efn, &ef_state);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$readef(1) after set (status=0x%x, state=0x%x)\n",
               status, ef_state);

        /* Status should be SS$_WASSET since we just set it */
        if (status == SS$_WASSET) {
            printf("PASS: Flag state is WASSET as expected\n");
        } else {
            printf("FAIL: Expected SS$_WASSET (0x%x), got 0x%x\n",
                   SS$_WASSET, status);
            failures++;
        }
    } else {
        printf("FAIL: sys$readef(1) after set failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 5: sys$setef on already-set flag */
    status = sys$setef(test_efn);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$setef(1) on already-set flag (status=0x%x)\n", status);

        /* Should return SS$_WASSET since flag was already set */
        if (status == SS$_WASSET) {
            printf("PASS: sys$setef correctly returned WASSET\n");
        } else {
            printf("FAIL: Expected SS$_WASSET, got 0x%x\n", status);
            failures++;
        }
    } else {
        printf("FAIL: sys$setef(1) on set flag failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 6: sys$waitfr - wait for set flag (should return immediately) */
    status = sys$waitfr(test_efn);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$waitfr(1) returned immediately for set flag\n");
    } else {
        printf("FAIL: sys$waitfr(1) failed (status=0x%x)\n", status);
        failures++;
    }

    /* Test 7: Clear and verify */
    status = sys$clref(test_efn);
    if ($VMS_STATUS_SUCCESS(status)) {
        if (status == SS$_WASSET) {
            printf("PASS: sys$clref returned WASSET (flag was set before clear)\n");
        } else {
            printf("FAIL: Expected WASSET from clref, got 0x%x\n", status);
            failures++;
        }
    } else {
        printf("FAIL: sys$clref failed (status=0x%x)\n", status);
        failures++;
    }

    /* Verify it's cleared */
    status = sys$readef(test_efn, &ef_state);
    if (status == SS$_WASCLR) {
        printf("PASS: Flag is now cleared\n");
    } else {
        printf("FAIL: Expected WASCLR, got 0x%x\n", status);
        failures++;
    }

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All event flag tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
