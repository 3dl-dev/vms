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

/* ORACLE-PINNED (vms-68c), docs/oracle/vax73-event-flags.md: $SSDEF in
 * SYS$LIBRARY:STARLET.MLB on the reference lab VAX V7.3 gives SS$_WASCLR 1
 * (the SAME value as SS$_NORMAL -- that alias is VMS) and SS$_ILLEFC 236.
 * This file previously carried 5 and 44, matching src/kernel/vms_internal.h,
 * which the same oracle disproves: F$MESSAGE(5) is %NONAME-?-NOMSG and
 * F$MESSAGE(44) is %SYSTEM-F-ABORT. Both sides are corrected together.
 *
 * NOTE for the reader of the assertions below: since SS_WASCLR == SS_NORMAL,
 * "status == SS_WASCLR" no longer discriminates against a bare success. The
 * assertions that need to discriminate check SS_WASSET, which is distinct. */
#define SS_NORMAL   1
#define SS_WASSET   9
#define SS_WASCLR   1
#define SS_ILLEFC   236

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout so an unflushed fork() cannot splice output */
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
    /* negctl-knockon: eflag-setef-status-inverted */
    CHECK(ef.status == SS_WASCLR, "setef(5) returns WASCLR");

    /* 2. Set flag 5 again (should return WASSET) */
    memset(&ef, 0, sizeof(ef));
    ef.efn = 5;
    ioctl(fd, VMS_IOCTL_SETEF, &ef);
    /* negctl-knockon: eflag-setef-status-inverted */
    CHECK(ef.status == SS_WASSET, "setef(5) again returns WASSET");

    /* 3. Read event flag 5 */
    struct vms_ef_read_args rd = {0};
    rd.efn = 5;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    /* negctl-knockon: eflag-readef-status-inverted */
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
    /* negctl: eflag-clref-noop */
    /* negctl-knockon: eflag-readef-status-inverted */
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
    /* negctl: eflag-clref-noop */
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
    /* negctl-knockon: eflag-setef-status-inverted */
    CHECK(ef.status == SS_WASCLR, "setef(40) in cluster 1 returns WASCLR");

    memset(&rd, 0, sizeof(rd));
    rd.efn = 40;
    ioctl(fd, VMS_IOCTL_READEF, &rd);
    CHECK((rd.state & (1 << 8)) != 0, "cluster 1 has bit 8 (flag 40) set");

    close(fd);

    printf("=== test_kmod_eflag: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
