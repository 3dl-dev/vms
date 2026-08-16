/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_search_ods2.c - proof for the RMS $SEARCH SYS$DISK reroute
 * (src/vmsrms/rms_search.c, epic vms-5eb rung R5b -- vms-dca), the SEARCH half
 * of the ODS-2 runtime flip (docs/design-ods2-runtime-flip.md).
 *
 * WHAT THIS PROVES, AND WHY. $SEARCH's wildcard enumeration historically walks
 * the host directory with opendir(2)/readdir(2) over the /vms passthrough, so
 * it sees POSIX directory entries -- not genuine ODS-2 records/versions. Rung
 * R5b reroutes a SYS$DISK ("/vms/...") directory THROUGH the registered
 * genuine-ODS-2 volume handle (ods2_sysdisk_list_dir): every name + VERSION
 * returned by $SEARCH now comes from the real Master File Directory / FID
 * chains, matched by the version-aware VMS matcher, in the genuine ODS-2
 * directory order -- never opendir's.
 *
 * This test builds a genuine ODS-2 system disk carrying [SYS0.SYSCOMMON.SYSEXE]
 * with MULTIPLE VERSIONS of a file (LOGIN.COM;1/;2/;3) plus a verbatim binary
 * and other files, registers it EXACTLY as PID 1 does at boot (via the boot
 * device path exported in OVMX_SYSDISK_DEV -- this process NEVER calls
 * vmsfs_volume_register itself), then drives the PUBLIC sys$search() and
 * asserts:
 *
 *   - the sequence of matches sys$search returns is BYTE-FOR-BYTE the genuine
 *     ODS-2 records filtered by the pattern, in the reader's delivery order --
 *     verified against ods2_sysdisk_list_dir (the genuine reader), NOT a POSIX
 *     opendir order;
 *   - every version of a name is returned as a DISTINCT match carrying its
 *     genuine ODS-2 version number (LOGIN.COM;3, ;2, ;1);
 *   - $SEARCH iterates one match per call and returns RMS$_NMF exactly once
 *     past the last match, freeing its context;
 *   - a narrower pattern filters the ODS-2 names (only *.EXE returns MAIL.EXE);
 *   - FAIL-HONEST (Rule 9 / INV-6): with NO genuine-ODS-2 SYS$DISK volume
 *     registered (no OVMX_SYSDISK_DEV, or one naming a non-ODS-2 file),
 *     sys$search over a SYS$DISK path returns SS$_DEVNOTMOUNT and reads NOTHING
 *     -- never a silent POSIX fallback, never a crash.
 *
 * Every fact is verified against the ods2.h structs / the genuine reader, never
 * via POSIX stat()/opendir().
 */

#define _POSIX_C_SOURCE 200809L

#include "rms/rms.h"
#include "vmsfs/sysdisk.h"
#include "vmsfs/volume.h"
#include "vmsfs/ods2.h"
#include "vmsfs/filespec.h"
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

/* A "binary" image with embedded NULs / high bytes -- proves verbatim files
 * live on the volume alongside the versioned text ones. */
static const uint8_t IMAGE_BYTES[] = {
    0x7f, 'E', 'L', 'F', 0x00, 0x01, 0x02, 0x00, 0xff, 0xfe, 0x00, 'X'
};
static const char *const LOGIN_TEXT = "$ SET NOON\n";

/*
 * Build a genuine ODS-2 volume with [SYS0.SYSCOMMON.SYSEXE] carrying:
 *   LOGIN.COM;1, ;2, ;3   (three SEPARATE versions, three FIDs -- the
 *                          multi-version case rung R5b must enumerate)
 *   MAIL.EXE;1            (verbatim binary)
 *   SYLOGIN.COM;1         (a second .COM name)
 * and lay it onto a fresh loop image at `path` (mkstemp template, filled in).
 * Returns 0 on success. Mirrors the writer sequence the R6 boot-master emits.
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
    ods2_fid_t login1, login2, login3, mailexe, sylogin;
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

    /* LOGIN.COM;1/;2/;3 -- three SEPARATE files, SAME name, merged into one
     * directory record (highest version delivered first by the reader). */
    STEP(ods2_wvolume_create_file(&wvol, "LOGIN.COM", 1,
                                  (const uint8_t *)LOGIN_TEXT, strlen(LOGIN_TEXT),
                                  sysexe, &login1));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "LOGIN.COM", 1, login1));
    STEP(ods2_wvolume_create_file(&wvol, "LOGIN.COM", 2,
                                  (const uint8_t *)LOGIN_TEXT, strlen(LOGIN_TEXT),
                                  sysexe, &login2));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "LOGIN.COM", 2, login2));
    STEP(ods2_wvolume_create_file(&wvol, "LOGIN.COM", 3,
                                  (const uint8_t *)LOGIN_TEXT, strlen(LOGIN_TEXT),
                                  sysexe, &login3));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "LOGIN.COM", 3, login3));

    /* Verbatim binary image (the R6-master content policy). */
    STEP(ods2_wvolume_create_file_raw(&wvol, "MAIL.EXE", 1,
                                      IMAGE_BYTES, sizeof(IMAGE_BYTES),
                                      sysexe, &mailexe));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "MAIL.EXE", 1, mailexe));

    STEP(ods2_wvolume_create_file(&wvol, "SYLOGIN.COM", 1,
                                  (const uint8_t *)LOGIN_TEXT, strlen(LOGIN_TEXT),
                                  sysexe, &sylogin));
    STEP(ods2_wvolume_dir_insert(&wvol, sysexe, "SYLOGIN.COM", 1, sylogin));

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

/* ---- Expected sequence: the GENUINE ODS-2 records filtered by the pattern,
 * in the reader's delivery order (this is "the ods2 reader" the reroute is
 * verified against -- never a POSIX opendir order). ---- */
#define MAX_EXP 32
struct expect_ctx {
    const char *pattern;
    char names[MAX_EXP][300];   /* "NAME.EXT;version" */
    int  count;
};

static int expect_cb(const char *name, unsigned name_len, uint16_t version,
                     const ods2_fid_t *fid, void *vctx)
{
    (void)fid;
    struct expect_ctx *e = (struct expect_ctx *)vctx;
    char ename[256];
    if (name_len >= sizeof(ename)) name_len = (unsigned)(sizeof(ename) - 1);
    memcpy(ename, name, name_len);
    ename[name_len] = '\0';
    char match[300];
    snprintf(match, sizeof(match), "%s;%u", ename, (unsigned)version);
    if (e->pattern && *e->pattern && !vmsfs_wildcard_match(e->pattern, match))
        return 0;
    if (e->count < MAX_EXP)
        strncpy(e->names[e->count++], match, sizeof(e->names[0]) - 1);
    return 0;
}

/* Return the trailing "NAME.EXT;version" component of a resolved
 * "/vms/.../NAME.EXT;version" path. */
static const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/*
 * Drive sys$search to exhaustion over `expanded`, capturing each match's
 * trailing "NAME.EXT;version" (from FAB._resolved_path) into out[]. Returns the
 * terminating status (RMS$_NMF on a clean walk, or the fail-honest error).
 */
static uint32_t run_search(const char *expanded,
                           char out[][300], int cap, int *n_out)
{
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char rsa[256];
    nam.nam$l_esa = (char *)expanded;
    nam.nam$b_esl = (uint8_t)strlen(expanded);
    nam.nam$l_rsa = rsa;
    nam.nam$b_rss = (uint8_t)sizeof(rsa) - 1;
    nam.nam$$l_context = NULL;
    fab.fab$l_nam = &nam;

    int n = 0;
    uint32_t st;
    for (;;) {
        st = sys$search(&fab, NULL, NULL);
        if (st != RMS$_NORMAL)
            break;
        if (n < cap)
            strncpy(out[n], basename_of(fab._resolved_path),
                    300 - 1);
        n++;
        if (n > cap + 8)    /* runaway guard */
            break;
    }
    *n_out = n;
    return st;
}

int main(void)
{
    printf("=== test_search_ods2: $SEARCH walks the genuine ODS-2 SYS$DISK ===\n");

    /* $SEARCH expanded strings ($PARSE would set nam$l_esa to these; they
     * translate to /vms/SYS0/SYSCOMMON/SYSEXE/<pattern> via vmsfs_to_linux). */
    const char *EXP_ALL = "[SYS0.SYSCOMMON.SYSEXE]*.*";
    const char *EXP_EXE = "[SYS0.SYSCOMMON.SYSEXE]*.EXE";
    const char *SYSEXE_LINUX = "/vms/SYS0/SYSCOMMON/SYSEXE";

    char got[MAX_EXP][300];
    int ngot = 0;

    /* ---------------------------------------------------------------
     * FAIL-HONEST 1: no OVMX_SYSDISK_DEV channel, nothing registered.
     * This process never calls vmsfs_volume_register: the adapter must not
     * fall back to a POSIX opendir of the /vms tree.
     * --------------------------------------------------------------- */
    unsetenv("OVMX_SYSDISK_DEV");
    CHECK(vmsfs_volume_count() == 0, "no volume registered yet");
    uint32_t st = run_search(EXP_ALL, got, MAX_EXP, &ngot);
    CHECK(st == SS$_DEVNOTMOUNT,
          "$SEARCH SYS$DISK, no volume -> SS$_DEVNOTMOUNT (no POSIX fallback)");
    CHECK(ngot == 0, "  ... and returned zero matches");

    /* ---------------------------------------------------------------
     * FAIL-HONEST 2: OVMX_SYSDISK_DEV names a NON-ODS-2 file.
     * --------------------------------------------------------------- */
    char junk_path[] = "/tmp/search_ods2_junkXXXXXX";
    int jfd = mkstemp(junk_path);
    if (jfd >= 0) { (void)!write(jfd, "not an ods2 volume", 18); close(jfd); }
    setenv("OVMX_SYSDISK_DEV", junk_path, 1);
    st = run_search(EXP_ALL, got, MAX_EXP, &ngot);
    CHECK(st == SS$_DEVNOTMOUNT,
          "$SEARCH SYS$DISK, bad channel -> SS$_DEVNOTMOUNT (no POSIX fallback)");

    /* ---------------------------------------------------------------
     * Build + lazily register a genuine ODS-2 SYS$DISK via OVMX_SYSDISK_DEV
     * (exactly the boot path: PID 1 exports the device, the adapter registers
     * DKA0: in this process on first touch).
     * --------------------------------------------------------------- */
    char img_path[] = "/tmp/search_ods2_volXXXXXX";
    if (build_ods2_image(img_path) != 0) {
        printf("  FAIL: could not build ODS-2 image\n");
        return 1;
    }
    setenv("OVMX_SYSDISK_DEV", img_path, 1);

    /* Expected sequences straight from the genuine reader, filtered by the
     * SAME pattern -- reader delivery order (name asc, version high->low). */
    struct expect_ctx exp_all = { EXP_ALL + strlen("[SYS0.SYSCOMMON.SYSEXE]"), {{0}}, 0 };
    st = ods2_sysdisk_list_dir(SYSEXE_LINUX, expect_cb, &exp_all);
    CHECK(st == SS$_NORMAL, "reader lists [SYSEXE] via adapter -> SS$_NORMAL");
    CHECK(exp_all.count == 5,
          "genuine reader yields 5 records (LOGIN;3/;2/;1, MAIL.EXE;1, SYLOGIN;1)");

    /* ---------------------------------------------------------------
     * $SEARCH *.* : the returned sequence must equal the genuine ODS-2
     * records, in reader order, name+version exact.
     * --------------------------------------------------------------- */
    st = run_search(EXP_ALL, got, MAX_EXP, &ngot);
    CHECK(st == RMS$_NMF, "$SEARCH *.* terminates with RMS$_NMF");
    CHECK(ngot == exp_all.count,
          "$SEARCH returns exactly as many matches as the genuine reader");

    int order_ok = (ngot == exp_all.count);
    for (int i = 0; i < ngot && i < exp_all.count; i++) {
        if (strcasecmp(got[i], exp_all.names[i]) != 0) {
            order_ok = 0;
            printf("    mismatch[%d]: $SEARCH='%s' reader='%s'\n",
                   i, got[i], exp_all.names[i]);
        }
    }
    CHECK(order_ok,
          "$SEARCH sequence == genuine ODS-2 records in READER order (not opendir)");

    /* Multi-version: LOGIN.COM appears three times with its genuine versions,
     * highest first -- distinct matches, real ODS-2 versions. */
    int login3 = 0, login2 = 0, login1 = 0, login_seq_ok = 1, last_login = -1;
    for (int i = 0; i < ngot; i++) {
        if (strncasecmp(got[i], "LOGIN.COM;", 10) == 0) {
            int v = atoi(got[i] + 10);
            if (v == 3) login3 = 1;
            if (v == 2) login2 = 1;
            if (v == 1) login1 = 1;
            if (last_login != -1 && v > last_login) login_seq_ok = 0;
            last_login = v;
        }
    }
    CHECK(login3 && login2 && login1,
          "all three LOGIN.COM versions (;3 ;2 ;1) returned as distinct matches");
    CHECK(login_seq_ok, "LOGIN.COM versions returned highest-first (reader order)");

    /* ---------------------------------------------------------------
     * Narrower pattern *.EXE : ODS-2-name wildcard filtering -> MAIL.EXE only.
     * --------------------------------------------------------------- */
    struct expect_ctx exp_exe = { EXP_EXE + strlen("[SYS0.SYSCOMMON.SYSEXE]"), {{0}}, 0 };
    (void)ods2_sysdisk_list_dir(SYSEXE_LINUX, expect_cb, &exp_exe);
    st = run_search(EXP_EXE, got, MAX_EXP, &ngot);
    CHECK(st == RMS$_NMF, "$SEARCH *.EXE terminates with RMS$_NMF");
    CHECK(ngot == exp_exe.count && ngot == 1,
          "$SEARCH *.EXE returns exactly one match (MAIL.EXE)");
    CHECK(ngot == 1 && strcasecmp(got[0], "MAIL.EXE;1") == 0,
          "$SEARCH *.EXE match is MAIL.EXE;1 (genuine ODS-2 record + version)");

    /* ---------------------------------------------------------------
     * A non-SYS$DISK wildcard still walks POSIX (reroute is SYS$DISK-only).
     * Search a real /tmp scratch dir; must NOT return SS$_DEVNOTMOUNT.
     * --------------------------------------------------------------- */
    st = run_search("/tmp/*", got, MAX_EXP, &ngot);
    CHECK(st != SS$_DEVNOTMOUNT,
          "non-SYS$DISK path keeps POSIX walk (not rerouted, no DEVNOTMOUNT)");

    /* Clean up the registered volume + temp files. */
    (void)vmsfs_volume_unregister("DKA0");
    unlink(img_path);
    unlink(junk_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
