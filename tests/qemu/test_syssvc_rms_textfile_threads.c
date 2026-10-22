/*
 * test_syssvc_rms_textfile_threads.c - a reader of a small VMS text file sees
 * every record, every time, while another thread of the same image appends to
 * a different file through the same layer; and a failed open says WHY
 * (rd vms-acf3).
 *
 * WHY. Since rd vms-c89 the persistent NETACP files each line it writes as a
 * record of SYS$MANAGER:NETACP.LOG from a reader thread (sysoutput_rms.c ->
 * rms_textfile_append_line), while its main loop reads the node database
 * (NETNODE_REMOTE.DAT, rms_textfile_open/getline/close) to resolve SET HOST
 * targets. On the demo VAX, SET HOST OVMXA said %SYSTEM-F-NOSUCHNODE for a
 * node the database lists (6 of 8 visitor-gate orders): any failure to read
 * the database read as an EMPTY database.
 *
 * WHAT IS ASSERTED, on a real ACP-mounted ODS-2 volume:
 *   1. CONCURRENCY. A writer thread appends WRITES records to LOG while the
 *      main thread opens, reads and closes DB READS times. Every read opens,
 *      ends at RMS$_EOF (not a read error), and returns exactly DB's three
 *      records with their text. Every append succeeds, and LOG then holds all
 *      WRITES records in order.
 *   2. AN ABSENT FILE IS RMS$_FNF: rms_textfile_open_st on a name that is
 *      not in the directory.
 *   3. A FAILED OPEN THAT IS NOT ABSENCE IS NOT RMS$_FNF/RMS$_DNF: a device
 *      that does not exist. A reader can tell "no database" from "could not
 *      read the database".
 *   4. THE FILE-ACCESS LOCK IS THE FILE'S, NOT ITS FID'S. RMS arbitrates file
 *      sharing with a DLM lock per file. It was named by the FID alone, and in
 *      a VAXcluster -- one DLM, every member booting its own disk mastered from
 *      the same kit -- two DIFFERENT files with the same FID on two members'
 *      disks contended: node A appending its NETACP.LOG turned node B's read
 *      of NETNODE_REMOTE.DAT away (RMS$_SHR), and SET HOST said "remote node
 *      is unknown"; two members' OPERATOR.LOG appends split a log into a new
 *      version. The same class on ONE node: every ODS-2 volume has BADLOG.SYS
 *      at FID (9,9,0). Holding VDA0:'s exclusively (SHR=NIL) must refuse a
 *      second open of THAT file (the check has teeth) and must NOT refuse
 *      VDA300:'s BADLOG.SYS, a different file with the same FID.
 *
 * negctl: rms-textfile-open-failure-reads-as-absent
 * negctl: rms-file-lock-named-by-fid-alone
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rmsdef.h"
#include "rms/rms.h"
#include "rms_textfile.h"
#include "vms_kif.h"

#define EXIT_SKIP 77
#define ODS2_UNIT "VDA0:"
#define DB_SPEC   "VDA0:[OVMXDIR]ACF3DB.DAT"
#define LOG_SPEC  "VDA0:[OVMXDIR]ACF3LOG.LOG"
#define READS     300
#define WRITES    300

static int pass = 0, fail = 0;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else    { printf("  FAIL: %s\n", m); fail++; } } while (0)

static const char *const db_recs[3] = {
    "ACF3 node 1.963 OVMXA",
    "ACF3 node 1.964 OVMXB",
    "ACF3 node 1.965 VAXC",
};

static int append_failures = 0;

static void *writer(void *arg)
{
    (void)arg;
    char line[96];
    for (int i = 0; i < WRITES; i++) {
        snprintf(line, sizeof line, "ACF3 log record %04d", i);
        if (rms_textfile_append_line(LOG_SPEC, line) != 0)
            append_failures++;
    }
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_rms_textfile_threads (a reader sees every record while another thread appends; a failed open says why, rd vms-acf3) ===\n");
    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_rms_textfile_threads: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    CHECK(vms_kif_acp_mount(ODS2_UNIT) & 1, "VDA0: mounted executive-global for the ACP");
    CHECK(rms_textfile_write_line(DB_SPEC, db_recs[0]) == 0 &&
          rms_textfile_append_line(DB_SPEC, db_recs[1]) == 0 &&
          rms_textfile_append_line(DB_SPEC, db_recs[2]) == 0,
          "wrote the three-record database file through RMS");
    CHECK(rms_textfile_create(LOG_SPEC) == 0, "created a new version of the log file through RMS");

    /* 1. Concurrency. */
    pthread_t t;
    int started = pthread_create(&t, NULL, writer, NULL) == 0;
    CHECK(started, "started the writer thread");
    int open_fail = 0, short_read = 0, bad_text = 0, read_err = 0;
    uint32_t first_bad_st = 0;
    for (int i = 0; i < READS; i++) {
        uint32_t st = 0;
        rms_textfile_t *tf = rms_textfile_open_st(DB_SPEC, &st);
        if (!tf) {
            if (!open_fail) first_bad_st = st;
            open_fail++;
            continue;
        }
        char rec[256];
        int n = 0, too_long = 0;
        while (rms_textfile_getline(tf, rec, sizeof rec, &too_long) == 1) {
            if (n < 3 && strcmp(rec, db_recs[n]) != 0) bad_text++;
            n++;
        }
        uint32_t ls = rms_textfile_last_status(tf);
        if (ls != RMS$_EOF) {
            if (!read_err && !open_fail) first_bad_st = ls;
            read_err++;
        }
        if (n != 3) short_read++;
        rms_textfile_close(tf);
    }
    if (started)
        pthread_join(t, NULL);
    if (open_fail || read_err)
        printf("  INFO: first failing status %08X (%u open failures, %u read errors)\n",
               (unsigned)first_bad_st, (unsigned)open_fail, (unsigned)read_err);
    CHECK(open_fail == 0, "every one of the reader's opens succeeded while the other thread appended");
    CHECK(read_err == 0, "every read ended at RMS$_EOF, never at a read error");
    CHECK(short_read == 0 && bad_text == 0,
          "every read returned exactly the database's three records, with their text");
    CHECK(append_failures == 0, "every one of the writer thread's appends succeeded");

    {
        rms_textfile_t *tf = rms_textfile_open(LOG_SPEC);
        int n = 0, in_order = 1, too_long = 0;
        char rec[256], want[96];
        while (tf && rms_textfile_getline(tf, rec, sizeof rec, &too_long) == 1) {
            snprintf(want, sizeof want, "ACF3 log record %04d", n);
            if (strcmp(rec, want) != 0) in_order = 0;
            n++;
        }
        if (tf) rms_textfile_close(tf);
        printf("  INFO: the log holds %d record(s)\n", n);
        CHECK(tf && n == WRITES && in_order, "the log holds every appended record, in order");
    }

    /* 2. Absent is FNF. */
    {
        uint32_t st = 0;
        rms_textfile_t *tf = rms_textfile_open_st("VDA0:[OVMXDIR]ACF3NOSUCH.DAT", &st);
        if (tf) rms_textfile_close(tf);
        printf("  INFO: absent file: status %08X\n", (unsigned)st);
        CHECK(!tf && st == RMS$_FNF, "an absent file's open fails with RMS$_FNF");
    }
    /* 3. Not absence -> not FNF/DNF. */
    {
        uint32_t st = 0;
        rms_textfile_t *tf = rms_textfile_open_st("ACF3NODEV0:[OVMXDIR]ACF3DB.DAT", &st);
        if (tf) rms_textfile_close(tf);
        printf("  INFO: no such device: status %08X\n", (unsigned)st);
        /* negctl: rms-textfile-open-failure-reads-as-absent */
        CHECK(!tf && st != RMS$_FNF && st != RMS$_DNF && !(st & 1),
              "an open that fails for a reason other than absence does not report RMS$_FNF or RMS$_DNF");
    }

    /* 4. Same FID, different volume: no contention. */
    {
        CHECK(vms_kif_acp_mount("VDA300:") & 1, "VDA300: (a second ODS-2 volume) mounted for the ACP");
        static const char a_spec[] = "VDA0:[000000]BADLOG.SYS;1";
        static const char a2_spec[] = "VDA0:[000000]BADLOG.SYS;1";
        static const char b_spec[] = "VDA300:[000000]BADLOG.SYS;1";
        struct FAB fa = cc$rms_fab, fa2 = cc$rms_fab, fb = cc$rms_fab;
        fa.fab$l_fna = (char *)a_spec;   fa.fab$b_fns = (uint8_t)strlen(a_spec);
        fa2.fab$l_fna = (char *)a2_spec; fa2.fab$b_fns = (uint8_t)strlen(a2_spec);
        fb.fab$l_fna = (char *)b_spec;   fb.fab$b_fns = (uint8_t)strlen(b_spec);
        fa.fab$b_fac = fa2.fab$b_fac = fb.fab$b_fac = FAB$M_GET;
        fa.fab$b_shr = fa2.fab$b_shr = fb.fab$b_shr = FAB$M_NIL;   /* exclusive */
        uint32_t sa = sys$open(&fa, 0, 0);
        printf("  INFO: exclusive open of %s: status %08X\n", a_spec, (unsigned)sa);
        CHECK(sa == RMS$_NORMAL, "VDA0:'s BADLOG.SYS (FID 9,9,0) opened exclusively");
        uint32_t sa2 = sys$open(&fa2, 0, 0);
        printf("  INFO: second open of the same file: status %08X\n", (unsigned)sa2);
        CHECK(sa2 == RMS$_SHR,
              "a second open of THAT file while it is held exclusively is refused RMS$_SHR (the check has teeth)");
        if (sa2 == RMS$_NORMAL) sys$close(&fa2, 0, 0);
        uint32_t sb = sys$open(&fb, 0, 0);
        printf("  INFO: exclusive open of %s: status %08X\n", b_spec, (unsigned)sb);
        /* negctl: rms-file-lock-named-by-fid-alone */
        CHECK(sb == RMS$_NORMAL,
              "another volume's BADLOG.SYS -- same FID, different file -- opens while VDA0:'s is held: the file-access lock names the volume, not just the FID");
        if (sb == RMS$_NORMAL) sys$close(&fb, 0, 0);
        if (sa == RMS$_NORMAL) sys$close(&fa, 0, 0);
        (void)vms_kif_acp_dmount("VDA300:");
    }

    (void)vms_kif_acp_dmount(ODS2_UNIT);   /* clean slate for co-resident suites */
    printf("=== test_syssvc_rms_textfile_threads: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
