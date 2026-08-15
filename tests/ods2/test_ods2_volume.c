/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_volume.c - proof for the userspace MOUNTED-VOLUME table
 * (src/vmsfs/vmsfs_volume.c), the additive "B2" substrate of the ODS-2
 * real-runtime flip (vms-351, epic vms-5eb, docs/design-ods2-runtime-flip.md §4).
 *
 * WHAT THIS PROVES, AND WHY. The atomic RMS/DCL/MOUNT flip (rungs R2/R3/R5/R6)
 * needs a process-wide mapping from a VMS device (DKA0:) to an OPEN fd on its
 * backing block device plus a cached, validated genuine-ODS-2 volume handle
 * (ods2_bdev_t), populated at boot by PID 1. This is the SAME call PID 1's
 * register_system_volume() (src/ovmx_init/ovmx_init.c) makes at boot, driven
 * here over a genuine ODS-2 loop image rather than /dev/vda -- because today's
 * live boot disk is still the bespoke VMFS format (the R6 boot-master flip has
 * not landed), so a full boot-time resolve is not yet feasible on the real
 * disk. See tests/qemu/test_ods2_boot_register.sh for the lighter QEMU
 * assertion that PID 1 runs the registration on the real device.
 *
 *   - build a genuine ODS-2 volume carrying the real system-disk hierarchy
 *     shape ([000000]->[SYS0]->[SYS0.SYSCOMMON]->[SYS0.SYSCOMMON.SYSEXE] with
 *     files), lay it on a real loop image fd;
 *   - vmsfs_volume_register("DKA0", <that image>) -> SS$_NORMAL, and
 *     vmsfs_volume_handle("DKA0") returns a cached, validated handle;
 *   - through THAT cached handle, resolve [SYS0.SYSCOMMON.SYSEXE]LOGIN.COM via
 *     a real MFD->SYS0->SYSCOMMON->SYSEXE FID chain and read its content back
 *     as VAR text -- the vms-351 done-condition, over the real block device,
 *     never through the /vms POSIX passthrough (this test opens no /vms path);
 *   - FAIL-HONEST (Rule 9 / INV-6): registering a non-ODS-2 file registers
 *     NOTHING (SS$_DEVNOTMOUNT + honest ods2 error, handle stays NULL), and a
 *     missing backing path fails SS$_NOSUCHDEV -- never a per-process fake;
 *   - unregister closes the handle; the lookup then returns NULL.
 *
 * Every fact is verified against the ods2.h structs / the genuine reader,
 * never via POSIX stat()/opendir().
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/volume.h"
#include "vmsfs/ods2.h"
#include "ssdef.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    } else {                                                           \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                  \
} while (0)

#define VOL_MB     4u
#define VOL_LABEL  "OVMXSYS"

static const char *const LOGIN_TEXT =
    "$ SET NOON\n$ WRITE SYS$OUTPUT \"HI\"\n";

/*
 * Build a genuine ODS-2 volume with the system-disk hierarchy carrying
 * [SYS0.SYSCOMMON.SYSEXE]LOGIN.COM;1, and lay it onto a fresh loop image file
 * at `path` (mkstemp template, filled in). Returns 0 on success. This is
 * exactly the writer sequence test_ods2_path.c uses -- the shape the R6
 * boot-master builder will emit.
 */
static int build_ods2_image(char *path)
{
    uint32_t total_blocks =
        (uint32_t)((uint64_t)VOL_MB * 1024 * 1024 / ODS2_BLOCK_SIZE);
    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = (uint8_t *)calloc(1, image_len);
    if (!image)
        return -1;

    ods2_format_params_t params = { total_blocks,
                                    total_blocks / 100 < ODS2_RESFILES
                                        ? ODS2_RESFILES : total_blocks / 100,
                                    VOL_LABEL };
    ods2_wvolume_t wvol;
    ods2_fid_t sys0, syscommon, sysexe, login_v1;
    int rc = -1;

    if (ods2_volume_format(image, image_len, &params, &wvol) != ODS2_OK)
        goto out;

    if (ods2_wvolume_create_dir(&wvol, "SYS0.DIR", 1, wvol.mfd_fid, &sys0) != ODS2_OK ||
        ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "SYS0.DIR", 1, sys0) != ODS2_OK ||
        ods2_wvolume_create_dir(&wvol, "SYSCOMMON.DIR", 1, sys0, &syscommon) != ODS2_OK ||
        ods2_wvolume_dir_insert(&wvol, sys0, "SYSCOMMON.DIR", 1, syscommon) != ODS2_OK ||
        ods2_wvolume_create_dir(&wvol, "SYSEXE.DIR", 1, syscommon, &sysexe) != ODS2_OK ||
        ods2_wvolume_dir_insert(&wvol, syscommon, "SYSEXE.DIR", 1, sysexe) != ODS2_OK)
        goto out;

    if (ods2_wvolume_create_file(&wvol, "LOGIN.COM", 1,
                                 (const uint8_t *)LOGIN_TEXT, strlen(LOGIN_TEXT),
                                 sysexe, &login_v1) != ODS2_OK ||
        ods2_wvolume_dir_insert(&wvol, sysexe, "LOGIN.COM", 1, login_v1) != ODS2_OK)
        goto out;

    int fd = mkstemp(path);
    if (fd < 0)
        goto out;
    if (ftruncate(fd, (off_t)image_len) != 0 ||
        pwrite(fd, image, wvol.next_free_lbn * ODS2_BLOCK_SIZE, 0) < 0) {
        close(fd);
        goto out;
    }
    close(fd);
    rc = 0;

out:
    free(image);
    return rc;
}

int main(void)
{
    printf("=== test_ods2_volume (vms-351, epic vms-5eb B2): userspace "
           "mounted-volume table register + resolve-through-handle ===\n");

    char img_path[] = "/tmp/test_ods2_volume.XXXXXX";
    CHECK(build_ods2_image(img_path) == 0, "build a genuine ODS-2 loop image");
    if (g_failures) return 1;

    /* ---- REGISTER: DKA0: -> (open fd + cached ods2_bdev_t) ---- */
    ods2_status_t ost = ODS2_ERR_IO;
    int st = vmsfs_volume_register("DKA0", img_path, &ost);
    CHECK(st == SS$_NORMAL, "vmsfs_volume_register(DKA0:) succeeds on a genuine ODS-2 volume");
    CHECK(ost == ODS2_OK, "register reports ODS2_OK for the genuine volume");
    CHECK(vmsfs_volume_count() == 1, "table holds exactly one registered volume");

    /* Colon and case are normalized: DKA0:, dka0, DKA0 name the same slot. */
    const ods2_bdev_t *bv = vmsfs_volume_handle("DKA0:");
    CHECK(bv != NULL, "vmsfs_volume_handle(\"DKA0:\") returns the cached handle (colon-normalized)");
    CHECK(vmsfs_volume_handle("dka0") == bv,
          "handle lookup is case-insensitive (dka0 == DKA0)");
    CHECK(bv != NULL && bv->nblocks > 0, "cached handle carries a validated volume geometry");

    /* ---- RESOLVE + READ through the cached handle (the done-condition) ---- */
    const char *sysexe_comps[] = { "SYS0", "SYSCOMMON", "SYSEXE" };
    uint8_t filehdr[ODS2_BLOCK_SIZE];
    ods2_fid_t file_fid;

    st = ods2_bdev_resolve_file(bv, sysexe_comps, 3, "LOGIN.COM", /*highest*/0,
                                &file_fid, filehdr, sizeof(filehdr));
    CHECK(st == ODS2_OK,
          "resolve [SYS0.SYSCOMMON.SYSEXE]LOGIN.COM through the mounted-volume handle "
          "(real MFD->SYS0->SYSCOMMON->SYSEXE FID chain, never POSIX opendir)");

    char textbuf[4096];
    size_t textlen = 0;
    st = ods2_bdev_read_file_text(bv, filehdr, textbuf, sizeof(textbuf), &textlen);
    CHECK(st == ODS2_OK, "read LOGIN.COM as VAR text off the block device via the handle");
    CHECK(textlen == strlen(LOGIN_TEXT) &&
          memcmp(textbuf, LOGIN_TEXT, textlen) == 0,
          "LOGIN.COM content round-trips to what the writer laid down");

    /* ---- FAIL-HONEST: a non-ODS-2 backing store registers NOTHING ---- */
    char junk_path[] = "/tmp/test_ods2_volume_junk.XXXXXX";
    int jfd = mkstemp(junk_path);
    CHECK(jfd >= 0, "create a non-ODS-2 junk image");
    if (jfd >= 0) {
        /* A megabyte of non-DECFILE11B bytes: no valid home block at LBN 1. */
        static const char zeros[4096];
        for (int i = 0; i < 256; i++)
            (void)!write(jfd, zeros, sizeof(zeros));
        close(jfd);

        ost = ODS2_OK;
        st = vmsfs_volume_register("DKB0", junk_path, &ost);
        CHECK(st == SS$_DEVNOTMOUNT,
              "registering a non-ODS-2 volume fails SS$_DEVNOTMOUNT (fail-honest, Rule 9)");
        CHECK(ost != ODS2_OK,
              "register surfaces the reader's honest ODS-2 error, not a fake success");
        CHECK(vmsfs_volume_handle("DKB0") == NULL,
              "no handle is cached for a non-ODS-2 volume (INV-6: no per-process fake)");
        CHECK(vmsfs_volume_count() == 1,
              "the table did not grow for the rejected non-ODS-2 volume");
        unlink(junk_path);
    }

    /* A backing path that cannot be opened fails honestly too. */
    st = vmsfs_volume_register("DKC0", "/nonexistent/ovmx/dka0.dsk", NULL);
    CHECK(st == SS$_NOSUCHDEV,
          "registering an unopenable backing path fails SS$_NOSUCHDEV");
    CHECK(vmsfs_volume_handle("DKC0") == NULL, "no handle for the unopenable device");

    /* Bad arguments. */
    CHECK(vmsfs_volume_register(NULL, img_path, NULL) == SS$_BADPARAM,
          "NULL devname rejected SS$_BADPARAM");
    CHECK(vmsfs_volume_register("DKD0", NULL, NULL) == SS$_BADPARAM,
          "NULL backing_path rejected SS$_BADPARAM");

    /* ---- UNREGISTER: the cached handle is dropped ---- */
    st = vmsfs_volume_unregister("DKA0");
    CHECK(st == SS$_NORMAL, "vmsfs_volume_unregister(DKA0:) succeeds");
    CHECK(vmsfs_volume_handle("DKA0") == NULL, "handle lookup returns NULL after unregister");
    CHECK(vmsfs_volume_count() == 0, "table is empty after unregister");
    CHECK(vmsfs_volume_unregister("DKA0") == SS$_NOSUCHDEV,
          "unregistering an unregistered device fails SS$_NOSUCHDEV");

    unlink(img_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
