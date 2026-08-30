/*
 * test_kmod_disk.c - the executive names the machine's disks (vms-3e8)
 *
 * A VMS disk unit is a thing the EXECUTIVE knows about. On real VMS a disk
 * driver enters VDA0:, VDA100:, ... in the I/O database at boot, and from that
 * moment every process on the node can name them. OVMX's executive does the
 * same thing at module init: it enumerates the node's virtio block devices and
 * creates a DK-class unit for each -- VDA0: for vda, VDA100: for vdb, and so on
 * (src/kernel/vms_devtab.c). This suite proves that against a REAL vms.ko in
 * QEMU, with TWO attached virtio disks (run_tests.sh wires /dev/vda and
 * /dev/vdb), because there is no way to fake it: the units either exist in the
 * executive's table or they do not.
 *
 * THE DECISIVE CHECK is that the unit resolves to the RIGHT backing block
 * device, cross-checked against the ground truth userspace can read for itself:
 * stat("/dev/vda") gives the dev_t the kernel assigned vda, and the executive
 * must hand back that same major:minor for VDA0:. A resolver that returned a
 * plausible constant would pass "VDA0: resolves" and fail this.
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
    uint32_t vdc_maj = 0, vdc_min = 0, vdd_maj = 0, vdd_min = 0;
    uint32_t vde_maj = 0, vde_min = 0;
    int have_vda, have_vdb, have_vdc, have_vdd, have_vde;

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
     * attaches FIVE virtio disks (vms-3e8e added vde -> VDA400:, the ODS-2 image
     * volume the IMGACT-over-ACP test mounts), so all five nodes must be present
     * in devtmpfs. */
    have_vda = (stat_devt("/dev/vda", &vda_maj, &vda_min) == 0);
    have_vdb = (stat_devt("/dev/vdb", &vdb_maj, &vdb_min) == 0);
    have_vdc = (stat_devt("/dev/vdc", &vdc_maj, &vdc_min) == 0);
    have_vdd = (stat_devt("/dev/vdd", &vdd_maj, &vdd_min) == 0);
    have_vde = (stat_devt("/dev/vde", &vde_maj, &vde_min) == 0);
    CHECK(have_vda, "/dev/vda is present (first virtio disk attached to the guest)");
    CHECK(have_vdb, "/dev/vdb is present (second virtio disk attached to the guest)");
    CHECK(have_vdc, "/dev/vdc is present (third virtio disk attached to the guest)");
    CHECK(have_vdd, "/dev/vdd is present (fourth virtio disk attached to the guest)");
    CHECK(have_vde, "/dev/vde is present (fifth virtio disk attached to the guest)");

    /* --------------------------------------------------------------
     * 1. VDA0: exists in the executive's table -- nothing in this
     *    process created it -- and it resolves to vda's real dev_t.
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve("VDA0:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "VDA0: exists in the executive's table without any process creating it");
    /* negctl: disk-backing-not-resolved */
    CHECK(strcmp(backing, "vda") == 0,
          "VDA0: backing device is vda (the executive's enumeration)");
    CHECK(have_vda && maj == vda_maj && min == vda_min,
          "VDA0: backing dev_t matches /dev/vda as userspace stat()s it");

    /* --------------------------------------------------------------
     * 2. VDA100: is the second disk, vdb.
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    maj = min = 0;
    status = vms_kif_disk_resolve("VDA100:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "VDA100: exists in the executive's table (the second disk)");
    /* negctl-knockon: disk-backing-not-resolved */
    CHECK(strcmp(backing, "vdb") == 0,
          "VDA100: backing device is vdb (the executive's enumeration)");
    CHECK(have_vdb && maj == vdb_maj && min == vdb_min,
          "VDA100: backing dev_t matches /dev/vdb as userspace stat()s it");

    /* --------------------------------------------------------------
     * 3. VDA200: is the third disk, vdc (the generated multi-version ODS-2
     *    volume the $SEARCH test mounts; vms-a0b added it to run_tests.sh).
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    maj = min = 0;
    status = vms_kif_disk_resolve("VDA200:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "VDA200: exists in the executive's table (the third disk)");
    CHECK(strcmp(backing, "vdc") == 0,
          "VDA200: backing device is vdc (the executive's enumeration)");
    CHECK(have_vdc && maj == vdc_maj && min == vdc_min,
          "VDA200: backing dev_t matches /dev/vdc as userspace stat()s it");

    /* --------------------------------------------------------------
     * 4. VDA300: is the fourth disk, vdd (the generated system-disk ODS-2
     *    volume the directory-logical resolution test mounts; vms-0044 added
     *    it to run_tests.sh).
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    maj = min = 0;
    status = vms_kif_disk_resolve("VDA300:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "VDA300: exists in the executive's table (the fourth disk)");
    CHECK(strcmp(backing, "vdd") == 0,
          "VDA300: backing device is vdd (the executive's enumeration)");
    CHECK(have_vdd && maj == vdd_maj && min == vdd_min,
          "VDA300: backing dev_t matches /dev/vdd as userspace stat()s it");

    /* --------------------------------------------------------------
     * 5. VDA400: is the fifth disk, vde (the generated ODS-2 image volume the
     *    IMGACT-over-ACP test mounts; vms-3e8e added it to run_tests.sh).
     * -------------------------------------------------------------- */
    memset(backing, 0, sizeof(backing));
    maj = min = 0;
    status = vms_kif_disk_resolve("VDA400:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NORMAL,
          "VDA400: exists in the executive's table (the fifth disk)");
    CHECK(strcmp(backing, "vde") == 0,
          "VDA400: backing device is vde (the executive's enumeration)");
    CHECK(have_vde && maj == vde_maj && min == vde_min,
          "VDA400: backing dev_t matches /dev/vde as userspace stat()s it");

    /* --------------------------------------------------------------
     * 6. Negative controls -- a resolver that always succeeded would be
     *    indistinguishable from one that works.
     * -------------------------------------------------------------- */
    /* Five disks are attached (vda..vde), so there is no sixth unit. */
    memset(backing, 0, sizeof(backing));
    status = vms_kif_disk_resolve("VDA500:", backing, sizeof(backing), &maj, &min);
    CHECK(status == SS_NOSUCHDEV,
          "a disk unit that does not exist reports SS$_NOSUCHDEV (no sixth disk attached)");

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
