/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_mount_validate.c - proof for MOUNT volume validation
 * (src/vmsfs/ods2_sysdisk.c: ods2_sysdisk_validate_backing() /
 * ods2_sysdisk_validate_handle()), the R5-MOUNT rung of the ODS-2 runtime flip
 * (vms-6f5, epic vms-5eb, docs/design-ods2-runtime-flip.md).
 *
 * WHAT THIS PROVES, AND WHY. Under architecture A1, MOUNT stops mount(2)-ing an
 * arbitrary backing device as vmsfs and reporting success (the retired /vms
 * passthrough, which never proved the media was ODS-2). It instead PARSES the
 * candidate volume's genuine ODS-2 structures -- its HOME block (LBN 1:
 * DECFILE11B + both additive checksums + structure level) and its STORAGE
 * CONTROL BLOCK (SCB, BITMAP.SYS VBN 1) -- and ACCEPTS the volume only if both
 * validate, REJECTING non-ODS-2 media honestly. This test drives exactly the
 * two entry points cmd_mount consumes:
 *
 *   - ods2_sysdisk_validate_backing(): a genuine ODS-2 volume laid on a loop
 *     image validates SS$_NORMAL and REFLECTS the real on-disk fields (volume
 *     label from the home block, cluster factor + volume-size-in-blocks from
 *     the SCB); a NON-ODS-2 blob is REJECTED SS$_DEVNOTMOUNT (the honest MOUNT
 *     rejection -- never a false success); a NULL path is SS$_BADPARAM and an
 *     unopenable path is SS$_NOSUCHDEV.
 *   - ods2_sysdisk_validate_handle(): the SYS$DISK case. With NO volume
 *     registered (and OVMX_SYSDISK_DEV unset) it FAILS HONEST SS$_DEVNOTMOUNT
 *     (Rule 9 / INV-6, the atomic-group red-by-design edge); once the genuine
 *     volume is exported as the boot device and lazily registered, it
 *     validates SS$_NORMAL against the SAME real fields.
 *
 * Every fact is verified against the ods2.h structs via the validators, never
 * via POSIX stat()/open() of a mounted tree.
 */

#define _POSIX_C_SOURCE 200809L

#include "vmsfs/sysdisk.h"
#include "vmsfs/volume.h"
#include "vmsfs/ods2.h"
#include "ssdef.h"
#include "ovmx_layout.h"   /* SYSDISK_DEVICE ("DKA0") */

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

/*
 * Build a genuine ODS-2 volume (home block + reserved system files, including
 * BITMAP.SYS carrying the SCB at its VBN 1) and lay it onto a fresh loop image
 * at `path` (mkstemp template, filled in). A bare format is enough for
 * home/SCB validation -- no user files needed. Returns the volume size in
 * blocks on success (> 0), or 0 on failure.
 */
static uint32_t build_ods2_image(char *path)
{
    uint32_t total_blocks =
        (uint32_t)((uint64_t)VOL_MB * 1024 * 1024 / ODS2_BLOCK_SIZE);
    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = (uint8_t *)calloc(1, image_len);
    if (!image)
        return 0;

    ods2_format_params_t params = { total_blocks,
                                    total_blocks / 100 < ODS2_RESFILES
                                        ? ODS2_RESFILES : total_blocks / 100,
                                    VOL_LABEL };
    ods2_wvolume_t wvol;
    uint32_t rc = 0;

    if (ods2_volume_format(image, image_len, &params, &wvol) != ODS2_OK)
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
    rc = total_blocks;

out:
    free(image);
    return rc;
}

/* Write a non-ODS-2 blob (a valid file, wrong contents) to a fresh temp. */
static int build_junk_blob(char *path)
{
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    /* One MB of a repeating non-DECFILE11B pattern -- opens fine, home block
     * fails validation (no "DECFILE11B  " / bad checksums). */
    uint8_t chunk[ODS2_BLOCK_SIZE];
    for (size_t i = 0; i < sizeof(chunk); i++)
        chunk[i] = (uint8_t)(0xA5 ^ i);
    for (int b = 0; b < 2048; b++) {
        if (write(fd, chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk)) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

/* hm2_volname is a 12-byte space-padded field; compare its leading label. */
static int volname_is(const ods2_home_t *home, const char *want)
{
    size_t wl = strlen(want);
    if (wl > sizeof(home->hm2_volname))
        return 0;
    if (memcmp(home->hm2_volname, want, wl) != 0)
        return 0;
    for (size_t i = wl; i < sizeof(home->hm2_volname); i++)
        if (home->hm2_volname[i] != ' ')
            return 0;
    return 1;
}

int main(void)
{
    printf("=== test_ods2_mount_validate: MOUNT home+SCB validation ===\n");

    /* The SYS$DISK lazy-register channel must be OFF so the fail-honest edge
     * is a genuine "no volume", not an inherited environment. */
    unsetenv("OVMX_SYSDISK_DEV");

    ods2_home_t home;
    ods2_scb_t  scb;

    /* --- argument guards (no volume needed) --- */
    CHECK(ods2_sysdisk_validate_backing(NULL, &home, &scb) == SS$_BADPARAM,
          "validate_backing(NULL) -> SS$_BADPARAM");
    CHECK(ods2_sysdisk_validate_backing("/no/such/backing/device",
                                        &home, &scb) == SS$_NOSUCHDEV,
          "validate_backing(missing path) -> SS$_NOSUCHDEV");

    /* --- FAIL-HONEST SYS$DISK edge: no volume registered --- */
    CHECK(vmsfs_volume_count() == 0, "no SYS$DISK volume registered yet");
    CHECK(ods2_sysdisk_validate_handle(&home, &scb) == SS$_DEVNOTMOUNT,
          "validate_handle (no volume, OVMX_SYSDISK_DEV unset) -> SS$_DEVNOTMOUNT");

    /* --- NON-ODS-2 blob is REJECTED (the honest MOUNT rejection) --- */
    char junk_path[] = "/tmp/ods2_mount_junk_XXXXXX";
    if (build_junk_blob(junk_path) != 0) {
        printf("  FAIL: could not build junk blob\n");
        return 1;
    }
    memset(&home, 0, sizeof(home));
    CHECK(ods2_sysdisk_validate_backing(junk_path, &home, &scb) == SS$_DEVNOTMOUNT,
          "validate_backing(non-ODS-2 blob) -> SS$_DEVNOTMOUNT (no false success)");
    unlink(junk_path);

    /* --- genuine ODS-2 volume ACCEPTED + reflects real home/SCB fields --- */
    char image_path[] = "/tmp/ods2_mount_vol_XXXXXX";
    uint32_t total_blocks = build_ods2_image(image_path);
    if (total_blocks == 0) {
        printf("  FAIL: could not build ODS-2 image\n");
        return 1;
    }

    memset(&home, 0, sizeof(home));
    memset(&scb, 0, sizeof(scb));
    int st = ods2_sysdisk_validate_backing(image_path, &home, &scb);
    CHECK(st == SS$_NORMAL,
          "validate_backing(genuine ODS-2 volume) -> SS$_NORMAL");
    CHECK(volname_is(&home, VOL_LABEL),
          "home block reports the real volume label (" VOL_LABEL ")");
    CHECK(scb.scb_struclev == ODS2_STRUCLEV_V2,
          "SCB structure level is ODS-2 (0x0201)");
    CHECK(scb.scb_volsize == total_blocks,
          "SCB volume-size-in-blocks matches the built volume");
    CHECK(scb.scb_cluster == home.hm2_cluster,
          "SCB cluster factor agrees with the home block");

    /* Also validate with NULL out-params (accepted, no crash). */
    CHECK(ods2_sysdisk_validate_backing(image_path, NULL, NULL) == SS$_NORMAL,
          "validate_backing(genuine, NULL outs) -> SS$_NORMAL");

    /* --- SYS$DISK handle path once the volume IS the registered boot disk --- */
    setenv("OVMX_SYSDISK_DEV", image_path, 1);   /* PID 1's cross-process channel */
    memset(&home, 0, sizeof(home));
    memset(&scb, 0, sizeof(scb));
    st = ods2_sysdisk_validate_handle(&home, &scb);
    CHECK(st == SS$_NORMAL,
          "validate_handle (volume lazily registered from OVMX_SYSDISK_DEV) -> SS$_NORMAL");
    CHECK(volname_is(&home, VOL_LABEL),
          "handle path reports the SAME real volume label");
    CHECK(scb.scb_volsize == total_blocks,
          "handle path reports the SAME SCB volume size");

    vmsfs_volume_unregister(SYSDISK_DEVICE);
    unsetenv("OVMX_SYSDISK_DEV");
    unlink(image_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
