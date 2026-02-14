/*
 * test_descriptor.c - VMS String Descriptor Conformance Test
 *
 * Tests string descriptor operations:
 * - Static descriptors ($DESCRIPTOR)
 * - Dynamic descriptors
 * - str$concat, str$copy_dx, str$free1_dx
 * - str$match_wild
 */

#include <stdio.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <str$routines.h>

int main(void) {
    uint32_t status;
    int failures = 0;

    printf("VMS String Descriptor Test\n");
    printf("===========================\n");

    /* Test 1: Static descriptor creation */
    $DESCRIPTOR(static_str, "Hello, VMS!");
    if (static_str.dsc$w_length == 11 &&
        static_str.dsc$b_class == DSC$K_CLASS_S &&
        static_str.dsc$b_dtype == DSC$K_DTYPE_T) {
        printf("PASS: Static descriptor created correctly\n");
    } else {
        printf("FAIL: Static descriptor (expected len=11, class=1, dtype=14, got len=%d, class=%d, dtype=%d)\n",
               static_str.dsc$w_length, static_str.dsc$b_class, static_str.dsc$b_dtype);
        failures++;
    }

    /* Test 2: str$copy_dx to dynamic descriptor */
    $DESCRIPTOR_D(dynamic_str);
    status = str$copy_dx((struct dsc$descriptor_s *)&dynamic_str, &static_str);
    if ($VMS_STATUS_SUCCESS(status) &&
        dynamic_str.dsc$w_length == 11 &&
        dynamic_str.dsc$b_class == DSC$K_CLASS_D) {
        printf("PASS: str$copy_dx to dynamic descriptor\n");
    } else {
        printf("FAIL: str$copy_dx (status=0x%x, len=%d, class=%d)\n",
               status, dynamic_str.dsc$w_length, dynamic_str.dsc$b_class);
        failures++;
    }

    /* Test 3: str$concat */
    $DESCRIPTOR(part1, "OpenVMS");
    $DESCRIPTOR(part2, " on Linux");
    $DESCRIPTOR_D(concat_result);

    status = str$concat((struct dsc$descriptor_s *)&concat_result, &part1, &part2, NULL);
    if ($VMS_STATUS_SUCCESS(status) && concat_result.dsc$w_length == 16) {
        printf("PASS: str$concat (len=%d)\n", concat_result.dsc$w_length);
    } else {
        printf("FAIL: str$concat (status=0x%x, expected len=16, got %d)\n",
               status, concat_result.dsc$w_length);
        failures++;
    }

    /* Test 4: str$match_wild - exact match */
    $DESCRIPTOR(candidate1, "TEST.TXT");
    $DESCRIPTOR(pattern1, "TEST.TXT");
    status = str$match_wild(&candidate1, &pattern1);
    if (status == STR$_MATCH) {
        printf("PASS: str$match_wild exact match\n");
    } else {
        printf("FAIL: str$match_wild exact match (expected STR$_MATCH, got 0x%x)\n", status);
        failures++;
    }

    /* Test 5: str$match_wild - wildcard * */
    $DESCRIPTOR(candidate2, "HELLO.TXT");
    $DESCRIPTOR(pattern2, "*.TXT");
    status = str$match_wild(&candidate2, &pattern2);
    if (status == STR$_MATCH) {
        printf("PASS: str$match_wild with * wildcard\n");
    } else {
        printf("FAIL: str$match_wild with * (expected STR$_MATCH, got 0x%x)\n", status);
        failures++;
    }

    /* Test 6: str$match_wild - wildcard % */
    $DESCRIPTOR(candidate3, "ABC");
    $DESCRIPTOR(pattern3, "A%C");
    status = str$match_wild(&candidate3, &pattern3);
    if (status == STR$_MATCH) {
        printf("PASS: str$match_wild with %% wildcard\n");
    } else {
        printf("FAIL: str$match_wild with %% (expected STR$_MATCH, got 0x%x)\n", status);
        failures++;
    }

    /* Test 7: str$match_wild - no match */
    $DESCRIPTOR(candidate4, "NOMATCH.DAT");
    $DESCRIPTOR(pattern4, "*.TXT");
    status = str$match_wild(&candidate4, &pattern4);
    if (status == STR$_NOMATCH) {
        printf("PASS: str$match_wild no match\n");
    } else {
        printf("FAIL: str$match_wild no match (expected STR$_NOMATCH, got 0x%x)\n", status);
        failures++;
    }

    /* Test 8: str$free1_dx */
    status = str$free1_dx((struct dsc$descriptor_s *)&dynamic_str);
    if ($VMS_STATUS_SUCCESS(status) &&
        dynamic_str.dsc$w_length == 0 &&
        dynamic_str.dsc$a_pointer == NULL) {
        printf("PASS: str$free1_dx freed dynamic descriptor\n");
    } else {
        printf("FAIL: str$free1_dx (status=0x%x, len=%d, ptr=%p)\n",
               status, dynamic_str.dsc$w_length, dynamic_str.dsc$a_pointer);
        failures++;
    }

    /* Cleanup */
    str$free1_dx((struct dsc$descriptor_s *)&concat_result);

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All descriptor tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
