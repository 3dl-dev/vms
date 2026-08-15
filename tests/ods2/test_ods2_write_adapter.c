/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_write_adapter.c - proof for the SYS$DISK ODS-2 WRITE adapter
 * (src/vmsfs/ods2_sysdisk.c create/append/mkdir + src/vmsfs/ods2/ods2_writer.c
 * ods2_wvolume_open_bdev/_append_file), the WRITE half of the ODS-2 runtime
 * flip (vms-02e, epic vms-5eb, docs/design-ods2-runtime-flip.md).
 *
 * WHAT THIS PROVES, AND WHY. The read adapter (test_ods2_sysdisk.c) reroutes a
 * "/vms/A/B/.../NAME.EXT;ver" SYS$DISK read onto the registered block-device
 * volume handle. This is its WRITE twin: the same "/vms/..." path bridge, but
 * driving genuine-ODS-2 WRITES over a LIVE (already-formatted, registered)
 * volume through a per-call block-device-backed writer opened over the
 * registered fd. It drives the adapter exactly as a runtime writer will:
 *
 *   - build a genuine ODS-2 volume carrying [000000]->[SYS0]->[SYSCOMMON]->
 *     [SYSEXE] with an initial verbatim OPERATOR.LOG, lay it on a real loop
 *     image, and register it as DKA0: (SYSDISK_DEVICE) as PID 1 does;
 *   - (1) CREATE a new verbatim file through the adapter and read it back
 *     BYTE-IDENTICAL;
 *   - (2) APPEND to an EXISTING file repeatedly (the novel path -- an
 *     OPERATOR.LOG-style repeated append, some appends forcing block/extent
 *     growth, including a NON-contiguous second extent) and read back the
 *     full pre+appended concatenation BYTE-EXACT;
 *   - (3) MKDIR a new directory, create a file inside it, and resolve it;
 *   - (4) FAIL-HONEST (Rule 9 / INV-6): no volume -> SS$_DEVNOTMOUNT (no
 *     silent POSIX fallback); non-/vms -> SS$_BADPARAM; append to an absent
 *     file -> SS$_NOSUCHFILE; and a write over a genuinely UNWRITABLE volume
 *     (an O_RDONLY fd) fails honestly (ODS2_ERR_IO), never a fake success.
 *
 * ADDITIVE: this exercises the adapter directly; it reroutes no live RMS/DCL/
 * boot writer (that is the atomic group). Verified byte-exact against the
 * genuine reader (ods2_sysdisk_read_file / ods2_bdev_*), never via POSIX I/O.
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

/* An initial verbatim OPERATOR.LOG body (binary-safe: embedded NUL + high
 * bytes), so raw-byte append correctness -- not just text -- is proven. */
static const uint8_t OPLOG_INIT[] = {
    '%', 'O', 'P', 'C', 'O', 'M', 0x00, 0x01, 0xff, 0x80, 'A', '\n'
};

/* A "binary" image with NULs/high bytes: verbatim create must round-trip. */
static const uint8_t STARTUP_BYTES[] = {
    0x7f, 'E', 'L', 'F', 0x00, 0x02, 0x01, 0x00, 0xde, 0xad, 0xbe, 0xef,
    'O', 'V', 'M', 'X', 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0xa5, 0x5a
};

static int g_image_len = 0;

/*
 * Build a genuine ODS-2 volume with [SYS0.SYSCOMMON.SYSEXE] carrying an
 * initial verbatim OPERATOR.LOG;1, and lay it onto a fresh loop image at
 * `path` (mkstemp template, filled in). Returns 0 on success. The WHOLE image
 * is written (not just the reserved head) so later allocations land in real
 * backing store. Mirrors the R6 boot-master writer sequence.
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
    ods2_fid_t sys0, syscommon, sysexe, oplog;
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

    /* Initial verbatim OPERATOR.LOG;1 (the append target). */
    STEP(ods2_wvolume_create_file_raw(&wvol, "OPERATOR.LOG", 1,
                                      OPLOG_INIT, sizeof(OPLOG_INIT),
                                      sysexe, &oplog));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "OPERATOR.LOG", 1, oplog));

    int fd = mkstemp(path);
    if (fd < 0)
        goto out;
    if (ftruncate(fd, (off_t)image_len) != 0 ||
        pwrite(fd, image, image_len, 0) < 0) {
        close(fd);
        goto out;
    }
    close(fd);
    g_image_len = (int)image_len;
    rc = 0;

out:
    free(image);
    return rc;
#undef STEP
}

int main(void)
{
    printf("=== test_ods2_write_adapter: SYS$DISK ODS-2 WRITE adapter ===\n");

    const char *SYSEXE = "/vms/SYS0/SYSCOMMON/SYSEXE";
    char oplog_path[128], startup_path[128];
    snprintf(oplog_path, sizeof(oplog_path), "%s/OPERATOR.LOG", SYSEXE);
    snprintf(startup_path, sizeof(startup_path), "%s/STARTUP.DAT", SYSEXE);

    static uint8_t buf[65536];
    size_t got = 0;
    int st;

    /* --- FAIL-HONEST before any mount: no volume / not mine --- */
    CHECK(vmsfs_volume_count() == 0, "no volume registered yet");
    st = ods2_sysdisk_create_file(startup_path, STARTUP_BYTES,
                                  sizeof(STARTUP_BYTES));
    CHECK(st == SS$_DEVNOTMOUNT,
          "create before mount -> SS$_DEVNOTMOUNT (no silent POSIX fallback)");
    st = ods2_sysdisk_append_file(oplog_path, "X", 1);
    CHECK(st == SS$_DEVNOTMOUNT, "append before mount -> SS$_DEVNOTMOUNT");
    st = ods2_sysdisk_mkdir("/vms/SYS0/SYSCOMMON/SYSMGR");
    CHECK(st == SS$_DEVNOTMOUNT, "mkdir before mount -> SS$_DEVNOTMOUNT");
    st = ods2_sysdisk_create_file("/tmp/FOO.DAT", "x", 1);
    CHECK(st == SS$_BADPARAM, "create /tmp/... -> SS$_BADPARAM (not mine)");

    /* --- build + register the genuine ODS-2 SYS$DISK as DKA0: (O_RDWR) --- */
    char image_path[] = "/tmp/ods2_write_adapter_XXXXXX";
    if (build_ods2_image(image_path) != 0) {
        printf("  FAIL: could not build ODS-2 image\n");
        return 1;
    }
    ods2_status_t ost = ODS2_OK;
    int rst = vmsfs_volume_register("DKA0", image_path, &ost);
    CHECK(rst == SS$_NORMAL && ost == ODS2_OK, "register DKA0: -> SS$_NORMAL");

    /* --- (1) CREATE a new verbatim file, read back BYTE-IDENTICAL --- */
    st = ods2_sysdisk_create_file(startup_path, STARTUP_BYTES,
                                  sizeof(STARTUP_BYTES));
    CHECK(st == SS$_NORMAL, "create [SYSEXE]STARTUP.DAT via adapter -> SS$_NORMAL");
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(startup_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL && got == sizeof(STARTUP_BYTES) &&
          memcmp(buf, STARTUP_BYTES, sizeof(STARTUP_BYTES)) == 0,
          "STARTUP.DAT reads back byte-identical (live verbatim create)");

    /* Re-creating an existing name is refused honestly (dir dup). */
    st = ods2_sysdisk_create_file(startup_path, STARTUP_BYTES,
                                  sizeof(STARTUP_BYTES));
    CHECK(st != SS$_NORMAL, "re-create existing name -> honest non-success");

    /* --- (2) APPEND to an EXISTING file, read back concatenation byte-exact.
     * Build the expected concatenation as we go. Interleave a create so a
     * later append lands a NON-contiguous second extent (multi-extent read).
     * The chunk sizes deliberately cross 512-byte block boundaries. --- */
    static uint8_t expect[65536];
    size_t explen = 0;
    memcpy(expect, OPLOG_INIT, sizeof(OPLOG_INIT));
    explen = sizeof(OPLOG_INIT);

    /* append A: small, stays within the first block */
    static uint8_t chunkA[80];
    for (size_t i = 0; i < sizeof(chunkA); i++) chunkA[i] = (uint8_t)(0x40 + (i % 26));
    st = ods2_sysdisk_append_file(oplog_path, chunkA, sizeof(chunkA));
    CHECK(st == SS$_NORMAL, "append A (in-block) -> SS$_NORMAL");
    memcpy(expect + explen, chunkA, sizeof(chunkA)); explen += sizeof(chunkA);

    /* append B: large, forces allocation of a new block (contiguous merge,
     * OPERATOR.LOG is still the most-recently-allocated file here) */
    static uint8_t chunkB[600];
    for (size_t i = 0; i < sizeof(chunkB); i++) chunkB[i] = (uint8_t)(i * 7 + 1);
    st = ods2_sysdisk_append_file(oplog_path, chunkB, sizeof(chunkB));
    CHECK(st == SS$_NORMAL, "append B (spills to a new block, contiguous) -> SS$_NORMAL");
    memcpy(expect + explen, chunkB, sizeof(chunkB)); explen += sizeof(chunkB);

    /* create another file to advance the bump watermark, so the NEXT
     * OPERATOR.LOG growth cannot be physically contiguous -> new extent */
    static const uint8_t filler[300] = { 0xAB };
    char filler_path[128];
    snprintf(filler_path, sizeof(filler_path), "%s/FILLER.DAT", SYSEXE);
    st = ods2_sysdisk_create_file(filler_path, filler, sizeof(filler));
    CHECK(st == SS$_NORMAL, "create FILLER.DAT (advances watermark) -> SS$_NORMAL");

    /* append C: forces further growth -> a NON-contiguous 2nd data extent */
    static uint8_t chunkC[1100];
    for (size_t i = 0; i < sizeof(chunkC); i++) chunkC[i] = (uint8_t)(0xF0 ^ (i & 0xFF));
    st = ods2_sysdisk_append_file(oplog_path, chunkC, sizeof(chunkC));
    CHECK(st == SS$_NORMAL, "append C (grows across a non-contiguous extent) -> SS$_NORMAL");
    memcpy(expect + explen, chunkC, sizeof(chunkC)); explen += sizeof(chunkC);

    /* Read the whole OPERATOR.LOG back and compare byte-for-byte. */
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(oplog_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL, "read OPERATOR.LOG back -> SS$_NORMAL");
    CHECK(got == explen, "OPERATOR.LOG valid byte count == pre+appended total");
    CHECK(got == explen && memcmp(buf, expect, explen) == 0,
          "OPERATOR.LOG reads back the FULL concatenation BYTE-EXACT (append round-trip)");

    /* FILLER.DAT is undisturbed by the interleaved OPERATOR.LOG growth. */
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(filler_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL && got == sizeof(filler) &&
          memcmp(buf, filler, sizeof(filler)) == 0,
          "FILLER.DAT unaffected by OPERATOR.LOG's extent growth");

    /* Append to a file CREATED via the adapter this session (STARTUP.DAT). */
    static const uint8_t tail[40] = { 0x11, 0x22, 0x33 };
    st = ods2_sysdisk_append_file(startup_path, tail, sizeof(tail));
    CHECK(st == SS$_NORMAL, "append to adapter-created STARTUP.DAT -> SS$_NORMAL");
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(startup_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL && got == sizeof(STARTUP_BYTES) + sizeof(tail) &&
          memcmp(buf, STARTUP_BYTES, sizeof(STARTUP_BYTES)) == 0 &&
          memcmp(buf + sizeof(STARTUP_BYTES), tail, sizeof(tail)) == 0,
          "STARTUP.DAT create+append round-trips byte-exact");

    /* --- append to an ABSENT file fails honestly --- */
    st = ods2_sysdisk_append_file("/vms/SYS0/SYSCOMMON/SYSEXE/NOPE.DAT",
                                  "x", 1);
    CHECK(st == SS$_NOSUCHFILE, "append to absent file -> SS$_NOSUCHFILE");

    /* --- (3) MKDIR a new directory, create a file inside, resolve it --- */
    st = ods2_sysdisk_mkdir("/vms/SYS0/SYSCOMMON/SYSMGR");
    CHECK(st == SS$_NORMAL, "mkdir [SYSCOMMON]SYSMGR via adapter -> SS$_NORMAL");
    const char *sysmgr_file = "/vms/SYS0/SYSCOMMON/SYSMGR/WELCOME.TXT";
    static const uint8_t welcome[] = "OVMX SYS$DISK is now writable.\n";
    st = ods2_sysdisk_create_file(sysmgr_file, welcome, sizeof(welcome));
    CHECK(st == SS$_NORMAL, "create file inside the new SYSMGR dir -> SS$_NORMAL");
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(sysmgr_file, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL && got == sizeof(welcome) &&
          memcmp(buf, welcome, sizeof(welcome)) == 0,
          "file in the freshly mkdir'd directory resolves + reads byte-exact");

    /* --- (4) FAIL-HONEST: write over a genuinely UNWRITABLE (O_RDONLY) volume
     * fails honestly at the writer primitive -- deterministic regardless of
     * privilege (pwrite on an O_RDONLY fd is EBADF), never a fake success. --- */
    {
        int rofd = open(image_path, O_RDONLY);
        CHECK(rofd >= 0, "reopen image O_RDONLY for the unwritable-volume proof");
        if (rofd >= 0) {
            ods2_wvolume_t rowv;
            ods2_status_t ws = ods2_wvolume_open_bdev(rofd, 0, &rowv);
            CHECK(ws == ODS2_OK, "open_bdev over O_RDONLY fd (reads succeed)");
            if (ws == ODS2_OK) {
                /* OPERATOR.LOG is FID... resolve it via the read adapter to get
                 * a real FID, then attempt an append over the O_RDONLY wvol. */
                const ods2_bdev_t *robv = vmsfs_volume_handle("DKA0");
                ods2_fid_t oplog_fid;
                uint8_t fhdr[ODS2_BLOCK_SIZE];
                const char *comps[3] = { "SYS0", "SYSCOMMON", "SYSEXE" };
                ods2_status_t rs = ods2_bdev_resolve_file(robv, comps, 3,
                                                          "OPERATOR.LOG", 0,
                                                          &oplog_fid, fhdr,
                                                          sizeof(fhdr));
                CHECK(rs == ODS2_OK, "resolve OPERATOR.LOG FID for the RO-append proof");
                ws = ods2_wvolume_append_file(&rowv, oplog_fid, "Z", 1);
                CHECK(ws != ODS2_OK,
                      "append over O_RDONLY volume fails honestly (no fake success)");
                ods2_wvolume_close(&rowv);
            }
            close(rofd);
        }
    }

    /* The read-only append attempt must NOT have corrupted the volume: the
     * O_RDWR-registered OPERATOR.LOG still reads back the same concatenation. */
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(oplog_path, buf, sizeof(buf), &got);
    CHECK(st == SS$_NORMAL && got == explen && memcmp(buf, expect, explen) == 0,
          "OPERATOR.LOG still byte-exact after the failed RO append (no corruption)");

    /* --- after unregister: fail-honest again --- */
    CHECK(vmsfs_volume_unregister("DKA0") == SS$_NORMAL, "unregister DKA0:");
    st = ods2_sysdisk_append_file(oplog_path, "x", 1);
    CHECK(st == SS$_DEVNOTMOUNT, "append after unmount -> SS$_DEVNOTMOUNT");

    unlink(image_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
