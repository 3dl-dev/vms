/*
 * PARTS.C - a real VMS C business-records application over an RMS indexed file.
 *
 * OVMX 0.2 "it runs a real VMS app" demo (beads vms-e97 / vms-f20). PARTS
 * maintains a parts inventory in an RMS *indexed* (ISAM) file keyed on the
 * part number, using only Tier-1/2 RMS system services:
 *
 *     sys$create  - create the indexed file with a primary-key XABKEY
 *     sys$connect - open a record stream
 *     sys$put     - load records
 *     sys$get     - keyed random lookup by part number
 *     sys$close   - close
 *
 * It is single-user (RMS record locking is unwired in OVMX, bead vms-407).
 *
 * Build path (bead vms-f20): cc compiles PARTS.C + PARTS_DB.C; LINK.EXE links
 * them into a VMS-native ET_DYN executable with a .vms$sv symbol vector and
 * PT_INTERP=IMGACT.EXE (no ld/ld.so, no DT_NEEDED/DT_HASH); IMGACT.EXE
 * activates it under the real QEMU runtime.
 *
 * Invocation:
 *     $ RUN PARTS                 ! self-demo: create, load, keyed lookups
 *     $ PARTS LOAD   [n] [spec]   ! create the file and load n records
 *     $ PARTS LOOKUP pnnnnnnn [spec]   ! keyed lookup of one part
 * ("$ RUN" passes no arguments, so the self-demo is the default and needs no
 *  command line - which is what the QEMU end-to-end proof exercises.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     /* strcasecmp */

#include "rms/rms.h"     /* RMS services + rmsdef.h + ssdef.h ($VMS_STATUS_SUCCESS) */

#include "parts.h"

#ifndef PARTS_DEFAULT_COUNT
#define PARTS_DEFAULT_COUNT 10000
#endif

#define PARTS_DEFAULT_SPEC "DKA0:[USERS]PARTS.DAT"

/* Fabricate a deterministic part record for ordinal i (1-based). */
static void make_record(struct part_record *rec, unsigned i)
{
    static const char *kinds[] = {
        "WIDGET", "GASKET", "BEARING", "FLANGE",
        "BRACKET", "COUPLER", "VALVE", "SEAL"
    };
    memset(rec, 0, sizeof(*rec));
    parts_make_key(rec->part_number, i);

    /* description: blank-filled to PARTS_DESC_SIZE (no NUL). */
    char tmp[PARTS_DESC_SIZE + 1];
    snprintf(tmp, sizeof(tmp), "%s, TYPE %u", kinds[i % 8], (i % 900) + 100);
    size_t n = strlen(tmp);
    memset(rec->description, ' ', PARTS_DESC_SIZE);
    memcpy(rec->description, tmp, n < PARTS_DESC_SIZE ? n : PARTS_DESC_SIZE);

    rec->quantity    = (i * 7u) % 500u;
    rec->price_cents = 99u + (i % 5000u);
}

/* Print one record in a readable, VMS-ish columnar form. */
static void print_record(const struct part_record *rec)
{
    /* Trim trailing blanks from the fixed-width description for display. */
    int dlen = PARTS_DESC_SIZE;
    while (dlen > 0 && rec->description[dlen - 1] == ' ')
        dlen--;
    printf("    %.*s  %-*.*s  qty=%-4u  price=$%u.%02u\n",
           PARTS_KEY_SIZE, rec->part_number,
           PARTS_DESC_SIZE, dlen, rec->description,
           rec->quantity,
           rec->price_cents / 100u, rec->price_cents % 100u);
}

/* Create the file and load `count` records. Returns a VMS status. */
/*
 * Create the indexed file on `db` (left OPEN) and load `count` records.
 *
 * The handle is deliberately kept open so the caller can do keyed lookups on
 * the same stream. (OVMX's sys$close currently frees the in-memory B-tree
 * without persisting records added since the last periodic index save, so a
 * close-then-reopen would lose the tail of a large load - a real RMS defect,
 * filed separately. Load-then-query on one open stream is a normal RMS usage
 * pattern and sidesteps it entirely.)
 */
static uint32_t do_load(struct parts_db *db, const char *spec, unsigned count)
{
    uint32_t st = parts_db_create(db, spec);
    if (!$VMS_STATUS_SUCCESS(st)) {
        printf("%%PARTS-F-CRE, cannot create %s (RMS status %%X%08X)\n",
               spec, (unsigned)st);
        return st;
    }
    printf("%%PARTS-I-CREATE, created indexed file %s (primary key = part number)\n",
           spec);

    unsigned written = 0;
    for (unsigned i = 1; i <= count; i++) {
        struct part_record rec;
        make_record(&rec, i);
        st = parts_db_put(db, &rec);
        if (!$VMS_STATUS_SUCCESS(st)) {
            printf("%%PARTS-F-PUT, sys$put failed at record %u (RMS status %%X%08X)\n",
                   i, (unsigned)st);
            return st;
        }
        written++;
    }
    printf("%%PARTS-I-LOADED, %u part records written to the indexed file\n",
           written);
    fflush(stdout);
    return SS$_NORMAL;
}

/* Keyed lookup of one part number (exactly 8 chars) against an open db. */
static uint32_t lookup_one(struct parts_db *db, const char *key8)
{
    struct part_record rec;
    uint32_t st = parts_db_get(db, key8, &rec);
    if ($VMS_STATUS_SUCCESS(st)) {
        printf("  %%PARTS-S-FOUND  key %.8s:\n", key8);
        print_record(&rec);
    } else if (st == RMS$_RNF) {
        printf("  %%PARTS-W-NOTFOUND, no part with key %.8s\n", key8);
    } else {
        printf("  %%PARTS-E-GET, sys$get failed for key %.8s (RMS status %%X%08X)\n",
               key8, (unsigned)st);
    }
    fflush(stdout);
    return st;
}

/*
 * Perform a set of keyed random lookups on an already-open db: the first, a
 * middle, and the last loaded record (all must be FOUND by key), plus one key
 * that was never loaded (must be NOTFOUND). Returns the number of lookups that
 * did not match expectation (0 = all correct).
 */
static int do_lookups(struct parts_db *db, unsigned count)
{
    printf("%%PARTS-I-LOOKUP, keyed random lookups (proving indexed, not sequential, access):\n");

    int wrong = 0;
    char key[PARTS_KEY_SIZE];
    unsigned probes[3];
    probes[0] = 1;
    probes[1] = (count / 2) ? (count / 2) : 1;
    probes[2] = count;
    for (int p = 0; p < 3; p++) {
        parts_make_key(key, probes[p]);
        if (!$VMS_STATUS_SUCCESS(lookup_one(db, key)))
            wrong++;                         /* a loaded key MUST be found */
    }

    /* PN999999 is outside the loaded range (count <= 999998); must be NOTFOUND. */
    parts_make_key(key, 999999u);
    if (lookup_one(db, key) != RMS$_RNF)
        wrong++;

    return wrong;
}

static unsigned env_count(unsigned fallback)
{
    const char *e = getenv("PARTS_COUNT");
    if (e && *e) {
        unsigned long v = strtoul(e, NULL, 10);
        if (v > 0)
            return (unsigned)v;
    }
    return fallback;
}

static const char *env_spec(const char *fallback)
{
    const char *e = getenv("PARTS_FILE");
    return (e && *e) ? e : fallback;
}

int main(int argc, char **argv)
{
    const char *spec  = env_spec(PARTS_DEFAULT_SPEC);
    unsigned    count = env_count(PARTS_DEFAULT_COUNT);

    struct parts_db db;
    uint32_t st;

    /* Sub-command dispatch (self-demo when no verb is given, e.g. $ RUN). */
    if (argc >= 2 && strcasecmp(argv[1], "LOAD") == 0) {
        if (argc >= 3) count = (unsigned)strtoul(argv[2], NULL, 10);
        if (argc >= 4) spec  = argv[3];
        st = do_load(&db, spec, count);
        if ($VMS_STATUS_SUCCESS(st))
            do_lookups(&db, count);          /* verify before closing */
        parts_db_close(&db);
        return $VMS_STATUS_SUCCESS(st) ? 0 : 2;
    }
    if (argc >= 2 && strcasecmp(argv[1], "LOOKUP") == 0) {
        if (argc < 3) {
            printf("%%PARTS-E-USAGE, PARTS LOOKUP <part-number> [filespec]\n");
            return 2;
        }
        if (argc >= 4) spec = argv[3];
        char key[PARTS_KEY_SIZE];
        memset(key, ' ', sizeof(key));
        size_t n = strlen(argv[2]);
        memcpy(key, argv[2], n < PARTS_KEY_SIZE ? n : PARTS_KEY_SIZE);
        st = parts_db_open(&db, spec);
        if (!$VMS_STATUS_SUCCESS(st)) {
            printf("%%PARTS-F-OPEN, cannot open %s (RMS status %%X%08X)\n",
                   spec, (unsigned)st);
            return 2;
        }
        st = lookup_one(&db, key);
        parts_db_close(&db);
        return $VMS_STATUS_SUCCESS(st) ? 0 : 1;
    }

    /*
     * Default: the self-contained demo. One open stream: create -> load ->
     * keyed random lookups -> close. This is the path proven under QEMU by
     * $ RUN PARTS (which passes no command line).
     *
     * The file location is chosen by trying a small ordered list of VMS
     * filespecs and using the first sys$create() accepts (a login session may
     * not be able to write every system directory). PARTS_FILE overrides.
     */
    static const char *candidates[] = {
        "SYS$SCRATCH:PARTS.DAT",
        "SYS$LOGIN:PARTS.DAT",
        "DKA0:[USERS]PARTS.DAT",
        "SYS$SYSDEVICE:[SYSTMP]PARTS.DAT",
        "/tmp/PARTS.DAT",
    };
    extern uint32_t parts_db_last_stv;

    printf("%%PARTS-I-BEGIN, OVMX PARTS demo - RMS indexed file, primary key = part number\n");
    printf("%%PARTS-I-CONFIG, records=%u  record-size=%u bytes\n",
           count, (unsigned)PARTS_REC_SIZE);
    fflush(stdout);

    /* Create the file, trying candidate locations unless PARTS_FILE forces one. */
    int created = 0;
    if (getenv("PARTS_FILE")) {
        st = do_load(&db, spec, count);
        created = $VMS_STATUS_SUCCESS(st);
    } else {
        size_t n = sizeof(candidates) / sizeof(candidates[0]);
        for (size_t i = 0; i < n && !created; i++) {
            st = parts_db_create(&db, candidates[i]);
            if ($VMS_STATUS_SUCCESS(st)) {
                spec = candidates[i];
                printf("%%PARTS-I-CREATE, created indexed file %s (primary key = part number)\n",
                       spec);
                created = 1;
                /* load into the just-created, still-open db */
                unsigned written = 0;
                for (unsigned k = 1; k <= count; k++) {
                    struct part_record rec;
                    make_record(&rec, k);
                    uint32_t ps = parts_db_put(&db, &rec);
                    if (!$VMS_STATUS_SUCCESS(ps)) {
                        printf("%%PARTS-F-PUT, sys$put failed at record %u (RMS status %%X%08X)\n",
                               k, (unsigned)ps);
                        parts_db_close(&db);
                        return 2;
                    }
                    written++;
                }
                printf("%%PARTS-I-LOADED, %u part records written to the indexed file\n",
                       written);
                fflush(stdout);
            } else {
                printf("%%PARTS-I-TRYCRE, %s not usable (RMS %%X%08X errno=%u), trying next\n",
                       candidates[i], (unsigned)st, (unsigned)parts_db_last_stv);
                fflush(stdout);
            }
        }
    }

    if (!created) {
        printf("%%PARTS-F-NOFILE, could not create the indexed file in any candidate location\n");
        fflush(stdout);
        return 2;
    }

    int wrong = do_lookups(&db, count);
    parts_db_close(&db);

    if (wrong != 0) {
        printf("%%PARTS-E-VERIFY, %d keyed lookup(s) returned the wrong result\n", wrong);
        fflush(stdout);
        return 2;
    }

    printf("%%PARTS-S-DONE, PARTS demo complete - indexed create, %u puts, keyed gets all correct\n",
           count);
    fflush(stdout);
    return 0;
}
