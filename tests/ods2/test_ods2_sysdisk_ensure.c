/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_sysdisk_ensure.c - proof for the SYS$DISK read adapter's
 * CROSS-PROCESS lazy registration (src/vmsfs/ods2_sysdisk.c sysdisk_handle()),
 * the atomic ODS-2 flip's missing substrate (epic vms-5eb,
 * docs/design-ods2-runtime-flip.md).
 *
 * WHAT THIS PROVES, AND WHY. vmsfs_volume.c's mounted-volume table is
 * process-local (an fd + ods2_bdev_t are per-process). PID 1 registers DKA0: at
 * boot, but the flip's live consumers -- RMS in LOGINOUT, DCL in the shell
 * image -- run in OTHER processes whose tables start empty; a NULL handle there
 * would fail SS$_DEVNOTMOUNT and the reroute could never reach login. The
 * adapter closes that gap by lazily registering SYS$DISK in the calling process
 * from the boot-device path PID 1 exports in OVMX_SYSDISK_DEV. This test drives
 * that lazy path EXACTLY as a fresh RMS/DCL process would -- it NEVER calls
 * vmsfs_volume_register itself:
 *
 *   - FAIL-HONEST with no channel: OVMX_SYSDISK_DEV unset and no volume
 *     registered -> ods2_sysdisk_read_file returns SS$_DEVNOTMOUNT and the
 *     table stays empty (never a silent POSIX fallback; Rule 9 / INV-6);
 *   - FAIL-HONEST with a bad channel: OVMX_SYSDISK_DEV naming a NON-ODS-2 file
 *     -> SS$_DEVNOTMOUNT, table still empty (registration rejected the volume);
 *   - LAZY REGISTER: OVMX_SYSDISK_DEV naming a genuine ODS-2 volume ->
 *     ods2_sysdisk_read_file succeeds, reads the verbatim image BYTE-IDENTICAL,
 *     and the volume is now registered (count == 1) though this test never
 *     registered it;
 *   - IDEMPOTENT: a second read over the same (now cached) handle still
 *     succeeds and does not multiply registrations.
 *
 * Every fact is verified against the genuine reader via the adapter, never via
 * POSIX stat()/open()/opendir().
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/sysdisk.h"
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

/* Verbatim binary image (create_file_raw, the R6-master content policy):
 * embedded NULs + high bytes must read back byte-identical. */
static const uint8_t IMAGE_BYTES[] = {
    0x7f, 'E', 'L', 'F', 0x00, 0x01, 0x02, 0x00, 0xff, 0xfe, 0x00, 'X',
    'V', 'M', 'S', '$', 0x00, 0x00, 0x10, 0x20, 0x30, 0xa5, 0x5a, 0x00
};

/*
 * Build a genuine ODS-2 volume with [SYS0.SYSCOMMON.SYSEXE]DCL.EXE;1 (verbatim)
 * and lay it onto a fresh loop image at `path` (mkstemp template, filled in).
 * Mirrors the writer sequence the R6 boot-master emits.
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
    ods2_fid_t sys0, syscommon, sysexe, dclexe;
    int rc = -1;

#define STEP(expr) do { ods2_status_t _s = (expr); \
    if (_s != ODS2_OK) { fprintf(stderr, "  build step failed (%d): %s\n", \
        (int)_s, #expr); goto out; } } while (0)

    STEP(ods2_volume_format(image, image_len, &params, &wvol));

    STEP(ods2_wvolume_create_dir(&wvol, "SYS0.DIR", 1, wvol.mfd_fid, &sys0));
    STEP(ods2_wvolume_dir_insert(&wvol, wvol.mfd_fid, "SYS0.DIR", 1, sys0));
    STEP(ods2_wvolume_create_dir(&wvol, "SYSCOMMON.DIR", 1, sys0, &syscommon));
    STEP(ods2_wvolume_dir_insert(&wvol, sys0, "SYSCOMMON.DIR", 1, syscommon));
    STEP(ods2_wvolume_create_dir(&wvol, "SYSEXE.DIR", 1, syscommon, &sysexe));
    STEP(ods2_wvolume_dir_insert(&wvol, syscommon, "SYSEXE.DIR", 1, sysexe));

    STEP(ods2_wvolume_create_file_raw(&wvol, "DCL.EXE", 1,
                                      IMAGE_BYTES, sizeof(IMAGE_BYTES),
                                      sysexe, &dclexe));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "DCL.EXE", 1, dclexe));

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
#undef STEP
}

int main(void)
{
    printf("=== test_ods2_sysdisk_ensure: cross-process lazy registration ===\n");

    const char *DCLEXE = "/vms/SYS0/SYSCOMMON/SYSEXE/DCL.EXE";
    uint8_t buf[4096];
    size_t got = 0;
    int st;

    /* This process NEVER calls vmsfs_volume_register: the adapter must. */

    /* --- FAIL-HONEST: no OVMX_SYSDISK_DEV channel, nothing registered --- */
    unsetenv("OVMX_SYSDISK_DEV");
    CHECK(vmsfs_volume_count() == 0, "no volume registered at start");
    st = ods2_sysdisk_read_file(DCLEXE, buf, sizeof(buf), &got);
    CHECK(st == SS$_DEVNOTMOUNT, "no channel -> SS$_DEVNOTMOUNT (fail-honest)");
    CHECK(vmsfs_volume_count() == 0, "no channel -> table stays empty");

    /* --- FAIL-HONEST: channel names a NON-ODS-2 file --- */
    char junk_path[] = "/tmp/ods2_ensure_junkXXXXXX";
    int jfd = mkstemp(junk_path);
    CHECK(jfd >= 0, "created a non-ODS-2 scratch file");
    if (jfd >= 0) {
        (void)!write(jfd, "not an ODS-2 volume\n", 20);
        close(jfd);
    }
    setenv("OVMX_SYSDISK_DEV", junk_path, 1);
    st = ods2_sysdisk_read_file(DCLEXE, buf, sizeof(buf), &got);
    CHECK(st == SS$_DEVNOTMOUNT, "bad channel -> SS$_DEVNOTMOUNT (fail-honest)");
    CHECK(vmsfs_volume_count() == 0, "bad channel -> table stays empty");
    unlink(junk_path);

    /* --- LAZY REGISTER: channel names a genuine ODS-2 volume --- */
    char img_path[] = "/tmp/ods2_ensure_volXXXXXX";
    CHECK(build_ods2_image(img_path) == 0, "built a genuine ODS-2 volume image");
    setenv("OVMX_SYSDISK_DEV", img_path, 1);

    got = 0;
    st = ods2_sysdisk_read_file(DCLEXE, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL, "genuine channel -> read succeeds (lazy register)");
    CHECK(vmsfs_volume_count() == 1, "adapter lazily registered DKA0: (count==1)");
    CHECK(got == sizeof(IMAGE_BYTES) &&
          memcmp(buf, IMAGE_BYTES, sizeof(IMAGE_BYTES)) == 0,
          "verbatim image read back BYTE-IDENTICAL over the lazy handle");

    /* --- IDEMPOTENT: a second read reuses the cached handle --- */
    got = 0;
    st = ods2_sysdisk_read_file(DCLEXE, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL, "second read still succeeds (cached handle)");
    CHECK(vmsfs_volume_count() == 1, "no duplicate registration (count stays 1)");

    vmsfs_volume_unregister("DKA0");
    unlink(img_path);

    if (g_failures == 0) {
        printf("=== ALL CHECKS PASSED ===\n");
        return 0;
    }
    printf("=== %d CHECK(S) FAILED ===\n", g_failures);
    return 1;
}
