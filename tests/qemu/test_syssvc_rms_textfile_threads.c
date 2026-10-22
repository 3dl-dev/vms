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
 *   4. AN APPEND NEVER SPLITS A FILE. While another accessor holds the file
 *      past the share wait, an append FAILS (-1); it does not $CREATE a new
 *      version holding only the new record -- which is how OPERATOR.LOG lost
 *      a boot's earlier records under a share conflict (#1845's 2-node e2e).
 *      Once the holder lets go, the append lands in the same file.
 *
 * negctl: rms-textfile-open-failure-reads-as-absent
 * negctl: rms-textfile-append-splits-on-conflict
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

    /* 4. An append under a share conflict fails; it never starts a version. */
    {
        static const char sp[] = "VDA0:[OVMXDIR]ACF3SPLIT.LOG";
        CHECK(rms_textfile_write_line(sp, "ACF3 split record 1") == 0 &&
              rms_textfile_append_line(sp, "ACF3 split record 2") == 0,
              "wrote a two-record file");
        struct FAB hold = cc$rms_fab;
        hold.fab$l_fna = (char *)sp;
        hold.fab$b_fns = (uint8_t)strlen(sp);
        hold.fab$b_fac = FAB$M_GET;
        hold.fab$b_shr = FAB$M_NIL;          /* exclusive: no other accessor */
        uint32_t hs = sys$open(&hold, 0, 0);
        CHECK(hs == RMS$_NORMAL, "another accessor holds the file exclusively");
        int arc = rms_textfile_append_line(sp, "ACF3 split record 3");
        printf("  INFO: append while held: %d\n", arc);
        CHECK(arc != 0, "an append while the file is held past the share wait fails");
        if (hs == RMS$_NORMAL) sys$close(&hold, 0, 0);

        rms_textfile_t *tf = rms_textfile_open(sp);
        char rec[256];
        int n = 0, first_ok = 0, too_long = 0;
        while (tf && rms_textfile_getline(tf, rec, sizeof rec, &too_long) == 1) {
            if (n == 0 && strcmp(rec, "ACF3 split record 1") == 0) first_ok = 1;
            n++;
        }
        if (tf) rms_textfile_close(tf);
        printf("  INFO: the file now reads %d record(s)\n", n);
        /* negctl: rms-textfile-append-splits-on-conflict */
        CHECK(first_ok && n == 2,
              "the failed append did not start a new version: the file still reads its two original records");
        CHECK(rms_textfile_append_line(sp, "ACF3 split record 3") == 0,
              "once the holder lets go, the append succeeds");
        tf = rms_textfile_open(sp);
        n = 0;
        while (tf && rms_textfile_getline(tf, rec, sizeof rec, &too_long) == 1) n++;
        if (tf) rms_textfile_close(tf);
        CHECK(n == 3, "... and lands in the same file (three records)");
    }

    (void)vms_kif_acp_dmount(ODS2_UNIT);   /* clean slate for co-resident suites */
    printf("=== test_syssvc_rms_textfile_threads: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
