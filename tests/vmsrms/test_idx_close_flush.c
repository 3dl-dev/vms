/*
 * test_idx_close_flush.c - Regression for vms-5c6d
 *
 * Tier-1 SILENT DATA LOSS: sys$close on an indexed file used to free the
 * in-memory B-tree with a raw free() instead of persisting it first. Because
 * rms_idx_put only writes the index to disk on every 100th insert
 * (num_records % 100 == 0), closing an indexed file dropped every record added
 * since the last %100 boundary -- while sys$close still returned RMS$_NORMAL.
 *
 * This test IS the reproduction: create an indexed file, PUT 250 records (past
 * the 100- and 200-insert save boundaries), close, reopen, and keyed-$GET the
 * records. On pristine origin/main the on-disk index reflects only the state at
 * the last periodic save (insert 200), so records 201-250 are gone and a keyed
 * $GET for key "000250" returns RMS$_RNF. With the fix (btree_save() before the
 * free, in rms_idx_cleanup), every record survives the close/reopen cycle.
 *
 * Runs entirely on the host: the indexed B-tree is a userspace, file-backed
 * structure and needs no /dev/vms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "rms/rms.h"
#include "rms/xab.h"
#include "rmsdef.h"
#include "ssdef.h"

/*
 * Stub for vmsfs_resolve -- referenced by rms_parse.c but not linked here.
 * Return -1 so sys$parse falls back to the raw filespec (matches
 * test_vmsrms.c). We use absolute /tmp paths so no resolution is needed.
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

#define NUM_RECORDS 250
#define KEY_SIZE    6      /* zero-padded decimal, e.g. "000250" */
#define REC_SIZE    32     /* fixed record: [KEY_SIZE key][payload] */

/* Build the record for record number n (1-based): key = zero-padded n at
 * offset 0, followed by a recognizable payload the reader can verify. */
static void build_record(int n, char *rec)
{
    memset(rec, 0, REC_SIZE);
    char key[KEY_SIZE + 1];
    snprintf(key, sizeof(key), "%06d", n);
    memcpy(rec, key, KEY_SIZE);
    /* Payload after the key, so a torn/short read is caught too. */
    snprintf(rec + KEY_SIZE, REC_SIZE - KEY_SIZE, "REC%d", n);
}

/* Remove a data file plus its RMS sidecars. */
static void wipe(const char *path)
{
    char buf[1200];
    unlink(path);
    snprintf(buf, sizeof(buf), "%s.rms_meta", path); unlink(buf);
    snprintf(buf, sizeof(buf), "%s.rms_idx", path);  unlink(buf);
}

/* Keyed $GET of key number n. Returns the RMS status; on success copies the
 * record into out (REC_SIZE bytes). */
static uint32_t keyed_get(struct FAB *fab, int n, char *out, uint16_t *rsz)
{
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = fab;

    uint32_t st = sys$connect(&rab, 0, 0);
    if (st != RMS$_NORMAL) return st;

    char key[KEY_SIZE + 1];
    snprintf(key, sizeof(key), "%06d", n);

    char ubf[REC_SIZE];
    memset(ubf, 0, sizeof(ubf));
    rab.rab$b_rac = RAB$C_KEY;
    rab.rab$l_kbf = key;
    rab.rab$b_ksz = KEY_SIZE;
    rab.rab$l_ubf = ubf;
    rab.rab$w_usz = REC_SIZE;

    st = sys$get(&rab, 0, 0);
    if (st == RMS$_NORMAL) {
        memcpy(out, ubf, REC_SIZE);
        if (rsz) *rsz = rab.rab$w_rsz;
    }
    sys$disconnect(&rab, 0, 0);
    return st;
}

int main(void)
{
    printf("=== vms-5c6d: indexed sys$close flush-before-free ===\n");

    char base[256];
    snprintf(base, sizeof(base), "/tmp/test_idx_close_flush_%d", (int)getpid());
    wipe(base);

    /* ----- Phase 1: create indexed file, PUT 250, close ----- */
    struct XABKEY key_xab = cc$rms_xabkey;   /* primary string key */
    key_xab.xab$b_ref  = 0;
    key_xab.xab$b_dtp  = XAB$C_STG;
    key_xab.xab$w_pos0 = 0;
    key_xab.xab$b_siz0 = KEY_SIZE;
    key_xab.xab$b_nseg = 1;

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = base;
    fab.fab$b_fns = (uint8_t)strlen(base);
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab.fab$b_org = FAB$C_IDX;
    fab.fab$b_rfm = FAB$C_FIX;
    fab.fab$w_mrs = REC_SIZE;
    fab.fab$l_xab = &key_xab;

    uint32_t st = sys$create(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$create indexed file returns NORMAL");

    /* Keep the resolved (versioned) path for reopen. */
    char resolved[1024];
    strncpy(resolved, fab._resolved_path, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab, 0, 0);
    check(st == RMS$_NORMAL, "sys$connect for PUT returns NORMAL");

    int put_ok = 1;
    for (int n = 1; n <= NUM_RECORDS; n++) {
        char rec[REC_SIZE];
        build_record(n, rec);
        rab.rab$b_rac = RAB$C_SEQ;
        rab.rab$l_rbf = rec;
        rab.rab$w_rsz = REC_SIZE;
        st = sys$put(&rab, 0, 0);
        if (st != RMS$_NORMAL) { put_ok = 0; printf("  put %d -> 0x%x\n", n, st); }
    }
    check(put_ok, "all 250 sys$put return NORMAL");

    sys$disconnect(&rab, 0, 0);

    st = sys$close(&fab, 0, 0);
    check(st == RMS$_NORMAL, "sys$close returns NORMAL");
    /* The index tree must be released (no leak, no dangling state). */
    check(fab._rms_state == NULL, "index state freed after close");

    /* ----- Phase 2: reopen and keyed-GET across the save boundaries ----- */
    struct XABKEY key_xab2 = cc$rms_xabkey;
    key_xab2.xab$b_dtp  = XAB$C_STG;
    key_xab2.xab$w_pos0 = 0;
    key_xab2.xab$b_siz0 = KEY_SIZE;
    key_xab2.xab$b_nseg = 1;

    struct FAB rfab = cc$rms_fab;
    rfab.fab$l_fna = resolved;
    rfab.fab$b_fns = (uint8_t)strlen(resolved);
    rfab.fab$b_fac = FAB$M_GET;
    rfab.fab$b_org = FAB$C_IDX;
    rfab.fab$b_rfm = FAB$C_FIX;
    rfab.fab$w_mrs = REC_SIZE;
    rfab.fab$l_xab = &key_xab2;

    st = sys$open(&rfab, 0, 0);
    check(st == RMS$_NORMAL, "sys$open reopened indexed file returns NORMAL");

    /* Records to probe:
     *   1   - first record (present on buggy main too)
     *   100 - exactly at the first periodic save boundary
     *   150 - between boundaries; captured by the insert-200 save (present on
     *         buggy main -- a deliberate control that does NOT discriminate)
     *   201 - FIRST record past the last periodic save -> LOST on buggy main
     *   250 - last record -> LOST on buggy main; the primary discriminator
     */
    int probes[] = { 1, 100, 150, 201, 250 };
    int nprobes = (int)(sizeof(probes) / sizeof(probes[0]));
    int all_found = 1;
    for (int i = 0; i < nprobes; i++) {
        int n = probes[i];
        char got[REC_SIZE];
        uint16_t rsz = 0;
        uint32_t gst = keyed_get(&rfab, n, got, &rsz);
        char msg[96];
        snprintf(msg, sizeof(msg), "keyed $GET record %06d after close/reopen", n);
        int ok = (gst == RMS$_NORMAL);
        if (ok) {
            /* Verify it is the RIGHT record, not just some record. */
            char want[REC_SIZE];
            build_record(n, want);
            ok = (rsz == REC_SIZE) && (memcmp(got, want, REC_SIZE) == 0);
        } else {
            printf("    (record %06d -> status 0x%x; RMS$_RNF=0x%x)\n",
                   n, gst, RMS$_RNF);
        }
        if (!ok) all_found = 0;
        check(ok, msg);
    }
    check(all_found,
          "no records lost across close/reopen (records 201-250 survive)");

    st = sys$close(&rfab, 0, 0);
    check(st == RMS$_NORMAL, "sys$close after reopen returns NORMAL");

    /* ----- Cleanup ----- */
    wipe(base);
    wipe(resolved);

    printf("\n%s (%d failure%s)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
