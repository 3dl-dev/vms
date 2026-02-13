/* Test: Compile and run a VMS C program on Linux */
#include <stdio.h>
#include <descrip.h>
#include <starlet.h>
#include <ssdef.h>
#include <lib$routines.h>

int main(void) {
    uint32_t status;

    /* Test 1: Basic descriptor and lib$put_output */
    $DESCRIPTOR(msg, "Hello from OpenVMS on Linux!");
    status = lib$put_output(&msg);
    if (!$VMS_STATUS_SUCCESS(status)) {
        printf("FAIL: lib$put_output returned %08X\n", status);
        return 1;
    }

    /* Test 2: Dynamic descriptor and string operations */
    struct dsc$descriptor_d result = {0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL};
    $DESCRIPTOR(s1, "Hello ");
    $DESCRIPTOR(s2, "World!");
    status = str$concat(&result, &s1, &s2);
    if (!$VMS_STATUS_SUCCESS(status)) {
        printf("FAIL: str$concat returned %08X\n", status);
        return 1;
    }
    status = lib$put_output((struct dsc$descriptor_s *)&result);
    str$free1_dx(&result);

    /* Test 3: Time services */
    uint64_t now;
    uint16_t timbuf[7];
    status = sys$gettim(&now);
    if (!$VMS_STATUS_SUCCESS(status)) {
        printf("FAIL: sys$gettim returned %08X\n", status);
        return 1;
    }
    status = sys$numtim(timbuf, &now);
    if (!$VMS_STATUS_SUCCESS(status)) {
        printf("FAIL: sys$numtim returned %08X\n", status);
        return 1;
    }
    printf("VMS Time: %04d-%02d-%02d %02d:%02d:%02d.%02d\n",
           timbuf[0], timbuf[1], timbuf[2],
           timbuf[3], timbuf[4], timbuf[5], timbuf[6]);

    printf("\nAll tests passed!\n");
    return 0;
}
