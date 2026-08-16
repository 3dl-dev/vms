/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_rms_sysdisk_workcopy.c - proof for the RMS SYS$DISK ODS-2 WORKING-COPY
 * model (epic vms-5eb, R2; docs/design-ods2-runtime-flip.md §0, RE-PLAN #1
 * route (a)). Drives the real RMS API (sys$open/$create/$connect/$get/$put/
 * $close) against files on a GENUINE-ODS-2 SYS$DISK volume registered as DKA0:,
 * proving the reroute off the POSIX /vms passthrough onto the ODS-2 adapters
 * WITHOUT any change to the seq/rel/idx positioned-I/O engines.
 *
 * WHAT THIS PROVES (the R2 unit-level done-condition):
 *   (1) $OPEN of a SYS$DISK file checks the whole ODS-2 file out into a memfd
 *       whose bytes are EXACTLY the file's; RMS sequential $GET reads records
 *       byte-identical to reading the same bytes via a POSIX file.
 *   (2) a write-open + $PUT + $CLOSE writes the working copy back as a NEW
 *       ODS-2 version whose content (read via the genuine reader) equals what
 *       was written; the prior version survives. Two $OPEN(write)->modify->
 *       $CLOSE cycles over an EXISTING file mint ;2 then ;3 (multiversion
 *       dir_insert, vms-9794/#613): each byte-exact, all versions coexist.
 *   (3) a read-only $OPEN + $CLOSE leaves the volume UNCHANGED -- no spurious
 *       new version.
 *   (4) ods2_sysdisk_owns_path() routes SYS$DISK ("/vms/...") vs non-SYS$DISK.
 *   FAIL-HONEST (Rule 9 / INV-6): with no ODS-2 SYS$DISK registered, $OPEN of a
 *   "/vms/..." file returns an honest error and NEVER falls back to a POSIX
 *   open (fab->_linux_fd stays -1).
 *
 * Every SYS$DISK fact is verified through the genuine ODS-2 adapters over the
 * registered block-device volume, never via POSIX stat()/open()/opendir().
 */

#define _POSIX_C_SOURCE 200809L

#include "rms/rms.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "vmsfs/sysdisk.h"
#include "vmsfs/volume.h"
#include "vmsfs/ods2.h"
#include "ovmx_layout.h"   /* SYSDISK_DEVICE ("DKA0"), SYSDISK_MOUNT ("/vms") */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/*
 * rms_parse.c references vmsfs_resolve(); provide the same test stub the RMS
 * unit test uses so this binary links without pulling the full resolver.
 */
int vmsfs_resolve(const char *spec, const char *default_spec,
                  char *result, size_t resultlen)
{
    (void)spec; (void)default_spec; (void)result; (void)resultlen;
    return -1;
}

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

/* A SYS$DISK file addressed exactly as an RMS consumer would after
 * vmsfs_to_linux_path: "/vms" + the SYS0/SYSCOMMON/SYSEXE directory chain. */
#define SEQ_VMS_PATH  SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE/TESTSEQ.DAT"

/* The verbatim ODS-2 file content = a Stream-LF record stream (three records).
 * Reading it back through RMS STMLF $GET must yield these three records. */
static const char SEQ_CONTENT[] =
    "FIRST RECORD ON SYS$DISK\n"
    "SECOND RECORD WITH \"QUOTES\" AND SYMBOLS !@#\n"
    "THIRD AND FINAL RECORD\n";

static const char *const SEQ_RECS[] = {
    "FIRST RECORD ON SYS$DISK",
    "SECOND RECORD WITH \"QUOTES\" AND SYMBOLS !@#",
    "THIRD AND FINAL RECORD",
};
#define SEQ_NRECS  3

/*
 * Build a genuine ODS-2 volume carrying [SYS0.SYSCOMMON.SYSEXE]TESTSEQ.DAT;1
 * (SEQ_CONTENT, verbatim) onto a fresh loop image at `path` (mkstemp template).
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
    ods2_fid_t sys0, syscommon, sysexe, seqfid;
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

    STEP(ods2_wvolume_create_file_raw(&wvol, "TESTSEQ.DAT", 1,
                                      (const uint8_t *)SEQ_CONTENT,
                                      sizeof(SEQ_CONTENT) - 1, sysexe, &seqfid));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "TESTSEQ.DAT", 1, seqfid));

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

/* Count how many versions of a given filename exist in
 * [SYS0.SYSCOMMON.SYSEXE] on the registered SYS$DISK volume. */
struct vercount { const char *name; unsigned n; };
static int vercount_cb(const char *name, unsigned name_len, uint16_t version,
                       const ods2_fid_t *fid, void *ctx)
{
    (void)version; (void)fid;
    struct vercount *c = (struct vercount *)ctx;
    if (name_len == strlen(c->name) &&
        strncmp(name, c->name, name_len) == 0)
        c->n++;
    return 0;
}
static unsigned count_named_versions(const char *filename)
{
    struct vercount c = { filename, 0 };
    (void)ods2_sysdisk_list_dir(SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE",
                                vercount_cb, &c);
    return c.n;
}
static unsigned count_versions(void) { return count_named_versions("TESTSEQ.DAT"); }

/* Read every STMLF record from an open+connected RAB into `out` (concatenated
 * with '\n' terminators restored) and return the record count. */
static int drain_records(struct RAB *rab, char *out, size_t out_cap)
{
    char rbuf[512];
    size_t used = 0;
    int nrecs = 0;
    for (;;) {
        rab->rab$l_ubf = rbuf;
        rab->rab$w_usz = (uint16_t)sizeof(rbuf);
        uint32_t st = sys$get(rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (st != RMS$_NORMAL) {
            printf("  (drain: sys$get returned 0x%X)\n", st);
            return -1;
        }
        size_t rsz = rab->rab$w_rsz;
        if (used + rsz + 1 >= out_cap)
            return -1;
        memcpy(out + used, rbuf, rsz);
        used += rsz;
        out[used++] = '\n';
        nrecs++;
    }
    out[used] = '\0';
    return nrecs;
}

int main(void)
{
    printf("=== test_rms_sysdisk_workcopy: RMS SYS$DISK ODS-2 working copy ===\n");

    /* ---- (4) owns_path routing ---- */
    CHECK(ods2_sysdisk_owns_path(SEQ_VMS_PATH) == 1,
          "owns_path routes a /vms SYS$DISK file to the ODS-2 adapter");
    CHECK(ods2_sysdisk_owns_path("/tmp/not_on_sysdisk.dat") == 0,
          "owns_path leaves a non-/vms path to the POSIX caller");

    /* ---- FAIL-HONEST: no ODS-2 SYS$DISK registered ---- */
    unsetenv("OVMX_SYSDISK_DEV");
    {
        struct FAB fab = cc$rms_fab;
        char spec[] = SEQ_VMS_PATH;
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_fac = FAB$M_GET;
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_org = FAB$C_SEQ;
        uint32_t st = sys$open(&fab, 0, 0);
        CHECK(!(st & 1u), "no ODS-2 SYS$DISK -> $OPEN fails (honest, not success)");
        CHECK(fab._linux_fd == -1,
              "no ODS-2 SYS$DISK -> NO POSIX fallback fd (INV-6)");
    }

    /* ---- Build + register a genuine ODS-2 SYS$DISK as DKA0: ---- */
    char img_path[] = "/tmp/rms_workcopy_volXXXXXX";
    CHECK(build_ods2_image(img_path) == 0, "built a genuine ODS-2 SYS$DISK image");
    /* Register O_RDWR so the checkin write-back can reach the medium. */
    ods2_status_t ost = ODS2_OK;
    int rst = vmsfs_volume_register(SYSDISK_DEVICE, img_path, &ost);
    CHECK(rst == SS$_NORMAL, "registered the ODS-2 volume as DKA0:");
    setenv("OVMX_SYSDISK_DEV", img_path, 1);

    /* ---- (1) $OPEN checks out into a memfd; $GET is byte-identical to POSIX ---- */
    char sysdisk_records[2048] = "";
    int sysdisk_n = -1;
    {
        struct FAB fab = cc$rms_fab;
        char spec[] = SEQ_VMS_PATH;
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_fac = FAB$M_GET;
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_org = FAB$C_SEQ;

        uint32_t st = sys$open(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$OPEN SYS$DISK file (working-copy checkout)");
        CHECK(fab._sysdisk_workcopy == 1, "FAB marked as a SYS$DISK working copy");
        CHECK(fab._linux_fd >= 0, "working-copy memfd installed as _linux_fd");

        /* The memfd holds EXACTLY the ODS-2 file bytes. */
        char mem[2048];
        ssize_t got = pread(fab._linux_fd, mem, sizeof(mem), 0);
        CHECK(got == (ssize_t)(sizeof(SEQ_CONTENT) - 1) &&
              memcmp(mem, SEQ_CONTENT, sizeof(SEQ_CONTENT) - 1) == 0,
              "memfd bytes are byte-identical to the ODS-2 file content");

        struct RAB rab = cc$rms_rab;
        rab.rab$l_fab = &fab;
        st = sys$connect(&rab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$CONNECT the SYS$DISK working copy");
        sysdisk_n = drain_records(&rab, sysdisk_records, sizeof(sysdisk_records));
        CHECK(sysdisk_n == SEQ_NRECS, "read all records from the SYS$DISK file");
        sys$disconnect(&rab, 0, 0);
        st = sys$close(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "read-only $CLOSE (no checkin)");
    }

    /* Reference: the SAME bytes in a POSIX file, read via RMS the POSIX way. */
    char posix_records[2048] = "";
    int posix_n = -1;
    {
        char pfile[] = "/tmp/rms_workcopy_refXXXXXX";
        int pfd = mkstemp(pfile);
        CHECK(pfd >= 0, "created a POSIX reference file");
        (void)!write(pfd, SEQ_CONTENT, sizeof(SEQ_CONTENT) - 1);
        close(pfd);

        struct FAB fab = cc$rms_fab;
        fab.fab$l_fna = pfile;
        fab.fab$b_fns = (uint8_t)strlen(pfile);
        fab.fab$b_fac = FAB$M_GET;
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_org = FAB$C_SEQ;
        uint32_t st = sys$open(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$OPEN the POSIX reference file");
        CHECK(fab._sysdisk_workcopy == 0, "POSIX file is NOT a working copy");

        struct RAB rab = cc$rms_rab;
        rab.rab$l_fab = &fab;
        sys$connect(&rab, 0, 0);
        posix_n = drain_records(&rab, posix_records, sizeof(posix_records));
        sys$disconnect(&rab, 0, 0);
        sys$close(&fab, 0, 0);
        unlink(pfile);
    }
    CHECK(sysdisk_n == posix_n && sysdisk_n == SEQ_NRECS &&
          strcmp(sysdisk_records, posix_records) == 0,
          "SYS$DISK $GET stream is byte-identical to the POSIX $GET stream");

    /* Also match against the literal expected records. */
    {
        int ok = (sysdisk_n == SEQ_NRECS);
        char expect[2048] = "";
        size_t u = 0;
        for (int i = 0; i < SEQ_NRECS; i++)
            u += (size_t)snprintf(expect + u, sizeof(expect) - u, "%s\n",
                                  SEQ_RECS[i]);
        ok = ok && strcmp(sysdisk_records, expect) == 0;
        CHECK(ok, "SYS$DISK records match the expected record set exactly");
    }

    CHECK(count_versions() == 1, "one version of TESTSEQ.DAT after read-only use");

    /* ---- (2) write-open + $PUT + $CLOSE checks the working copy back in as a
     * genuine ODS-2 file. We create a NEW filename here (NEWFILE.DAT), whose
     * checkin mints version ;1 -- the write-back path proven end to end: RMS
     * $CREATE -> memfd -> $PUT -> $CLOSE -> a real Files-11 file on the volume
     * whose content, read via the genuine reader, equals exactly what $PUT
     * wrote.
     *
     * Minting a NEW VERSION of an EXISTING name (TESTSEQ.DAT;2/;3) is exercised
     * below in section (2b): the ODS-2 writer's dir_insert now merges multiple
     * {version,FID} value entries into one directory record (vms-9794 / #613),
     * so a dirty writable $CLOSE mints the next-higher version while every prior
     * version survives -- real VMS file-versioning semantics. */
    const char *NEWFILE_VMS = SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE/NEWFILE.DAT";
    static const char *const NEWRECS[] = {
        "FRESHLY WRITTEN LINE ALPHA",
        "FRESHLY WRITTEN LINE BRAVO",
        "FRESHLY WRITTEN LINE CHARLIE",
        "FRESHLY WRITTEN LINE DELTA",
    };
    const int NEW_N = 4;
    {
        struct FAB fab = cc$rms_fab;
        char spec[] = SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE/NEWFILE.DAT";
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_org = FAB$C_SEQ;

        uint32_t st = sys$create(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$CREATE a new SYS$DISK file (empty checkout)");
        CHECK(fab._sysdisk_workcopy == 1 && fab._sysdisk_created == 1,
              "created working copy is marked for checkin");

        struct RAB rab = cc$rms_rab;
        rab.rab$l_fab = &fab;
        st = sys$connect(&rab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$CONNECT the created working copy");
        for (int i = 0; i < NEW_N; i++) {
            rab.rab$l_rbf = (char *)NEWRECS[i];
            rab.rab$w_rsz = (uint16_t)strlen(NEWRECS[i]);
            st = sys$put(&rab, 0, 0);
            if (st != RMS$_NORMAL) { CHECK(0, "sys$put new record"); break; }
        }
        sys$disconnect(&rab, 0, 0);
        st = sys$close(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$CLOSE writes the working copy back (checkin)");
    }

    /* The checked-in file's content (via the genuine reader) equals what $PUT
     * wrote -- a real ODS-2 file, not the memfd. */
    {
        char expect[2048] = "";
        size_t u = 0;
        for (int i = 0; i < NEW_N; i++)
            u += (size_t)snprintf(expect + u, sizeof(expect) - u, "%s\n",
                                  NEWRECS[i]);

        uint8_t rback[2048];
        size_t rlen = 0;
        int vst = ods2_sysdisk_read_file(NEWFILE_VMS, rback, sizeof(rback),
                                         &rlen);
        CHECK(vst == SS$_NORMAL, "genuine reader reads the checked-in file");
        CHECK(rlen == u && memcmp(rback, expect, u) == 0,
              "checked-in ODS-2 file content == what RMS $PUT wrote");
    }

    /* ---- (2b) NEW VERSION OF AN EXISTING NAME -- the multiversion checkin.
     * The done-condition for vms-af7a: $OPEN(write) -> modify -> $CLOSE of an
     * EXISTING file mints the NEXT-HIGHER ODS-2 version while every prior
     * version survives (real VMS file-versioning). The writer's dir_insert now
     * merges multiple {version,FID} value entries per directory record
     * (vms-9794 / #613), so this path -- previously a substrate gap -- works.
     *
     * $OPEN(write) checks the HIGHEST existing version out into the memfd
     * (byte-for-byte); $PUT appends a record (rms_seq_put seeks to EOF), so the
     * working copy is now (prior content + the appended record) -- dirty; the
     * checkin lists the directory, finds the current max version, and mints
     * max+1 via ods2_sysdisk_create_file(".../TESTSEQ.DAT;N+1"). Doing it twice
     * mints ;2 then ;3, each based on the then-latest content. Every version is
     * read back byte-exact through the genuine ODS-2 reader and all coexist. */
    const char *TESTSEQ_APPEND[] = {
        "APPENDED LINE FOR VERSION TWO",
        "APPENDED LINE FOR VERSION THREE",
    };
    /* Expected byte content of each minted version, built incrementally: ;2 is
     * SEQ_CONTENT + append[0]; ;3 is (;2 content) + append[1]. */
    char expect_v2[2048], expect_v3[2048];
    {
        int n = snprintf(expect_v2, sizeof(expect_v2), "%s%s\n",
                         SEQ_CONTENT, TESTSEQ_APPEND[0]);
        (void)n;
        snprintf(expect_v3, sizeof(expect_v3), "%s%s\n",
                 expect_v2, TESTSEQ_APPEND[1]);
    }

    for (int pass = 0; pass < 2; pass++) {
        const unsigned want_ver = (unsigned)pass + 2;   /* ;2 then ;3 */
        struct FAB fab = cc$rms_fab;
        char spec[] = SEQ_VMS_PATH;
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;   /* write-open of existing file */
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_org = FAB$C_SEQ;

        uint32_t st = sys$open(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$OPEN(write) existing SYS$DISK file (checkout latest)");
        CHECK(fab._sysdisk_workcopy == 1 && fab._sysdisk_writable == 1,
              "write-open is marked a checkin candidate");

        struct RAB rab = cc$rms_rab;
        rab.rab$l_fab = &fab;
        st = sys$connect(&rab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$CONNECT the write working copy");
        rab.rab$l_rbf = (char *)TESTSEQ_APPEND[pass];
        rab.rab$w_rsz = (uint16_t)strlen(TESTSEQ_APPEND[pass]);
        st = sys$put(&rab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$PUT appends a record (working copy now dirty)");
        sys$disconnect(&rab, 0, 0);

        st = sys$close(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$CLOSE mints the next ODS-2 version (checkin)");

        /* The just-minted version reads back byte-exact via the genuine reader. */
        char verpath[256];
        snprintf(verpath, sizeof(verpath), "%s;%u", SEQ_VMS_PATH, want_ver);
        const char *expect = (want_ver == 2) ? expect_v2 : expect_v3;
        size_t explen = strlen(expect);
        uint8_t rback[2048];
        size_t rlen = 0;
        int vst = ods2_sysdisk_read_file(verpath, rback, sizeof(rback), &rlen);
        CHECK(vst == SS$_NORMAL, "genuine reader reads the newly minted version");
        CHECK(rlen == explen && memcmp(rback, expect, explen) == 0,
              want_ver == 2
                  ? "minted TESTSEQ.DAT;2 is byte-exact (prior content + append)"
                  : "minted TESTSEQ.DAT;3 is byte-exact (;2 content + append)");
    }

    /* All three versions coexist in the directory (proves the multiversion
     * dir_insert path -- one directory record now carries ;1, ;2, ;3). */
    CHECK(count_versions() == 3,
          "TESTSEQ.DAT has 3 coexisting versions after two write-open checkins");

    /* Every version is still independently readable and byte-exact -- the older
     * versions were NOT overwritten by the newer checkins. */
    {
        uint8_t v1[2048];
        size_t v1len = 0;
        int vst = ods2_sysdisk_read_file(SEQ_VMS_PATH ";1", v1, sizeof(v1),
                                         &v1len);
        CHECK(vst == SS$_NORMAL &&
              v1len == sizeof(SEQ_CONTENT) - 1 &&
              memcmp(v1, SEQ_CONTENT, v1len) == 0,
              "original TESTSEQ.DAT;1 still byte-intact alongside ;2 and ;3");

        uint8_t v2[2048];
        size_t v2len = 0;
        vst = ods2_sysdisk_read_file(SEQ_VMS_PATH ";2", v2, sizeof(v2), &v2len);
        CHECK(vst == SS$_NORMAL && v2len == strlen(expect_v2) &&
              memcmp(v2, expect_v2, v2len) == 0,
              "TESTSEQ.DAT;2 still byte-exact after ;3 was minted");
    }

    /* ---- (3) read-only $OPEN + $CLOSE leaves the volume unchanged ---- */
    {
        struct FAB fab = cc$rms_fab;
        char spec[] = SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE/NEWFILE.DAT";
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_fac = FAB$M_GET;
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_org = FAB$C_SEQ;

        uint32_t st = sys$open(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "$OPEN NEWFILE.DAT read-only");
        CHECK(fab._sysdisk_writable == 0, "read-only open is not a checkin candidate");
        struct RAB rab = cc$rms_rab;
        rab.rab$l_fab = &fab;
        sys$connect(&rab, 0, 0);
        char one[512];
        rab.rab$l_ubf = one; rab.rab$w_usz = sizeof(one);
        sys$get(&rab, 0, 0);
        sys$disconnect(&rab, 0, 0);
        st = sys$close(&fab, 0, 0);
        CHECK(st == RMS$_NORMAL, "read-only $CLOSE");
    }
    CHECK(count_named_versions("NEWFILE.DAT") == 1,
          "read-only open+close minted NO spurious version (NEWFILE still 1)");

    /* ---- cleanup ---- */
    vmsfs_volume_unregister(SYSDISK_DEVICE);
    unlink(img_path);

    if (g_failures == 0) {
        printf("=== ALL CHECKS PASSED ===\n");
        return 0;
    }
    printf("=== %d CHECK(S) FAILED ===\n", g_failures);
    return 1;
}
