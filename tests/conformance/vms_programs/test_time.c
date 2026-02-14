/*
 * test_time.c - VMS Time Services Conformance Test
 *
 * Tests VMS time services:
 * - sys$gettim (get current time)
 * - sys$numtim (convert to numeric components)
 * - lib$date_time (get formatted date/time string)
 * - lib$day (get day number)
 * - lib$day_of_week (get day of week)
 */

#include <stdio.h>
#include <string.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <starlet.h>
#include <lib$routines.h>

int main(void) {
    uint32_t status;
    int failures = 0;

    printf("VMS Time Services Test\n");
    printf("=======================\n");

    /* Test 1: sys$gettim - get current time */
    uint64_t vms_time;
    status = sys$gettim(&vms_time);
    if ($VMS_STATUS_SUCCESS(status) && vms_time > 0) {
        printf("PASS: sys$gettim returned time (0x%llx)\n",
               (unsigned long long)vms_time);
    } else {
        printf("FAIL: sys$gettim (status=0x%x, time=0x%llx)\n",
               status, (unsigned long long)vms_time);
        failures++;
    }

    /* Test 2: sys$numtim - convert to numeric components */
    uint16_t timbuf[7]; /* year, month, day, hour, minute, second, hundredths */
    status = sys$numtim(timbuf, &vms_time);
    if ($VMS_STATUS_SUCCESS(status)) {
        printf("PASS: sys$numtim (year=%d, month=%d, day=%d, %02d:%02d:%02d.%02d)\n",
               timbuf[0], timbuf[1], timbuf[2], timbuf[3], timbuf[4], timbuf[5], timbuf[6]);

        /* Sanity check values */
        if (timbuf[0] >= 1858 && timbuf[0] <= 9999 &&
            timbuf[1] >= 1 && timbuf[1] <= 12 &&
            timbuf[2] >= 1 && timbuf[2] <= 31 &&
            timbuf[3] <= 23 && timbuf[4] <= 59 && timbuf[5] <= 59) {
            printf("PASS: sys$numtim values are sane\n");
        } else {
            printf("FAIL: sys$numtim returned out-of-range values\n");
            failures++;
        }
    } else {
        printf("FAIL: sys$numtim (status=0x%x)\n", status);
        failures++;
    }

    /* Test 3: lib$date_time - formatted date/time string */
    char date_buf[32];
    struct dsc$descriptor_s date_desc;
    date_desc.dsc$w_length = sizeof(date_buf);
    date_desc.dsc$b_dtype = DSC$K_DTYPE_T;
    date_desc.dsc$b_class = DSC$K_CLASS_S;
    date_desc.dsc$a_pointer = date_buf;

    status = lib$date_time(&date_desc);
    if ($VMS_STATUS_SUCCESS(status)) {
        /* Null-terminate for printing */
        int len = date_desc.dsc$w_length < sizeof(date_buf) ? date_desc.dsc$w_length : sizeof(date_buf) - 1;
        date_buf[len] = '\0';
        printf("PASS: lib$date_time (\"%s\")\n", date_buf);

        /* Check format has expected characters (dd-MMM-yyyy hh:mm:ss) */
        if (len >= 20 && date_buf[2] == '-' && date_buf[6] == '-' &&
            date_buf[11] == ' ' && date_buf[14] == ':' && date_buf[17] == ':') {
            printf("PASS: lib$date_time format is correct\n");
        } else {
            printf("FAIL: lib$date_time format is unexpected\n");
            failures++;
        }
    } else {
        printf("FAIL: lib$date_time (status=0x%x)\n", status);
        failures++;
    }

    /* Test 4: lib$day - get day number */
    int32_t day_num;
    status = lib$day(&day_num, NULL, NULL);
    if ($VMS_STATUS_SUCCESS(status) && day_num > 0) {
        printf("PASS: lib$day returned day number %d\n", day_num);
    } else {
        printf("FAIL: lib$day (status=0x%x, day=%d)\n", status, day_num);
        failures++;
    }

    /* Test 5: lib$day_of_week */
    int32_t dow;
    status = lib$day_of_week(NULL, &dow);
    if ($VMS_STATUS_SUCCESS(status) && dow >= 1 && dow <= 7) {
        const char *day_names[] = {"", "Monday", "Tuesday", "Wednesday",
                                   "Thursday", "Friday", "Saturday", "Sunday"};
        printf("PASS: lib$day_of_week returned %d (%s)\n", dow, day_names[dow]);
    } else {
        printf("FAIL: lib$day_of_week (status=0x%x, dow=%d)\n", status, dow);
        failures++;
    }

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All time service tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
