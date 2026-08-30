/*
 * ovmx_boot_acp_read.c - PID 1's ACP-read bootstrap bridge (vms-5f0).
 *
 * The atomic flip makes SYS$DISK a genuine Files-11 (ODS-2) volume owned by
 * the executive ACP and retires the /vms POSIX passthrough. But the Linux
 * kernel activates a VMS image the Unix way: execve() maps a MAIN image's
 * PT_LOAD and opens its PT_INTERP (IMGACT.EXE) by POSIX path before any OVMX
 * code runs. So the small set of images the boot chain genuinely
 * fork()+execve()s -- PROVISION.EXE, DCL.EXE, JOB_CONTROL.EXE, LOGINOUT.EXE,
 * and the PT_INTERP IMGACT.EXE -- must have a POSIX home even though the
 * volume they live on is now an ODS-2 block device the ACP owns.
 *
 * This module is that bridge: it reads each first-hop image FROM THE GENUINE
 * ODS-2 VOLUME THROUGH THE EXECUTIVE ACP -- IO$_ACCESS + IO$_READVBLK over
 * /dev/vms, via the SAME proven imgact_acp.c code IMGACT.EXE itself uses --
 * and writes it into the OVMX_BOOT_STAGE_DIR tmpfs. The bytes come from the
 * ACP, never a /vms read (INV-6); tmpfs is only the Linux-exec handoff.
 *
 * It backs imgact_acp.c's three host primitives with libc (PID 1 is a hosted,
 * statically linked musl image), exactly as tests/qemu/test_syssvc_imgact_acp.c
 * does -- so the staging read exercises the real ACP file-access path, not a
 * re-implementation of it. NO POSIX FALLBACK: if /dev/vms is unreachable or the
 * boot volume is not ACP-mounted the read fails honestly (SS$_NOSUCHDEV), and a
 * missing image is SS$_NOSUCHFILE -- it is never silently read off /vms.
 */

/* Feature-test macros (_POSIX_C_SOURCE / _DEFAULT_SOURCE) come from the
 * ovmx_init CMake target, the same as every other file it compiles. */
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ssdef.h"
#include "ovmx_layout.h"
#include "imgact_acp.h"
#include "ovmx_boot_acp_read.h"
#include "ovmx_boot.h"          /* ovmx_boot_system_disk_unit() -- the DISCOVERED unit */

/* The ACP-mounted boot/system unit -- the DISCOVERED device (vms-9f5), the SAME
 * unit PID 1 $MOUNTs via ovmx_boot_acp_mount_system_disk() and IMGACT.EXE
 * activates images from. NOT the compile-time SYSDISK_DEVICE default: when the
 * boot volume is a non-default disk (e.g. VDA100:), $ASSIGNing the compile
 * default (VDA0:) would probe an unmounted device and every image would read as
 * absent -- ovmx_boot_system_disk_unit() returns whatever PID 1 actually mounted. */

/* --------------------------------------------------------------------------
 * imgact_acp.c host primitives, libc-backed (the freestanding/hosted seam).
 * -------------------------------------------------------------------------- */
int imgact_acp_dev_open(void)
{
    return open("/dev/vms", O_RDWR);
}

void imgact_acp_dev_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

long imgact_acp_dev_ioctl(int fd, unsigned long req, void *arg)
{
    return ioctl(fd, req, arg) < 0 ? -1 : 0;
}

/* --------------------------------------------------------------------------
 * Presence probe + staging.
 * -------------------------------------------------------------------------- */

/*
 * ovmx_boot_acp_present - true iff `acp_path` opens on the ACP-mounted boot
 * volume. A pure read: $ASSIGN a file-class channel + IO$_ACCESS the file, then
 * release. SS$_NOSUCHFILE (absent) / SS$_NOSUCHDEV (no executive / not mounted)
 * both read as "not present" -- fail-honest, never a POSIX stat of /vms.
 */
int ovmx_boot_acp_present(const char *acp_path)
{
    struct imgact_acp_file f;
    uint32_t st = imgact_acp_open(&f, ovmx_boot_system_disk_unit(), acp_path);
    if ($VMS_STATUS_SUCCESS(st)) {
        imgact_acp_close(&f);
        return 1;
    }
    return 0;
}

/*
 * ovmx_boot_acp_stage - copy `acp_path` off the ACP-mounted boot volume into
 * `dest` (a tmpfs path), mode 0755, so the Linux kernel can execve it. Returns
 * a VMS status: SS$_NORMAL on success, the ACP open status on a read failure
 * (SS$_NOSUCHFILE / SS$_NOSUCHDEV), or SS$_ABORT if the tmpfs write fails.
 */
uint32_t ovmx_boot_acp_stage(const char *acp_path, const char *dest)
{
    struct imgact_acp_file f;
    uint32_t st = imgact_acp_open(&f, ovmx_boot_system_disk_unit(), acp_path);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;

    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        imgact_acp_close(&f);
        return SS$_ABORT;
    }

    uint32_t rc = SS$_NORMAL;
    long off = 0;
    static char buf[65536];
    for (;;) {
        long r = imgact_acp_pread(&f, buf, sizeof(buf), off);
        if (r < 0) {
            rc = SS$_ABORT;
            break;
        }
        if (r == 0)
            break;                    /* EOF */
        char *p = buf;
        long left = r;
        while (left > 0) {
            ssize_t w = write(fd, p, (size_t)left);
            if (w <= 0) {
                rc = SS$_ABORT;
                break;
            }
            p += w;
            left -= w;
        }
        if (rc != SS$_NORMAL)
            break;
        off += r;
    }

    fchmod(fd, 0755);
    close(fd);
    imgact_acp_close(&f);
    return rc;
}
