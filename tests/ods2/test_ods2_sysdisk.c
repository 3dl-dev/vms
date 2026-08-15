/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_sysdisk.c - proof for the SYS$DISK read adapter
 * (src/vmsfs/ods2_sysdisk.c), the "/vms/..." -> genuine-ODS-2 path bridge of
 * the ODS-2 runtime flip (epic vms-5eb, docs/design-ods2-runtime-flip.md).
 *
 * WHAT THIS PROVES, AND WHY. The atomic RMS/DCL/MOUNT flip (rungs R2/R3/R5)
 * reroutes every SYS$DISK access off the POSIX /vms passthrough onto the
 * registered block-device volume handle. The ONE bridge all three consumers
 * share is: take the "/vms/A/B/.../NAME.EXT;ver" Linux path the caller has
 * already resolved via vmsfs_to_linux_path, and turn it into an ods2_bdev
 * directory walk + file resolve/read over the cached handle. This test drives
 * that bridge exactly as RMS rms_impl_open's open(2) site and DCL
 * cmd_directory's opendir(2) site will call it:
 *
 *   - build a genuine ODS-2 volume carrying the real system-disk hierarchy
 *     ([000000]->[SYS0]->[SYS0.SYSCOMMON]->[SYS0.SYSCOMMON.SYSEXE]) with a
 *     VERBATIM binary image (create_file_raw, the R6-master content policy) and
 *     a VAR text file, lay it on a real loop image fd;
 *   - register it as DKA0: (SYSDISK_DEVICE), exactly as PID 1 does at boot;
 *   - through the ADAPTER, keyed only off "/vms/..." Linux paths (never a POSIX
 *     open/opendir of the /vms tree), resolve + read the verbatim image back
 *     BYTE-IDENTICAL (the architecture-A1 read-path property), resolve the text
 *     file by explicit and highest version, and list the directory;
 *   - FAIL-HONEST (Rule 9 / INV-6): with NO volume registered a SYS$DISK read
 *     returns SS$_DEVNOTMOUNT (never a silent POSIX fallback); a non-/vms path
 *     returns SS$_BADPARAM ("not mine"); a missing file returns SS$_NOSUCHFILE;
 *     an undersized buffer returns SS$_DATACHECK.
 *
 * Every fact is verified against the ods2.h structs / the genuine reader via
 * the adapter, never via POSIX stat()/open()/opendir().
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

/* A "binary" image with embedded NULs and high bytes: re-framing this as VAR
 * text records (create_file()) WOULD corrupt it; create_file_raw() must read
 * back byte-identical -- the property IMGACT relies on for DCL.EXE et al. */
static const uint8_t IMAGE_BYTES[] = {
    0x7f, 'E', 'L', 'F', 0x00, 0x01, 0x02, 0x00, 0xff, 0xfe, 0x00, 'X',
    'V', 'M', 'S', '$', 0x00, 0x00, 0x10, 0x20, 0x30, 0xa5, 0x5a, 0x00
};
static const char *const LOGIN_TEXT =
    "$ SET NOON\n$ WRITE SYS$OUTPUT \"HI\"\n";

/*
 * Build a genuine ODS-2 volume with [SYS0.SYSCOMMON.SYSEXE] carrying a
 * verbatim image DCL.EXE;1 and a VAR text LOGIN.COM (;1 and ;2), and lay it
 * onto a fresh loop image at `path` (mkstemp template, filled in). Returns 0
 * on success. Mirrors the writer sequence the R6 boot-master emits.
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
    ods2_fid_t sys0, syscommon, sysexe, dclexe, login_v1;
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

    /* Verbatim binary image (the R6-master content policy). */
    STEP(ods2_wvolume_create_file_raw(&wvol, "DCL.EXE", 1,
                                      IMAGE_BYTES, sizeof(IMAGE_BYTES),
                                      sysexe, &dclexe));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "DCL.EXE", 1, dclexe));

    /* A VAR text file at version 1 (the writer holds one version per name). */
    STEP(ods2_wvolume_create_file(&wvol, "LOGIN.COM", 1,
                                  (const uint8_t *)LOGIN_TEXT, strlen(LOGIN_TEXT),
                                  sysexe, &login_v1));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "LOGIN.COM", 1, login_v1));

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

/* list_dir callback: count entries and note whether DCL.EXE / LOGIN.COM seen. */
struct list_ctx {
    int total;
    int saw_dclexe;
    int login_versions;   /* how many LOGIN.COM entries (versions) seen */
};

static int list_cb(const char *name, unsigned name_len, uint16_t version,
                   const ods2_fid_t *fid, void *ctx)
{
    (void)version; (void)fid;
    struct list_ctx *lc = (struct list_ctx *)ctx;
    lc->total++;
    if (name_len == 7 && strncmp(name, "DCL.EXE", 7) == 0)
        lc->saw_dclexe++;
    if (name_len == 9 && strncmp(name, "LOGIN.COM", 9) == 0)
        lc->login_versions++;
    return 0;
}

int main(void)
{
    printf("=== test_ods2_sysdisk: SYS$DISK read adapter ===\n");

    const char *SYSEXE = "/vms/SYS0/SYSCOMMON/SYSEXE";
    char dclexe_path[128], login_path[128], login_v1_path[128];
    snprintf(dclexe_path, sizeof(dclexe_path), "%s/DCL.EXE", SYSEXE);
    snprintf(login_path, sizeof(login_path), "%s/LOGIN.COM", SYSEXE);
    snprintf(login_v1_path, sizeof(login_v1_path), "%s/LOGIN.COM;1", SYSEXE);

    /* --- ods2_sysdisk_owns_path predicate (no volume needed) --- */
    CHECK(ods2_sysdisk_owns_path("/vms/SYS0/X.DAT") == 1, "owns_path(/vms/...) == 1");
    CHECK(ods2_sysdisk_owns_path("/vms") == 1, "owns_path(/vms) == 1");
    CHECK(ods2_sysdisk_owns_path("/vmsXYZ/X") == 0, "owns_path(/vmsXYZ) == 0 (boundary)");
    CHECK(ods2_sysdisk_owns_path("/tmp/X.DAT") == 0, "owns_path(/tmp/...) == 0");
    CHECK(ods2_sysdisk_owns_path(NULL) == 0, "owns_path(NULL) == 0");

    /* --- FAIL-HONEST: no volume registered --- */
    CHECK(vmsfs_volume_count() == 0, "no volume registered yet");
    uint8_t buf[4096];
    size_t got = 0;
    int st = ods2_sysdisk_read_file(dclexe_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_DEVNOTMOUNT,
          "read before mount -> SS$_DEVNOTMOUNT (no silent POSIX fallback)");
    st = ods2_sysdisk_list_dir(SYSEXE, list_cb, NULL);
    CHECK(st == SS$_DEVNOTMOUNT, "list before mount -> SS$_DEVNOTMOUNT");

    /* A non-SYS$DISK path is honestly "not mine", even before any mount. */
    st = ods2_sysdisk_read_file("/tmp/FOO.DAT", buf, sizeof(buf), &got);
    CHECK(st == SS$_BADPARAM, "read /tmp/... -> SS$_BADPARAM (caller keeps POSIX)");

    /* --- build + register the genuine ODS-2 SYS$DISK as DKA0: --- */
    char image_path[] = "/tmp/ods2_sysdisk_XXXXXX";
    if (build_ods2_image(image_path) != 0) {
        printf("  FAIL: could not build ODS-2 image\n");
        return 1;
    }
    ods2_status_t ost = ODS2_OK;
    int rst = vmsfs_volume_register("DKA0", image_path, &ost);
    CHECK(rst == SS$_NORMAL && ost == ODS2_OK, "register DKA0: -> SS$_NORMAL");

    /* --- verbatim image read-back BYTE-IDENTICAL (architecture A1) --- */
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(dclexe_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL, "read [SYSEXE]DCL.EXE via adapter -> SS$_NORMAL");
    CHECK(got == sizeof(IMAGE_BYTES), "DCL.EXE valid byte count exact");
    CHECK(got == sizeof(IMAGE_BYTES) &&
          memcmp(buf, IMAGE_BYTES, sizeof(IMAGE_BYTES)) == 0,
          "DCL.EXE bytes identical to source (verbatim, no VAR corruption)");

    /* --- version parsing routes to the right on-disk version --- */
    const ods2_bdev_t *bv = NULL;
    ods2_fid_t fid_high, fid_v1;
    uint8_t header[ODS2_BLOCK_SIZE];
    st = ods2_sysdisk_resolve_file(login_path, &bv, &fid_high,
                                   header, sizeof(header));
    CHECK(st == SS$_NORMAL && bv != NULL,
          "resolve [SYSEXE]LOGIN.COM (no ;ver -> highest) -> SS$_NORMAL");
    st = ods2_sysdisk_resolve_file(login_v1_path, NULL, &fid_v1,
                                   header, sizeof(header));
    CHECK(st == SS$_NORMAL, "resolve [SYSEXE]LOGIN.COM;1 (explicit) -> SS$_NORMAL");
    CHECK(ods2_fid_number(&fid_high) == ods2_fid_number(&fid_v1),
          "highest and ;1 resolve to the SAME FID (one version on disk)");
    /* An explicit version that is not present fails honestly. */
    st = ods2_sysdisk_read_file("/vms/SYS0/SYSCOMMON/SYSEXE/LOGIN.COM;9",
                                buf, sizeof(buf), &got);
    CHECK(st == SS$_NOSUCHFILE, "resolve LOGIN.COM;9 (absent version) -> SS$_NOSUCHFILE");

    /* --- missing file -> SS$_NOSUCHFILE --- */
    st = ods2_sysdisk_read_file("/vms/SYS0/SYSCOMMON/SYSEXE/NOPE.DAT",
                                buf, sizeof(buf), &got);
    CHECK(st == SS$_NOSUCHFILE, "read missing file -> SS$_NOSUCHFILE");
    st = ods2_sysdisk_list_dir("/vms/SYS0/NOSUCHDIR", list_cb, NULL);
    CHECK(st == SS$_NOSUCHFILE, "list missing dir -> SS$_NOSUCHFILE");

    /* --- undersized read buffer -> SS$_DATACHECK --- */
    st = ods2_sysdisk_read_file(dclexe_path, buf, 4, &got);
    CHECK(st == SS$_DATACHECK, "read into too-small buffer -> SS$_DATACHECK");

    /* --- directory listing via the adapter --- */
    struct list_ctx lc = { 0, 0, 0 };
    st = ods2_sysdisk_list_dir(SYSEXE, list_cb, &lc);
    CHECK(st == SS$_NORMAL, "list [SYSEXE] via adapter -> SS$_NORMAL");
    CHECK(lc.saw_dclexe == 1, "listing shows DCL.EXE");
    CHECK(lc.login_versions == 1, "listing shows LOGIN.COM");

    /* trailing slash tolerated */
    struct list_ctx lc2 = { 0, 0, 0 };
    st = ods2_sysdisk_list_dir("/vms/SYS0/SYSCOMMON/SYSEXE/", list_cb, &lc2);
    CHECK(st == SS$_NORMAL && lc2.total == lc.total,
          "trailing '/' lists the same directory");

    /* --- after unregister: fail-honest again --- */
    CHECK(vmsfs_volume_unregister("DKA0") == SS$_NORMAL, "unregister DKA0:");
    st = ods2_sysdisk_read_file(dclexe_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_DEVNOTMOUNT, "read after unmount -> SS$_DEVNOTMOUNT");

    unlink(image_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
