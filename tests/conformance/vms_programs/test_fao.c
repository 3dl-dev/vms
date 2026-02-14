/*
 * test_fao.c - VMS FAO (Formatted ASCII Output) Conformance Test
 *
 * Tests sys$fao formatted output directives:
 * - !AS (string descriptor)
 * - !SL (signed longword)
 * - !UL (unsigned longword)
 * - !XL (hexadecimal longword)
 * - !/ (newline)
 * - !_ (tab)
 * - !! (literal !)
 */

#include <stdio.h>
#include <string.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <starlet.h>

int main(void) {
    uint32_t status;
    int failures = 0;
    uint16_t outlen;
    char buffer[256];

    printf("VMS FAO Test\n");
    printf("============\n");

    /* Test 1: Simple string (!AS) */
    $DESCRIPTOR(fmt1, "Hello, !AS!!");
    $DESCRIPTOR(arg1, "World");
    struct dsc$descriptor_s outbuf1;
    outbuf1.dsc$w_length = sizeof(buffer);
    outbuf1.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf1.dsc$b_class = DSC$K_CLASS_S;
    outbuf1.dsc$a_pointer = buffer;

    status = sys$fao(&fmt1, &outlen, &outbuf1, &arg1);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Hello, World!") == 0) {
        printf("PASS: !AS directive (output: \"%s\")\n", buffer);
    } else {
        printf("FAIL: !AS directive (status=0x%x, expected \"Hello, World!\", got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Test 2: Signed longword (!SL) */
    $DESCRIPTOR(fmt2, "Value: !SL");
    struct dsc$descriptor_s outbuf2;
    outbuf2.dsc$w_length = sizeof(buffer);
    outbuf2.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf2.dsc$b_class = DSC$K_CLASS_S;
    outbuf2.dsc$a_pointer = buffer;

    int32_t val2 = -42;
    status = sys$fao(&fmt2, &outlen, &outbuf2, val2);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Value: -42") == 0) {
        printf("PASS: !SL directive (output: \"%s\")\n", buffer);
    } else {
        printf("FAIL: !SL directive (status=0x%x, expected \"Value: -42\", got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Test 3: Unsigned longword (!UL) */
    $DESCRIPTOR(fmt3, "Count: !UL");
    struct dsc$descriptor_s outbuf3;
    outbuf3.dsc$w_length = sizeof(buffer);
    outbuf3.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf3.dsc$b_class = DSC$K_CLASS_S;
    outbuf3.dsc$a_pointer = buffer;

    uint32_t val3 = 12345;
    status = sys$fao(&fmt3, &outlen, &outbuf3, val3);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Count: 12345") == 0) {
        printf("PASS: !UL directive (output: \"%s\")\n", buffer);
    } else {
        printf("FAIL: !UL directive (status=0x%x, expected \"Count: 12345\", got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Test 4: Hexadecimal longword (!XL) */
    $DESCRIPTOR(fmt4, "Hex: !XL");
    struct dsc$descriptor_s outbuf4;
    outbuf4.dsc$w_length = sizeof(buffer);
    outbuf4.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf4.dsc$b_class = DSC$K_CLASS_S;
    outbuf4.dsc$a_pointer = buffer;

    uint32_t val4 = 0xDEADBEEF;
    status = sys$fao(&fmt4, &outlen, &outbuf4, val4);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Hex: DEADBEEF") == 0) {
        printf("PASS: !XL directive (output: \"%s\")\n", buffer);
    } else {
        printf("FAIL: !XL directive (status=0x%x, expected \"Hex: DEADBEEF\", got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Test 5: Newline (!/) */
    $DESCRIPTOR(fmt5, "Line1!/Line2");
    struct dsc$descriptor_s outbuf5;
    outbuf5.dsc$w_length = sizeof(buffer);
    outbuf5.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf5.dsc$b_class = DSC$K_CLASS_S;
    outbuf5.dsc$a_pointer = buffer;

    status = sys$fao(&fmt5, &outlen, &outbuf5);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Line1\nLine2") == 0) {
        printf("PASS: !/ directive (newline)\n");
    } else {
        printf("FAIL: !/ directive (status=0x%x, expected newline, got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Test 6: Tab (!_) */
    $DESCRIPTOR(fmt6, "Col1!_Col2");
    struct dsc$descriptor_s outbuf6;
    outbuf6.dsc$w_length = sizeof(buffer);
    outbuf6.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf6.dsc$b_class = DSC$K_CLASS_S;
    outbuf6.dsc$a_pointer = buffer;

    status = sys$fao(&fmt6, &outlen, &outbuf6);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Col1\tCol2") == 0) {
        printf("PASS: !_ directive (tab)\n");
    } else {
        printf("FAIL: !_ directive (status=0x%x, expected tab, got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Test 7: Literal ! (!!) */
    $DESCRIPTOR(fmt7, "Important!!");
    struct dsc$descriptor_s outbuf7;
    outbuf7.dsc$w_length = sizeof(buffer);
    outbuf7.dsc$b_dtype = DSC$K_DTYPE_T;
    outbuf7.dsc$b_class = DSC$K_CLASS_S;
    outbuf7.dsc$a_pointer = buffer;

    status = sys$fao(&fmt7, &outlen, &outbuf7);
    buffer[outlen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && strcmp(buffer, "Important!") == 0) {
        printf("PASS: !! directive (literal !)\n");
    } else {
        printf("FAIL: !! directive (status=0x%x, expected \"Important!\", got \"%s\")\n",
               status, buffer);
        failures++;
    }

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All FAO tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
