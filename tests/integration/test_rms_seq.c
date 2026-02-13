/* Test: RMS sequential file I/O */
#include <stdio.h>
#include <string.h>
#include <rms/rms.h>
#include <ssdef.h>
#include <rmsdef.h>

int main(void) {
    struct FAB fab;
    struct RAB rab;
    uint32_t status;
    char buffer[256];

    printf("=== RMS Sequential File Test ===\n");

    /* Create a sequential file */
    fab = cc$rms_fab;
    fab.fab$l_fna = "/tmp/test_rms_seq.dat";
    fab.fab$b_fns = (uint8_t)strlen(fab.fab$l_fna);
    fab.fab$b_fac = FAB$M_PUT;
    fab.fab$b_rfm = FAB$C_STMLF;

    status = sys$create(&fab);
    if (!(status & 1)) {
        printf("FAIL: sys$create returned %08X\n", status);
        return 1;
    }

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    status = sys$connect(&rab);
    if (!(status & 1)) {
        printf("FAIL: sys$connect returned %08X\n", status);
        return 1;
    }

    /* Write records */
    const char *lines[] = {"Record 1", "Record 2", "Record 3"};
    for (int i = 0; i < 3; i++) {
        rab.rab$l_rbf = (char *)lines[i];
        rab.rab$w_rsz = (uint16_t)strlen(lines[i]);
        status = sys$put(&rab);
        if (!(status & 1)) {
            printf("FAIL: sys$put returned %08X\n", status);
            return 1;
        }
    }

    sys$close(&fab);

    /* Re-open and read back */
    fab = cc$rms_fab;
    fab.fab$l_fna = "/tmp/test_rms_seq.dat";
    fab.fab$b_fns = (uint8_t)strlen(fab.fab$l_fna);
    fab.fab$b_fac = FAB$M_GET;
    fab.fab$b_rfm = FAB$C_STMLF;

    status = sys$open(&fab);
    if (!(status & 1)) {
        printf("FAIL: sys$open returned %08X\n", status);
        return 1;
    }

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$l_ubf = buffer;
    rab.rab$w_usz = sizeof(buffer);
    status = sys$connect(&rab);
    if (!(status & 1)) {
        printf("FAIL: sys$connect (read) returned %08X\n", status);
        return 1;
    }

    int count = 0;
    while (1) {
        status = sys$get(&rab);
        if (status == RMS$_EOF) break;
        if (!(status & 1)) {
            printf("FAIL: sys$get returned %08X\n", status);
            return 1;
        }
        buffer[rab.rab$w_rsz] = '\0';
        printf("Read: %s\n", buffer);
        count++;
    }

    sys$close(&fab);

    if (count != 3) {
        printf("FAIL: Expected 3 records, got %d\n", count);
        return 1;
    }

    printf("All RMS sequential tests passed!\n");
    return 0;
}
