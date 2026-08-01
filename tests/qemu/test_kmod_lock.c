/*
 * test_kmod_lock.c - Test lock manager via /dev/vms ioctl
 *
 * Tests:
 *   1. ENQ a NL lock on a resource
 *   2. Convert NL -> CR
 *   3. Convert CR -> EX
 *   4. DEQ the lock
 *   5. ENQ two compatible locks (CR + CR)
 *   6. ENQ EX with NOQUEUE when CR is held (should fail)
 *   7. GETLKI to query lock state
 *   8. Value block round-trip
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"

#define SS_NORMAL       1
#define SS_NOTQUEUED    40

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    printf("=== test_kmod_lock ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
    }

    /* Register */
    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    reg.init_privs = 0xFFFFFFFFFFFFFFFFULL;
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(reg.status == SS_NORMAL, "register");

    /* 1. ENQ NL lock on "TESTRES1" */
    struct vms_enq_args enq = {0};
    enq.lkmode = LCK_K_NLMODE;
    strncpy(enq.resnam, "TESTRES1", sizeof(enq.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq);
    CHECK(enq.status == SS_NORMAL, "ENQ NL on TESTRES1");
    CHECK(enq.lkid != 0, "got non-zero lock ID");

    uint32_t lkid1 = enq.lkid;
    printf("  INFO: lock ID = %u\n", lkid1);

    /* 2. Convert NL -> CR */
    memset(&enq, 0, sizeof(enq));
    enq.lkid = lkid1;
    enq.lkmode = LCK_K_CRMODE;
    enq.flags = LCK_M_CONVERT;
    strncpy(enq.resnam, "TESTRES1", sizeof(enq.resnam));
    ioctl(fd, VMS_IOCTL_CONVERT, &enq);
    CHECK(enq.status == SS_NORMAL, "convert NL -> CR");

    /* 3. GETLKI to verify */
    struct vms_getlki_args lki = {0};
    lki.lkid = lkid1;
    ioctl(fd, VMS_IOCTL_GETLKI, &lki);
    CHECK(lki.status == SS_NORMAL, "GETLKI succeeds");
    CHECK(lki.granted_mode == LCK_K_CRMODE, "granted mode is CR");
    CHECK(strncmp(lki.resnam, "TESTRES1", 8) == 0, "resource name matches");

    /* 4. Convert CR -> EX */
    memset(&enq, 0, sizeof(enq));
    enq.lkid = lkid1;
    enq.lkmode = LCK_K_EXMODE;
    enq.flags = LCK_M_CONVERT;
    strncpy(enq.resnam, "TESTRES1", sizeof(enq.resnam));
    ioctl(fd, VMS_IOCTL_CONVERT, &enq);
    CHECK(enq.status == SS_NORMAL, "convert CR -> EX");

    /* 5. DEQ the lock */
    struct vms_deq_args deq = {0};
    deq.lkid = lkid1;
    ioctl(fd, VMS_IOCTL_DEQ, &deq);
    CHECK(deq.status == SS_NORMAL, "DEQ lock");

    /* 6. ENQ two compatible locks on same resource */
    struct vms_enq_args enq_a = {0};
    enq_a.lkmode = LCK_K_CRMODE;
    strncpy(enq_a.resnam, "TESTRES2", sizeof(enq_a.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq_a);
    CHECK(enq_a.status == SS_NORMAL, "ENQ CR on TESTRES2");

    struct vms_enq_args enq_b = {0};
    enq_b.lkmode = LCK_K_CRMODE;
    strncpy(enq_b.resnam, "TESTRES2", sizeof(enq_b.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq_b);
    CHECK(enq_b.status == SS_NORMAL, "ENQ second CR on TESTRES2 (compatible)");

    /* 7. Try EX with NOQUEUE while CR is held - should fail */
    struct vms_enq_args enq_c = {0};
    enq_c.lkmode = LCK_K_EXMODE;
    enq_c.flags = LCK_M_NOQUEUE;
    strncpy(enq_c.resnam, "TESTRES2", sizeof(enq_c.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq_c);
    CHECK(enq_c.status == SS_NOTQUEUED, "ENQ EX+NOQUEUE denied (CR held)");

    /* 8. Value block round-trip */
    struct vms_enq_args enq_v = {0};
    enq_v.lkmode = LCK_K_EXMODE;
    enq_v.flags = LCK_M_VALBLK;
    strncpy(enq_v.resnam, "TESTRES3", sizeof(enq_v.resnam));
    memcpy(enq_v.valblk, "HELLO_VMS_LOCK!", 16);
    ioctl(fd, VMS_IOCTL_ENQ, &enq_v);
    CHECK(enq_v.status == SS_NORMAL, "ENQ EX with value block");

    /* DEQ with value block readback */
    struct vms_deq_args deq_v = {0};
    deq_v.lkid = enq_v.lkid;
    ioctl(fd, VMS_IOCTL_DEQ, &deq_v);
    /* Re-acquire and check value block */
    memset(&enq_v, 0, sizeof(enq_v));
    enq_v.lkmode = LCK_K_EXMODE;
    enq_v.flags = LCK_M_VALBLK;
    strncpy(enq_v.resnam, "TESTRES3", sizeof(enq_v.resnam));
    ioctl(fd, VMS_IOCTL_ENQ, &enq_v);
    CHECK(memcmp(enq_v.valblk, "HELLO_VMS_LOCK!", 15) == 0,
          "value block preserved across DEQ/ENQ");

    /* Cleanup */
    deq.lkid = enq_a.lkid; ioctl(fd, VMS_IOCTL_DEQ, &deq);
    deq.lkid = enq_b.lkid; ioctl(fd, VMS_IOCTL_DEQ, &deq);
    deq.lkid = enq_v.lkid; ioctl(fd, VMS_IOCTL_DEQ, &deq);

    close(fd);

    printf("=== test_kmod_lock: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
