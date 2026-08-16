/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_write_broker.c - proof for the SYS$DISK ODS-2 WRITE-PATH
 * single-writer SERIALIZATION BROKER (vms-49d, epic vms-5eb,
 * docs/design-ods2-runtime-flip.md §5.1).
 *
 * WHAT THIS PROVES, AND WHY. The one SYS$DISK block device has no transaction
 * manager, yet at boot THREE independent processes write it concurrently
 * (PID 1 OPERATOR.LOG append, a login LASTLOGIN write, RMS $PUT). Each
 * ods2_sysdisk_* write entry point opens a per-call block-device-backed writer
 * (ods2_wvolume_open_bdev, which RECONSTRUCTS the free-block / free-FID
 * watermarks from the on-disk bitmaps), mutates, and flushes. Without a lock,
 * two such spans interleaving on the SAME device read the SAME watermark and
 * allocate the SAME LBN/FID, and two dir_insert read-modify-writes of one
 * parent directory block clobber each other -- torn blocks and lost entries.
 *
 * The broker (src/vmsfs/ods2_sysdisk.c sysdisk_wlock, flock(LOCK_EX) held
 * across the whole open->mutate->flush span) serializes this. flock is
 * inode-associated, so the REAL cross-process case is the one it defends:
 * this test therefore uses fork()'d CHILD PROCESSES, each REGISTERING SYS$DISK
 * for ITSELF (its OWN open file description on the device -- exactly what every
 * boot process does via sysdisk_handle()), then all released together by a
 * barrier so their write critical sections MAXIMALLY overlap. pthreads would
 * be insufficient: threads share one open file description, on which flock does
 * not contend, so they could not distinguish "broker works" from "broker
 * absent".
 *
 * Over several rounds, each round: the parent mkdir's a fresh subdirectory
 * (single-writer, no race), then forks NCHILD children that each CREATE one
 * distinct, byte-patterned file inside it. After all children finish, the
 * parent re-reads with the genuine block-backed reader (ods2_sysdisk_read_file
 * / ods2_bdev_*) and asserts: EVERY file present, EVERY file byte-exact, and
 * the directory lists exactly NCHILD entries -- no torn blocks, no lost inserts.
 *
 * ENFORCEMENT: this is the good-case pass; the accompanying manual check (see
 * the header comment in ods2_sysdisk.c and the PR body -- temporarily neuter
 * sysdisk_wlock() to a no-op, rebuild, and this test FAILS under the same
 * contention) proves the lock is what prevents the corruption. Fail-honest
 * (no volume -> SS$_DEVNOTMOUNT) is also asserted. Additive: reroutes no live
 * writer. Verified byte-exact against the genuine reader, never via POSIX I/O.
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
#include <sys/wait.h>

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
#define NCHILD     8      /* concurrent writers per round (>= the 3 boot writers) */
#define ROUNDS     4      /* independent barrier-released batches */
#define FILE_LEN   1500u  /* crosses the 512-byte ODS-2 block boundary */

static const char *SYSEXE = "/vms/SYS0/SYSCOMMON/SYSEXE";

/* Deterministic per-(round,child) content: a marker header + a byte pattern
 * keyed off round and child so a torn/overwritten block is detected exactly. */
static void fill_pattern(uint8_t *buf, size_t len, int round, int child)
{
    int m = snprintf((char *)buf, len, "OVMX-BROKER r%d c%d\n", round, child);
    if (m < 0)
        m = 0;
    for (size_t i = (size_t)m; i < len; i++)
        buf[i] = (uint8_t)((i * 31u + (unsigned)child * 7u
                            + (unsigned)round * 131u) & 0xFF);
}

/*
 * Build a genuine ODS-2 volume with [SYS0.SYSCOMMON.SYSEXE] and lay it onto a
 * fresh loop image at `path` (mkstemp template, filled in). Mirrors the R6
 * boot-master writer sequence. Returns 0 on success.
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
    ods2_fid_t sys0, syscommon, sysexe;
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

    int fd = mkstemp(path);
    if (fd < 0)
        goto out;
    if (ftruncate(fd, (off_t)image_len) != 0 ||
        pwrite(fd, image, image_len, 0) < 0) {
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

struct dir_count { int n; };

static int count_cb(const char *name, unsigned name_len, uint16_t version,
                    const ods2_fid_t *fid, void *ctx)
{
    (void)name; (void)name_len; (void)version; (void)fid;
    ((struct dir_count *)ctx)->n++;
    return 0;   /* keep listing */
}

int main(void)
{
    printf("=== test_ods2_write_broker: SYS$DISK write serialization (vms-49d) ===\n");

    /* --- fail-honest before any mount: no volume -> SS$_DEVNOTMOUNT --- */
    CHECK(vmsfs_volume_count() == 0, "no volume registered yet");
    int st0 = ods2_sysdisk_create_file(
        "/vms/SYS0/SYSCOMMON/SYSEXE/PRE.DAT", "x", 1);
    CHECK(st0 == SS$_DEVNOTMOUNT,
          "create before mount -> SS$_DEVNOTMOUNT (no silent POSIX fallback)");

    /* --- build + register the genuine ODS-2 SYS$DISK as DKA0: (O_RDWR) --- */
    char image_path[] = "/tmp/ods2_write_broker_XXXXXX";
    if (build_ods2_image(image_path) != 0) {
        printf("  FAIL: could not build ODS-2 image\n");
        return 1;
    }
    ods2_status_t ost = ODS2_OK;
    int rst = vmsfs_volume_register("DKA0", image_path, &ost);
    CHECK(rst == SS$_NORMAL && ost == ODS2_OK, "register DKA0: -> SS$_NORMAL");

    static uint8_t rdbuf[FILE_LEN * 2];
    static uint8_t expect[FILE_LEN];

    for (int r = 0; r < ROUNDS; r++) {
        char round_dir[64];
        snprintf(round_dir, sizeof(round_dir), "%s/R%d", SYSEXE, r);

        /* Parent, single-threaded: create this round's target directory. */
        int stm = ods2_sysdisk_mkdir(round_dir);
        CHECK(stm == SS$_NORMAL, "parent mkdir round dir -> SS$_NORMAL");

        /* Barrier pipe: children block on a read until the parent releases
         * them all at once, so their write critical sections overlap. */
        int bar[2];
        if (pipe(bar) != 0) {
            printf("  FAIL: pipe\n");
            g_failures++;
            break;
        }

        pid_t kids[NCHILD];
        for (int c = 0; c < NCHILD; c++) {
            pid_t pid = fork();
            if (pid < 0) {
                printf("  FAIL: fork\n");
                g_failures++;
                kids[c] = -1;
                continue;
            }
            if (pid == 0) {
                /* CHILD. Re-register SYS$DISK so THIS process owns its OWN fd
                 * (open file description) on the device -- the real
                 * cross-process case flock must serialize. (After fork the
                 * inherited fd is a SHARED open file description, on which
                 * flock would not contend.) */
                close(bar[1]);
                ods2_status_t cst = ODS2_OK;
                if (vmsfs_volume_register("DKA0", image_path, &cst)
                        != SS$_NORMAL || cst != ODS2_OK)
                    _exit(2);

                /* Wait at the barrier: block until the parent closes the write
                 * end (read returns 0). */
                char b;
                (void)!read(bar[0], &b, 1);

                uint8_t *mine = (uint8_t *)malloc(FILE_LEN);
                if (!mine)
                    _exit(3);
                fill_pattern(mine, FILE_LEN, r, c);

                char fpath[96];
                snprintf(fpath, sizeof(fpath), "%s/F%d.DAT", round_dir, c);
                int s = ods2_sysdisk_create_file(fpath, mine, FILE_LEN);
                free(mine);
                _exit(s == SS$_NORMAL ? 0 : 4);
            }
            kids[c] = pid;
        }

        /* PARENT: release all children simultaneously, then reap. */
        close(bar[0]);
        close(bar[1]);   /* read() in every child now returns 0 -> all go */

        int child_write_ok = 1;
        for (int c = 0; c < NCHILD; c++) {
            if (kids[c] < 0) { child_write_ok = 0; continue; }
            int wstatus = 0;
            waitpid(kids[c], &wstatus, 0);
            if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0)
                child_write_ok = 0;
        }
        CHECK(child_write_ok,
              "all concurrent child writers returned SS$_NORMAL");

        /* PARENT verifies: every file present + byte-exact via the reader. */
        int all_present = 1, all_exact = 1;
        for (int c = 0; c < NCHILD; c++) {
            char fpath[96];
            snprintf(fpath, sizeof(fpath), "%s/F%d.DAT", round_dir, c);
            memset(rdbuf, 0, sizeof(rdbuf));
            size_t got = 0;
            int s = ods2_sysdisk_read_file(fpath, rdbuf, sizeof(rdbuf), &got);
            if (s != SS$_NORMAL || got != FILE_LEN) {
                all_present = 0;
                continue;
            }
            fill_pattern(expect, FILE_LEN, r, c);
            if (memcmp(rdbuf, expect, FILE_LEN) != 0)
                all_exact = 0;
        }
        CHECK(all_present,
              "every concurrently-created file resolves + reads full length "
              "(no lost dir insert / no torn allocation)");
        CHECK(all_exact,
              "every concurrently-created file is BYTE-EXACT (no torn block)");

        /* Directory lists exactly NCHILD entries -- no insert was clobbered. */
        struct dir_count dc = { 0 };
        int sl = ods2_sysdisk_list_dir(round_dir, count_cb, &dc);
        CHECK(sl == SS$_NORMAL && dc.n == NCHILD,
              "round directory lists exactly NCHILD entries (no lost insert)");
    }

    /* --- fail-honest after unmount --- */
    CHECK(vmsfs_volume_unregister("DKA0") == SS$_NORMAL,
          "unregister DKA0:");
    int su = ods2_sysdisk_create_file(
        "/vms/SYS0/SYSCOMMON/SYSEXE/POST.DAT", "x", 1);
    CHECK(su == SS$_DEVNOTMOUNT, "create after unmount -> SS$_DEVNOTMOUNT");

    unlink(image_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
