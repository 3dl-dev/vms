/*
 * test_kmod_errcnt.c - the executive counts REAL device I/O errors (vms-5f82)
 *
 * SHOW ERROR and F$GETDVI(...,"ERRCNT") report a per-device error count. #892
 * made SHOW ERROR a genuine READER of that count -- it walks the
 * executive-resident device table with $DEVICE_SCAN and prints one row per unit
 * whose errcnt > 0 -- but nothing was ever WRITING that field, so it read zero
 * for every device and its non-zero-row path was never exercised at run time.
 * That is the exact "reader of a field nobody sets" gap this suite closes.
 *
 * The executive now has the WRITER: the Files-11 ODS-2 ACP is a disk unit's
 * "driver", and every FAILED raw block read/write it does to a unit's backing
 * device is charged to that unit's errcnt (kernel-core/vmsfs_acp.c ->
 * vms_devtab_note_io_error). This suite proves it against a REAL vms.ko under a
 * real /dev/vms, the only place the executive is real (CLAUDE.md Rule 9):
 *
 *   1. A device with no I/O errors reads ERRCNT 0 (the honest empty state).
 *   2. Force ONE genuine ACP block read to FAIL -- via the test-only
 *      fault-injection knob, which makes exec_blockdev_read_block return the
 *      SAME failure a bad sector produces -- and the unit's ERRCNT moves by
 *      exactly one. The increment is on the executive's REAL error path; only
 *      the failure SOURCE is injected (the knob never touches errcnt).
 *   3. A SECOND injected error moves it to +2 -- it is a COUNTER, not a flag.
 *   4. $DEVICE_SCAN (the SAME walk cmd_show_error() renders) reports the unit
 *      with that non-zero count -- so SHOW ERROR's non-zero-row path is now
 *      reachable at run time, with a true reading, not a fabricated one.
 *   5. With injection DISARMED, the same mount attempt (its block read now
 *      SUCCEEDS) does NOT move ERRCNT: the count tracks real I/O errors, not
 *      mount failures. This is the INV-6 assertion -- errcnt is never seeded and
 *      never bumped speculatively.
 *
 * The trigger is $MOUNT of DKA100: (vdb, a blank non-ODS-2 unit run_tests.sh
 * attaches). $MOUNT reads the home block at LBN 1 off the backing device before
 * anything else; armed, that read fails and the mount is refused fail-honest,
 * and the failed read is charged to DKA100:. DKA100: is non-ODS-2 so it is never
 * recorded in the mount table -- every attempt re-reads the disk, so the trigger
 * is deterministic regardless of what other suites in this boot did.
 *
 * Runs against the real userspace client (src/libvmssys/vms_kif.c), so the
 * client libvms will call is the client under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vms_kif.h"

#define SS_NORMAL       1
#define SS_DEVNOTMOUNT  2688    /* SS$_DEVNOTMOUNT -- not a mountable ODS-2 volume */
#define SS_NOMOREDEV    2648    /* SS$_NOMOREDEV -- $DEVICE_SCAN exhausted */

#define TARGET_DEV      "DKA100:"   /* vdb: the blank, non-ODS-2 unit */

#define FAULT_PARAM     "/sys/module/vms/parameters/vms_ktest_bdev_fault"

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/*
 * Arm (or disarm) the executive's test-only block-I/O fault injection for the
 * backing device (major,minor): the next `count` block ops on it report failure,
 * exactly as a bad sector would. Returns 0 on success. A failure here is a hard
 * error, not a skip -- the whole suite depends on being able to force a genuine
 * I/O error.
 */
static int arm_fault(uint32_t major, uint32_t minor, uint32_t count)
{
    FILE *f = fopen(FAULT_PARAM, "w");
    int rc;

    if (!f)
        return -1;
    rc = fprintf(f, "%u:%u:%u", major, minor, count);
    if (fclose(f) != 0)
        return -1;
    return (rc > 0) ? 0 : -1;
}

/* Read the executive's current error count for a device, or (uint32_t)-1 if the
 * device could not be read. */
static uint32_t errcnt_of(const char *devnam)
{
    struct vms_devinfo info;

    memset(&info, 0, sizeof(info));
    if (vms_kif_getdvi_devnam(devnam, &info) != SS_NORMAL)
        return (uint32_t)-1;
    return info.errcnt;
}

/* Walk $DEVICE_SCAN exactly as cmd_show_error() does and return the errcnt it
 * reports for `devnam` (the value SHOW ERROR would print), or (uint32_t)-1 if
 * the unit was not found in the scan. */
static uint32_t errcnt_via_devscan(const char *devnam)
{
    struct vms_devinfo info;
    uint32_t index = 0;
    uint32_t status;

    while ((status = vms_kif_devscan(&index, &info)) == SS_NORMAL) {
        info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
        if (strcmp(info.devnam, devnam) == 0)
            return info.errcnt;
    }
    return (uint32_t)-1;
}

int main(void)
{
    char backing[16];
    uint32_t maj = 0, min = 0, status;
    uint32_t base, after1, after2, scanned, disarmed;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_kmod_errcnt: the executive counts real device I/O errors ===\n");

    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        printf("=== test_kmod_errcnt: 0 passed, 1 failed ===\n");
        return 1;
    }
    if (vms_kif_register(NULL) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        printf("=== test_kmod_errcnt: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* Resolve the target unit to its backing block device -- the executive's own
     * fact (vms_kif_disk_resolve), so the fault is armed on exactly the dev_t the
     * ACP will read. */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve(TARGET_DEV, backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "DKA100: resolves to its backing block device (executive enumeration)");
    if (status != SS_NORMAL) {
        printf("=== test_kmod_errcnt: %d passed, %d failed ===\n", pass, fail);
        vms_kif_close();
        return 1;
    }

    /* (1) Baseline -- read RELATIVE to whatever this boot's other suites left, so
     * the proof does not depend on test ordering. */
    base = errcnt_of(TARGET_DEV);
    CHECK(base != (uint32_t)-1,
          "DKA100: ERRCNT is readable via $GETDVI (the field SHOW ERROR reads)");

    /* (2) Force ONE genuine ACP block read to fail, then mount -- $MOUNT reads the
     * home block at LBN 1 first, so the injected failure lands on a real read and
     * the mount is refused fail-honest. */
    CHECK(arm_fault(maj, min, 1) == 0,
          "armed test-only block-I/O fault injection for DKA100: backing device");
    status = vms_kif_acp_mount(TARGET_DEV);
    CHECK(status == SS_DEVNOTMOUNT,
          "$MOUNT of DKA100: with a failing home-block read is refused SS$_DEVNOTMOUNT");
    after1 = errcnt_of(TARGET_DEV);
    CHECK(after1 == base + 1,
          "one genuine ACP block-read failure incremented DKA100: ERRCNT by exactly one");

    /* (3) A second injected error -- prove it is a counter, not a set-once flag. */
    CHECK(arm_fault(maj, min, 1) == 0,
          "re-armed the fault for a second block-read failure");
    (void)vms_kif_acp_mount(TARGET_DEV);
    after2 = errcnt_of(TARGET_DEV);
    CHECK(after2 == base + 2,
          "a second block-read failure moved DKA100: ERRCNT to +2 (a real counter)");

    /* (4) $DEVICE_SCAN -- the SAME reader cmd_show_error() uses -- reports the
     * non-zero count, so SHOW ERROR's non-zero-row path is now runtime-reachable. */
    scanned = errcnt_via_devscan(TARGET_DEV);
    CHECK(scanned == after2,
          "$DEVICE_SCAN reports DKA100: with the same non-zero ERRCNT SHOW ERROR would print");

    /* (5) INV-6 -- disarm and mount again: the read now SUCCEEDS (blank, non-ODS-2
     * media), the mount still fails, but ERRCNT does NOT move. The count tracks
     * real I/O errors, never mount failures and never speculation. */
    CHECK(arm_fault(maj, min, 0) == 0,
          "disarmed the fault injection");
    status = vms_kif_acp_mount(TARGET_DEV);
    CHECK(status == SS_DEVNOTMOUNT,
          "$MOUNT of the blank DKA100: still refused (non-ODS-2), now with a SUCCESSFUL read");
    disarmed = errcnt_of(TARGET_DEV);
    CHECK(disarmed == after2,
          "a mount whose block read SUCCEEDED did NOT change ERRCNT (INV-6: no fabricated errors)");

    vms_kif_close();

    printf("=== test_kmod_errcnt: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
