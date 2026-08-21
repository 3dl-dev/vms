/*
 * test_vmsrms.c - Unit tests for vmsrms (Record Management Services)
 *
 * Tests: sys$parse, sys$create, sys$open, sys$close, sys$connect,
 *        sys$get, sys$put, sys$rewind, sys$disconnect
 *
 * Uses temporary files in /tmp. Cleans up on exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

#include "rms/rms.h"
#include "rms/xab.h"
#include "rmsdef.h"
#include "ssdef.h"
#include "descrip.h"
#include "starlet.h"   /* sys$asctim */

/*
 * Stub for vmsfs_resolve — referenced by rms_parse.c but has no definition
 * in the vmsfs library. For unit test purposes, return -1 so sys$parse falls
 * back to using the raw filespec string without device/logical resolution.
 */
int vmsfs_resolve(const char *spec, const char *default_spec,
                  char *result, size_t resultlen)
{
    (void)spec; (void)default_spec; (void)result; (void)resultlen;
    return -1;
}

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* Temporary files to clean up */
static char tmp_path[256];
static char tmp_meta[256];

static void cleanup(void)
{
    if (tmp_path[0]) {
        unlink(tmp_path);
        tmp_path[0] = '\0';
    }
    if (tmp_meta[0]) {
        unlink(tmp_meta);
        tmp_meta[0] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Test: sys$parse - parse a VMS filespec into NAM components         */
/* ------------------------------------------------------------------ */
static void test_parse(void)
{
    printf("\n--- sys$parse ---\n");

    char esa[256];
    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;

    nam.nam$l_esa = esa;
    nam.nam$b_ess = sizeof(esa) - 1;
    fab.fab$l_nam = &nam;

    /* Test 1: simple name.type;version */
    char spec1[] = "LOGIN.COM;1";
    fab.fab$l_fna = spec1;
    fab.fab$b_fns = (uint8_t)strlen(spec1);

    uint32_t st = sys$parse(&fab, 0, 0);
    check(st == RMS$_NORMAL, "parse LOGIN.COM;1 returns NORMAL");
    check(nam.nam$b_name > 0, "name component length set");
    check(nam.nam$l_name != NULL, "name component pointer set");
    check(nam.nam$b_type > 0, "type component length set");
    check(nam.nam$b_ver > 0, "version component length set");

    /* Verify name is "LOGIN" */
    if (nam.nam$l_name && nam.nam$b_name == 5) {
        check(strncmp(nam.nam$l_name, "LOGIN", 5) == 0, "name is LOGIN");
    } else {
        check(0, "name is LOGIN (length wrong)");
    }

    /* Verify type includes .COM */
    if (nam.nam$l_type && nam.nam$b_type >= 4) {
        check(strncmp(nam.nam$l_type, ".COM", 4) == 0, "type is .COM");
    } else {
        check(0, "type is .COM (length wrong)");
    }

    /* Test 2: with directory */
    char spec2[] = "[USERS.BARON]STARTUP.COM;2";
    fab.fab$l_fna = spec2;
    fab.fab$b_fns = (uint8_t)strlen(spec2);
    memset(esa, 0, sizeof(esa));
    nam.nam$b_esl = 0;

    st = sys$parse(&fab, 0, 0);
    check(st == RMS$_NORMAL, "parse [USERS.BARON]STARTUP.COM;2 returns NORMAL");
    check(nam.nam$b_dir > 0, "directory component present");
    check(nam.nam$l_fnb & NAM$M_EXP_DIR, "EXP_DIR flag set");

    /* Test 3: filename with no type */
    char spec3[] = "NOEXT";
    fab.fab$l_fna = spec3;
    fab.fab$b_fns = (uint8_t)strlen(spec3);
    memset(esa, 0, sizeof(esa));
    nam.nam$b_esl = 0;

    st = sys$parse(&fab, 0, 0);
    check(st == RMS$_NORMAL, "parse NOEXT (no type) returns NORMAL");
    check(nam.nam$b_name > 0, "name present for NOEXT");

    /* Test 4: wildcard detection */
    char spec4[] = "*.COM;*";
    fab.fab$l_fna = spec4;
    fab.fab$b_fns = (uint8_t)strlen(spec4);
    memset(esa, 0, sizeof(esa));
    nam.nam$b_esl = 0;

    st = sys$parse(&fab, 0, 0);
    check(st == RMS$_NORMAL, "parse *.COM;* returns NORMAL");
    check(nam.nam$l_fnb & NAM$M_WILDCARD, "wildcard flag set for *.COM;*");

    /* Test 5: NULL FAB returns RMS$_FAB */
    st = sys$parse(NULL, 0, 0);
    check(st == RMS$_FAB, "parse NULL FAB returns RMS$_FAB");

    /* Test 6: FAB with no NAM returns RMS$_NAM */
    struct FAB fab2 = cc$rms_fab;
    char spec5[] = "TEST.DAT";
    fab2.fab$l_fna = spec5;
    fab2.fab$b_fns = (uint8_t)strlen(spec5);
    fab2.fab$l_nam = NULL;
    st = sys$parse(&fab2, 0, 0);
    check(st == RMS$_NAM, "parse with no NAM block returns RMS$_NAM");
}

/* ------------------------------------------------------------------ */
/* Test: sys$create / sys$close - create a sequential file            */
/* ------------------------------------------------------------------ */
static void test_create_close(void)
{
    printf("\n--- sys$create / sys$close ---\n");

    /* Build a temp path */
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/test_vmsrms_%d", (int)getpid());
    snprintf(tmp_meta, sizeof(tmp_meta), "%s.rms_meta", tmp_path);

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna  = tmp_path;
    fab.fab$b_fns  = (uint8_t)strlen(tmp_path);
    fab.fab$b_fac  = FAB$M_PUT | FAB$M_GET;
    fab.fab$b_rfm  = FAB$C_STMLF;
    fab.fab$b_org  = FAB$C_SEQ;

    uint32_t st = sys$create(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$create returns NORMAL");
    check(fab.fab$l_sts == RMS$_CREATED, "fab$l_sts is RMS$_CREATED after create");
    check(fab.fab$w_ifi != 0, "IFI assigned after create");
    check(fab._rms_file != 0, "RMS file handle is valid after create");  /* vms-bc7 */

    /*
     * sys$create appends a version number (e.g. ;1) to the path.
     * Store the resolved path for subsequent tests and cleanup.
     */
    strncpy(tmp_path, fab._resolved_path, sizeof(tmp_path) - 1);
    tmp_path[sizeof(tmp_path) - 1] = '\0';
    snprintf(tmp_meta, sizeof(tmp_meta), "%.246s.rms_meta", tmp_path);

    st = sys$close(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$close returns NORMAL after create");
    check(fab._rms_file == 0, "RMS file handle cleared after close");  /* vms-bc7 */
    check(fab.fab$w_ifi == 0, "IFI cleared after close");

    /* Verify file exists on disk (use resolved path with version) */
    check(access(tmp_path, F_OK) == 0, "file exists after create+close");
}

/* ------------------------------------------------------------------ */
/* Test: sys$open - open an existing file                             */
/* ------------------------------------------------------------------ */
static void test_open_close(void)
{
    printf("\n--- sys$open / sys$close ---\n");

    /*
     * Create a file directly (bypassing RMS protection logic) and open it
     * with sys$open. Using a versioned name so sys$open's resolve_for_open
     * can find it directly via rms_resolve_version.
     */
    char openfile[256];
    char openmeta[270];
    snprintf(openfile, sizeof(openfile), "/tmp/test_vmsrms_open_%d;1", (int)getpid());
    snprintf(openmeta, sizeof(openmeta), "%s.rms_meta", openfile);

    /* Create via RMS so metadata sidecar exists */
    char createfile[256];
    snprintf(createfile, sizeof(createfile), "/tmp/test_vmsrms_open_%d", (int)getpid());

    struct FAB cfab = cc$rms_fab;
    cfab.fab$l_fna = createfile;
    cfab.fab$b_fns = (uint8_t)strlen(createfile);
    cfab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    cfab.fab$b_rfm = FAB$C_STMLF;
    sys$create(&cfab, 0, 0);

    /*
     * Save the resolved (versioned) path BEFORE closing.
     * sys$close zeros _resolved_path in some implementations.
     */
    char resolved_open[1024];
    strncpy(resolved_open, cfab._resolved_path, sizeof(resolved_open) - 1);
    resolved_open[sizeof(resolved_open) - 1] = '\0';

    /*
     * Set mode to 0777 (all permissions allowed) so the real open() below
     * -- the only protection decision RMS makes (vms-2b8 deleted the
     * userspace rms_check_protection() pre-check; see sys_security.c) --
     * grants access regardless of the calling UID.
     */
    chmod(resolved_open, 0777);
    sys$close(&cfab, 0, 0);

    /* Now open using sys$open with the resolved (versioned) path */
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna  = resolved_open;
    fab.fab$b_fns  = (uint8_t)strlen(resolved_open);
    fab.fab$b_fac  = FAB$M_GET;

    uint32_t st = sys$open(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$open returns NORMAL for existing file");
    check(fab.fab$l_sts == RMS$_NORMAL, "fab$l_sts is NORMAL after open");
    check(fab.fab$w_ifi != 0, "IFI assigned after open");

    st = sys$close(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$close returns NORMAL after open");

    /* Clean up */
    unlink(resolved_open);
    char meta2[1100]; snprintf(meta2, sizeof(meta2), "%s.rms_meta", resolved_open);
    unlink(meta2);

    /* Try to open a non-existent file */
    char nofile[] = "/tmp/vmsrms_nonexistent_xyz_999";
    struct FAB fab2 = cc$rms_fab;
    fab2.fab$l_fna = nofile;
    fab2.fab$b_fns = (uint8_t)strlen(nofile);
    fab2.fab$b_fac = FAB$M_GET;

    st = sys$open(&fab2, 0, 0);
    check(st == RMS$_FNF, "sys$open non-existent file returns RMS$_FNF");
}

/* ------------------------------------------------------------------ */
/* Test: sys$connect / sys$put / sys$get / sys$rewind / sys$disconnect */
/* ------------------------------------------------------------------ */
static void test_record_io(void)
{
    printf("\n--- record I/O (connect/put/get/rewind/disconnect) ---\n");

    /* Create a fresh file for this test */
    char recfile[256];
    char recmeta[280];
    snprintf(recfile, sizeof(recfile), "/tmp/test_vmsrms_rec_%d", (int)getpid());
    snprintf(recmeta, sizeof(recmeta), "%s.rms_meta", recfile);

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = recfile;
    fab.fab$b_fns = (uint8_t)strlen(recfile);
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab.fab$b_rfm = FAB$C_STMLF;
    fab.fab$b_org = FAB$C_SEQ;

    uint32_t st = sys$create(&fab, 0, 0);
    check(st == RMS$_NORMAL, "create record file");

    /* Connect RAB */
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;

    st = sys$connect(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$connect returns NORMAL");
    check(rab.rab$w_isi != 0, "ISI assigned after connect");

    /* Write three records */
    char rec1[] = "Hello, VMS world";
    char rec2[] = "Second record line";
    char rec3[] = "Third line here";

    rab.rab$l_rbf = rec1;
    rab.rab$w_rsz = (uint16_t)strlen(rec1);
    st = sys$put(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$put record 1");

    rab.rab$l_rbf = rec2;
    rab.rab$w_rsz = (uint16_t)strlen(rec2);
    st = sys$put(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$put record 2");

    rab.rab$l_rbf = rec3;
    rab.rab$w_rsz = (uint16_t)strlen(rec3);
    st = sys$put(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$put record 3");

    /* Rewind */
    st = sys$rewind(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$rewind returns NORMAL");
    check(rab._current_offset == 0, "offset is 0 after rewind");
    check(rab._eof == 0, "EOF cleared after rewind");

    /* Read back the three records */
    char rbuf[256];
    rab.rab$l_ubf = rbuf;
    rab.rab$w_usz = sizeof(rbuf);

    st = sys$get(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$get record 1");
    check(rab.rab$w_rsz == (uint16_t)strlen(rec1), "record 1 size correct");
    check(strncmp(rbuf, rec1, rab.rab$w_rsz) == 0, "record 1 content correct");

    st = sys$get(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$get record 2");
    check(rab.rab$w_rsz == (uint16_t)strlen(rec2), "record 2 size correct");
    check(strncmp(rbuf, rec2, rab.rab$w_rsz) == 0, "record 2 content correct");

    st = sys$get(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$get record 3");
    check(rab.rab$w_rsz == (uint16_t)strlen(rec3), "record 3 size correct");
    check(strncmp(rbuf, rec3, rab.rab$w_rsz) == 0, "record 3 content correct");

    /* Next get should return EOF */
    st = sys$get(&rab, 0, 0);
    check(st == RMS$_EOF, "sys$get at EOF returns RMS$_EOF");

    /* Disconnect */
    st = sys$disconnect(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$disconnect returns NORMAL");
    check(rab.rab$w_isi == 0, "ISI cleared after disconnect");

    sys$close(&fab, 0, 0);

    /* Clean up */
    unlink(recfile);
    unlink(recmeta);
}

/* ------------------------------------------------------------------ */
/* Test: fixed-length record format                                    */
/* ------------------------------------------------------------------ */
static void test_fixed_records(void)
{
    printf("\n--- fixed-length record I/O ---\n");

    char fixfile[256];
    char fixmeta[280];
    snprintf(fixfile, sizeof(fixfile), "/tmp/test_vmsrms_fix_%d", (int)getpid());
    snprintf(fixmeta, sizeof(fixmeta), "%s.rms_meta", fixfile);

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = fixfile;
    fab.fab$b_fns = (uint8_t)strlen(fixfile);
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab.fab$b_rfm = FAB$C_FIX;
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$w_mrs = 16;  /* 16-byte fixed records */

    uint32_t st = sys$create(&fab, 0, 0);
    check(st == RMS$_NORMAL, "create fixed-length file");

    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    sys$connect(&rab, 0, 0);

    /* Write a fixed-length record */
    char data[16] = "ABCDEFGHIJKLMNOP";
    rab.rab$l_rbf = data;
    rab.rab$w_rsz = 16;
    st = sys$put(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$put fixed-length record");

    /* Write another */
    char data2[16] = "0123456789ABCDEF";
    rab.rab$l_rbf = data2;
    rab.rab$w_rsz = 16;
    st = sys$put(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$put fixed-length record 2");

    sys$rewind(&rab, 0, 0);

    char rbuf[32];
    rab.rab$l_ubf = rbuf;
    rab.rab$w_usz = sizeof(rbuf);

    st = sys$get(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$get fixed record 1");
    check(rab.rab$w_rsz == 16, "fixed record 1 size is 16");
    check(memcmp(rbuf, data, 16) == 0, "fixed record 1 content correct");

    st = sys$get(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$get fixed record 2");
    check(rab.rab$w_rsz == 16, "fixed record 2 size is 16");
    check(memcmp(rbuf, data2, 16) == 0, "fixed record 2 content correct");

    sys$disconnect(&rab, 0, 0);
    sys$close(&fab, 0, 0);

    unlink(fixfile);
    unlink(fixmeta);
}

/* ------------------------------------------------------------------ */
/* Test: XAB creation/revision dates are real VMS binary time (vms-3dd) */
/*                                                                      */
/* Regression: rms_impl_display() used to stuff raw Unix time_t into    */
/* xab$q_cdt/xab$q_rdt, which VMS defines as 100ns intervals since      */
/* 17-NOV-1858. A raw time_t (~1.6e9) read as a VMS quadword decodes to */
/* a few minutes past the 1858 base date -- ~112 years wrong and in the */
/* wrong units. The fix converts through lib$cvt_vectim. This test sets  */
/* a known revision time, displays the XAB, and asserts the quadword     */
/* round-trips to the same wall-clock instant and renders via $ASCTIM.   */
static void test_xab_dates(void)
{
    printf("\n--- XAB creation/revision dates (vms-3dd) ---\n");

    /* Ticks between the VMS base date (17-NOV-1858) and the Unix epoch,
     * in 100ns units. Documented VMS constant; the same value lives in
     * src/libvms/rtl/lib_datetime.c and src/libvms/syssvc/sys_time.c. Used
     * here only to DECODE the produced quadword for the round-trip check. */
    const uint64_t VMS_EPOCH_OFFSET = 0x007C95674BEB4000ULL;

    char datefile[256];
    char datemeta[280];
    snprintf(datefile, sizeof(datefile), "/tmp/test_vmsrms_dat_%d", (int)getpid());
    snprintf(datemeta, sizeof(datemeta), "%s.rms_meta", datefile);

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = datefile;
    fab.fab$b_fns = (uint8_t)strlen(datefile);
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$b_org = FAB$C_SEQ;

    uint32_t st = sys$create(&fab, 0, 0);
    check(st == RMS$_NORMAL, "create file for date test");
    /* The on-disk path carries a version suffix (e.g. ";1"); capture it
     * before sys$close zeros _resolved_path. utimensat must target the real
     * file that sys$display() will stat, not the bare filespec. */
    char resolved[1024];
    strncpy(resolved, fab._resolved_path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';
    sys$close(&fab, 0, 0);

    /* Pin a known revision instant: 15-JUN-2021 12:00:00 UTC. Noon keeps the
     * calendar DAY/MONTH/YEAR stable under any reasonable timezone shift the
     * $ASCTIM display might apply. */
    struct tm known;
    memset(&known, 0, sizeof(known));
    known.tm_year = 2021 - 1900;
    known.tm_mon  = 6 - 1;
    known.tm_mday = 15;
    known.tm_hour = 12;
    known.tm_min  = 0;
    known.tm_sec  = 0;
    time_t known_t = timegm(&known);

    struct timespec times[2];
    times[0].tv_sec = known_t; times[0].tv_nsec = 0;   /* atime */
    times[1].tv_sec = known_t; times[1].tv_nsec = 0;   /* mtime -> xab$q_rdt */
    check(utimensat(AT_FDCWD, resolved, times, 0) == 0, "set known mtime on file");

    /* Re-open and $DISPLAY with a XABDAT chained to the FAB. */
    struct XABDAT dat = cc$rms_xabdat;
    struct FAB fab2 = cc$rms_fab;
    fab2.fab$l_fna = datefile;
    fab2.fab$b_fns = (uint8_t)strlen(datefile);
    fab2.fab$b_fac = FAB$M_GET;
    fab2.fab$l_xab = (struct XABKEY *)&dat;

    st = sys$open(&fab2, 0, 0);
    check(st == RMS$_NORMAL, "open file for display");
    st = sys$display(&fab2, 0, 0);
    check(st == RMS$_NORMAL, "sys$display fills XABDAT");
    sys$close(&fab2, 0, 0);

    /* A raw Unix time_t (~1.6e9) is far below VMS_EPOCH_OFFSET (~3.5e16); a
     * real post-1970 VMS binary time is above it. This alone catches the bug. */
    check(dat.xab$q_rdt > VMS_EPOCH_OFFSET,
          "revision date is a post-1970 VMS binary time, not raw time_t");
    check(dat.xab$q_cdt > VMS_EPOCH_OFFSET,
          "creation date is a post-1970 VMS binary time, not raw time_t");

    /* Round-trip: decode the revision quadword back to Unix seconds. */
    uint64_t ticks = dat.xab$q_rdt - VMS_EPOCH_OFFSET;
    time_t decoded = (time_t)(ticks / 10000000ULL);
    check(decoded == known_t,
          "revision date round-trips to the exact wall-clock instant");

    /* $ASCTIM must render the quadword as a real VMS date string. */
    char abuf[64];
    struct dsc$descriptor_s adsc;
    adsc.dsc$w_length  = sizeof(abuf);
    adsc.dsc$b_dtype   = DSC$K_DTYPE_T;
    adsc.dsc$b_class   = DSC$K_CLASS_S;
    adsc.dsc$a_pointer = abuf;
    uint16_t alen = 0;
    uint64_t rdt = dat.xab$q_rdt;
    st = sys$asctim(&alen, &adsc, &rdt, 0);
    check(st == SS$_NORMAL, "sys$asctim converts the revision date");
    if (alen >= sizeof(abuf)) alen = sizeof(abuf) - 1;
    abuf[alen] = '\0';
    printf("    $ASCTIM rendered: '%s'\n", abuf);
    check(strstr(abuf, "2021") != NULL, "sys$asctim output shows the year 2021");
    check(strstr(abuf, "JUN") != NULL, "sys$asctim output shows the month JUN");

    unlink(resolved);
    unlink(datefile);
    unlink(datemeta);
}

int main(void)
{
    printf("=== vmsrms unit tests ===\n");

    test_parse();
    test_create_close();
    test_open_close();
    test_record_io();
    test_fixed_records();
    test_xab_dates();

    /* Clean up main temp files */
    cleanup();

    if (failures == 0)
        printf("\nAll vmsrms tests passed.\n");
    else
        printf("\nSome vmsrms tests FAILED (%d).\n", failures);

    return failures;
}
