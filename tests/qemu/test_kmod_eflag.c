/*
 * test_kmod_eflag.c - Test event flag operations via /dev/vms ioctl
 *
 * Tests:
 *   1. Register, set event flag 5, read it back
 *   2. Clear event flag 5, verify clear
 *   3. Read cluster state, verify bitmask
 *   4. Set multiple flags, verify cluster
 *   5. Test out-of-range flag number
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"

#define SS_NORMAL   1
#define SS_WASSET   9
#define SS_WASCLR   5
#define SS_ILLEFC   44

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    printf("=== test_kmod_eflag ===\n");

    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0) {
        printf("  FAIL: cannot open /dev/vms\n");
        return 1;
    }

    /* Register with all privs */
    struct vms_register_args reg = {0};
    reg.vms_pid = (uint32_t)getpid();
    /* NO init_privs (vms-2b8): VMS_IOCTL_REGISTER no longer carries a
     * privilege mask. It used to, and this line asked for all 64 bits --
     * a process declaring its own privileges, which is the honor system
     * that item removed. The executive now DERIVES the mask from the
     * task's real credentials, and nothing below needs a privilege. */
    ioctl(fd, VMS_IOCTL_REGISTER, &reg);
    CHECK(reg.status == SS_NORMAL, "register");

    /* 1. Set event flag 5 (should return WASCLR since it starts clear) */
    struct vms_ef_args ef = {0};
    ef.efn = 5;
    ioctl(fd, VMS_IOCTL_SETEF, &ef);
    CHECK(ef.status == SS_WASCLR, "setef(5) returns WASCLR");

    /* 2. Set flag 5 again (should return WASSET) */
    memset(&ef, 0, sizeof(ef));
    ef.efn = 5;
    ioctl(fd, VMS_IOCTL_SETEF, &ef);
    CHECK(ef.status == SS_WASSET, "setef(5) again returns WASSET");

    /* 3. Read event flag 5 */
    struct vms_ef_read_args rd = {0};
    rd.efn = 5;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    CHECK(rd.status == SS_WASSET, "readef(5) returns WASSET");
    CHECK((rd.state & (1 << 5)) != 0, "cluster state has bit 5 set");

    /* 4. Clear event flag 5 */
    memset(&ef, 0, sizeof(ef));
    ef.efn = 5;
    ioctl(fd, VMS_IOCTL_CLREF, &ef);
    CHECK(ef.status == SS_WASSET, "clref(5) returns WASSET");

    /* 5. Read again - should be clear */
    memset(&rd, 0, sizeof(rd));
    rd.efn = 5;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    CHECK(rd.status == SS_WASCLR, "readef(5) after clear returns WASCLR");

    /* 6. Set multiple flags and verify cluster */
    ef.efn = 0;  ioctl(fd, VMS_IOCTL_SETEF, &ef);
    ef.efn = 3;  ioctl(fd, VMS_IOCTL_SETEF, &ef);
    ef.efn = 7;  ioctl(fd, VMS_IOCTL_SETEF, &ef);
    ef.efn = 31; ioctl(fd, VMS_IOCTL_SETEF, &ef);

    memset(&rd, 0, sizeof(rd));
    rd.efn = 0;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    uint32_t expected = (1 << 0) | (1 << 3) | (1 << 7) | (1 << 31);
    CHECK(rd.state == expected, "cluster has flags 0,3,7,31 set");

    /* 7. Test out-of-range flag */
    memset(&ef, 0, sizeof(ef));
    ef.efn = 200;  /* > 127 */
    ioctl(fd, VMS_IOCTL_SETEF, &ef);
    CHECK(ef.status == SS_ILLEFC, "setef(200) returns ILLEFC");

    /* 8. Test second cluster (flags 32-63) */
    memset(&ef, 0, sizeof(ef));
    ef.efn = 40;
    ioctl(fd, VMS_IOCTL_SETEF, &ef);
    CHECK(ef.status == SS_WASCLR, "setef(40) in cluster 1 returns WASCLR");

    memset(&rd, 0, sizeof(rd));
    rd.efn = 40;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    CHECK((rd.state & (1 << 8)) != 0, "cluster 1 has bit 8 (flag 40) set");

    close(fd);

    printf("=== test_kmod_eflag: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
