/*
 * test_kmod_disk.c - the executive names the machine's disks (vms-3e8)
 *
 * A VMS disk unit is a thing the EXECUTIVE knows about. On real VMS a disk
 * driver enters DKA0:, DKA100:, ... in the I/O database at boot, and from that
 * moment every process on the node can name them. OVMX's executive does the
 * same thing at module init: it enumerates the node's virtio block devices and
 * creates a DK-class unit for each -- DKA0: for vda, DKA100: for vdb, and so on
 * (src/kernel/vms_devtab.c). This suite proves that against a REAL vms.ko in
 * QEMU, with TWO attached virtio disks (run_tests.sh wires /dev/vda and
 * /dev/vdb), because there is no way to fake it: the units either exist in the
 * executive's table or they do not.
 *
 * THE DECISIVE CHECK is that the unit resolves to the RIGHT backing block
 * device, cross-checked against the ground truth userspace can read for itself:
 * stat("/dev/vda") gives the dev_t the kernel assigned vda, and the executive
 * must hand back that same major:minor for DKA0:. A resolver that returned a
 * plausible constant would pass "DKA0: resolves" and fail this.
 *
 * The process NEVER scans /sys/block: it asks the executive
 * (vms_kif_disk_resolve), which is the whole point -- the fact lives in the
 * executive (CLAUDE.md Rule 11). The stat() here is the TEST's independent
 * oracle, not how a product process would learn the backing device.
 *
 * Runs against the real userspace client (src/libvmssys/vms_kif.c), so the
 * client libvms will call is the client under test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "vms_kif.h"

#define SS_NORMAL       1
#define SS_IVDEVNAM     608
#define SS_NOSUCHDEV    2680

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* The dev_t userspace sees for a /dev node, or 0 if it is not there. This is
 * the TEST's independent oracle -- the value the executive must agree with. */
static int stat_devt(const char *path, uint32_t *maj, uint32_t *min)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    *maj = major(st.st_rdev);
    *min = minor(st.st_rdev);
    return 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* vms-b5b: line-buffer stdout */

    char backing[16];
    uint32_t maj = 0, min = 0, status;
    uint32_t vda_maj = 0, vda_min = 0, vdb_maj = 0, vdb_min = 0;
    int have_vda, have_vdb;

    printf("=== test_kmod_disk: the executive names the machine's disks ===\n");

    if (vms_kif_open() < 0) {
        printf("  FAIL: cannot open /dev/vms (executive absent)\n");
        printf("=== test_kmod_disk: 0 passed, 1 failed ===\n");
        return 1;
    }
    if (vms_kif_register(NULL) != SS_NORMAL) {
        printf("  FAIL: VMS_IOCTL_REGISTER rejected\n");
        printf("=== test_kmod_disk: 0 passed, 1 failed ===\n");
        return 1;
    }

    /* The ground truth, read independently of the executive. run_tests.sh
     * attaches two virtio disks, so both nodes must be present in devtmpfs. */
    have_vda = (stat_devt("/dev/vda", &vda_maj, &vda_min) == 0);
    have_vdb = (stat_devt("/dev/vdb", &vdb_maj, &vdb_min) == 0);
    CHECK(have_vda, "/dev/vda is present (first virtio disk attached to the guest)");
    CHECK(have_vdb, "/dev/vdb is present (second virtio disk attached to the guest)");

    /* --------------------------------------------------------------
     * 1. DKA0: exists in the executive's table -- nothing in this
     *    process created it -- and it resolves to vda's real dev_t.
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve("DKA0:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "DKA0: exists in the executive's table without any process creating it");
    /* negctl: disk-backing-not-resolved */
    CHECK(strcmp(backing, "vda") == 0,
          "DKA0: backing device is vda (the executive's enumeration)");
    CHECK(have_vda && maj == vda_maj && min == vda_min,
          "DKA0: backing dev_t matches /dev/vda as userspace stat()s it");

    /* --------------------------------------------------------------
     * 2. DKA100: is the second disk, vdb.
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    maj = min = 0;
    status = vms_kif_disk_resolve("DKA100:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "DKA100: exists in the executive's table (the second disk)");
    /* negctl-knockon: disk-backing-not-resolved */
    CHECK(strcmp(backing, "vdb") == 0,
          "DKA100: backing device is vdb (the executive's enumeration)");
    CHECK(have_vdb && maj == vdb_maj && min == vdb_min,
          "DKA100: backing dev_t matches /dev/vdb as userspace stat()s it");

    /* --------------------------------------------------------------
     * 3. Negative controls -- a resolver that always succeeded would be
     *    indistinguishable from one that works.
     * -------------------------------------------------------------- */
    /* Only two disks are attached, so there is no third unit. */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve("DKA200:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NOSUCHDEV,
          "a disk unit that does not exist reports SS$_NOSUCHDEV (no third disk attached)");

    /* OPA0: exists, but it is a TERMINAL -- it has no backing block device. */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve("OPA0:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_IVDEVNAM,
          "resolving a non-disk device (the console OPA0:) reports SS$_IVDEVNAM");

    /* A malformed name is not a device name at all. */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve("not a device", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_IVDEVNAM,
          "a malformed device name reports SS$_IVDEVNAM");

    vms_kif_close();

    printf("=== test_kmod_disk: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
