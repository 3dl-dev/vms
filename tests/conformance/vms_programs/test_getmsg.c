/*
 * test_getmsg.c - VMS Message Services Conformance Test
 *
 * Tests sys$getmsg:
 * - Retrieve message text for VMS status codes
 * - Test various MSG$M_ flags (TEXT, IDENT, SEVERITY, FACILITY)
 * - Verify formatted output for known status codes
 */

#include <stdio.h>
#include <string.h>
#include <descrip.h>
#include <ssdef.h>
#include <stsdef.h>
#include <msgdef.h>
#include <starlet.h>

int main(void) {
    uint32_t status;
    int failures = 0;
    uint16_t msglen;
    char buffer[256];

    printf("VMS Message Services Test\n");
    printf("==========================\n");

    /* Test 1: sys$getmsg with all components (MSG$M_ALL) */
    struct dsc$descriptor_s msgbuf1;
    msgbuf1.dsc$w_length = sizeof(buffer);
    msgbuf1.dsc$b_dtype = DSC$K_DTYPE_T;
    msgbuf1.dsc$b_class = DSC$K_CLASS_S;
    msgbuf1.dsc$a_pointer = buffer;

    status = sys$getmsg(SS$_NORMAL, &msglen, &msgbuf1, MSG$M_ALL, NULL);
    buffer[msglen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && msglen > 0) {
        printf("PASS: sys$getmsg with MSG$M_ALL (len=%d, msg=\"%s\")\n",
               msglen, buffer);
    } else {
        printf("FAIL: sys$getmsg with MSG$M_ALL (status=0x%x, len=%d)\n",
               status, msglen);
        failures++;
    }

    /* Test 2: sys$getmsg with text only (MSG$M_TEXT) */
    struct dsc$descriptor_s msgbuf2;
    msgbuf2.dsc$w_length = sizeof(buffer);
    msgbuf2.dsc$b_dtype = DSC$K_DTYPE_T;
    msgbuf2.dsc$b_class = DSC$K_CLASS_S;
    msgbuf2.dsc$a_pointer = buffer;

    status = sys$getmsg(SS$_NORMAL, &msglen, &msgbuf2, MSG$M_TEXT, NULL);
    buffer[msglen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && msglen > 0) {
        printf("PASS: sys$getmsg with MSG$M_TEXT (msg=\"%s\")\n", buffer);

        /* Text-only should not have severity prefix like %-S- */
        if (strchr(buffer, '%') == NULL) {
            printf("PASS: MSG$M_TEXT excludes severity prefix\n");
        } else {
            printf("FAIL: MSG$M_TEXT should not include severity prefix\n");
            failures++;
        }
    } else {
        printf("FAIL: sys$getmsg with MSG$M_TEXT (status=0x%x, len=%d)\n",
               status, msglen);
        failures++;
    }

    /* Test 3: sys$getmsg for error status (SS$_ACCVIO) */
    struct dsc$descriptor_s msgbuf3;
    msgbuf3.dsc$w_length = sizeof(buffer);
    msgbuf3.dsc$b_dtype = DSC$K_DTYPE_T;
    msgbuf3.dsc$b_class = DSC$K_CLASS_S;
    msgbuf3.dsc$a_pointer = buffer;

    status = sys$getmsg(SS$_ACCVIO, &msglen, &msgbuf3, MSG$M_ALL, NULL);
    buffer[msglen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && msglen > 0) {
        printf("PASS: sys$getmsg for SS$_ACCVIO (msg=\"%s\")\n", buffer);

        /* Should have error severity indicator */
        if (strstr(buffer, "-E-") != NULL || strstr(buffer, "-F-") != NULL) {
            printf("PASS: Error status includes severity indicator\n");
        } else {
            printf("FAIL: Error status should include severity indicator (got \"%s\")\n", buffer);
            failures++;
        }
    } else {
        printf("FAIL: sys$getmsg for SS$_ACCVIO (status=0x%x, len=%d)\n",
               status, msglen);
        failures++;
    }

    /* Test 4: sys$getmsg with IDENT only */
    struct dsc$descriptor_s msgbuf4;
    msgbuf4.dsc$w_length = sizeof(buffer);
    msgbuf4.dsc$b_dtype = DSC$K_DTYPE_T;
    msgbuf4.dsc$b_class = DSC$K_CLASS_S;
    msgbuf4.dsc$a_pointer = buffer;

    status = sys$getmsg(SS$_NORMAL, &msglen, &msgbuf4, MSG$M_IDENT, NULL);
    buffer[msglen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && msglen > 0) {
        printf("PASS: sys$getmsg with MSG$M_IDENT (msg=\"%s\")\n", buffer);
    } else {
        printf("FAIL: sys$getmsg with MSG$M_IDENT (status=0x%x, len=%d)\n",
               status, msglen);
        failures++;
    }

    /* Test 5: sys$getmsg for success status */
    struct dsc$descriptor_s msgbuf5;
    msgbuf5.dsc$w_length = sizeof(buffer);
    msgbuf5.dsc$b_dtype = DSC$K_DTYPE_T;
    msgbuf5.dsc$b_class = DSC$K_CLASS_S;
    msgbuf5.dsc$a_pointer = buffer;

    status = sys$getmsg(SS$_WASSET, &msglen, &msgbuf5, MSG$M_ALL, NULL);
    buffer[msglen] = '\0';
    if ($VMS_STATUS_SUCCESS(status) && msglen > 0) {
        printf("PASS: sys$getmsg for SS$_WASSET (msg=\"%s\")\n", buffer);
    } else {
        printf("FAIL: sys$getmsg for SS$_WASSET (status=0x%x, len=%d)\n",
               status, msglen);
        failures++;
    }

    /* Summary */
    printf("\n");
    if (failures == 0) {
        printf("All message service tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
