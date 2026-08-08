#ifndef OVMX_APPS_PARTS_H
#define OVMX_APPS_PARTS_H

/*
 * PARTS.H - shared definitions for the PARTS demo application.
 *
 * PARTS is a small VMS C business-records application built over an RMS
 * indexed (ISAM) file with a single primary key. It is the OVMX 0.2 "runs a
 * real VMS app" demo (beads vms-e97 / vms-f20): authored in VMS C against the
 * Tier-1/2 RMS system services (sys$create/$connect/$put/$get/$close), built
 * through the VMS-native toolchain (cc -> LINK.EXE with a .vms$sv symbol
 * vector, activated by IMGACT.EXE), and run under the real QEMU runtime.
 *
 * Single-user: RMS record locking is unwired in OVMX (bead vms-407), so PARTS
 * opens the file for exclusive access and never depends on shared-record locks.
 */

#include <stdint.h>

/*
 * The primary key is the part number: exactly PARTS_KEY_SIZE characters,
 * left-justified and blank-filled (VMS string-key convention), stored at
 * offset 0 of every record. Fixed-width so the stored key bytes and a lookup
 * key buffer are byte-for-byte comparable by RMS's key comparator.
 */
#define PARTS_KEY_SIZE   8       /* "PNnnnnnn"                        */
#define PARTS_DESC_SIZE  40      /* part description, blank-filled     */

/*
 * The part record. Field offsets are fixed and known to the XABKEY: the
 * primary key occupies bytes [0, PARTS_KEY_SIZE). Total record size is
 * PARTS_REC_SIZE (56 bytes). Packed so the on-disk layout is deterministic
 * across compilers/arches (x86_64 and aarch64 lay this out identically).
 */
struct part_record {
    char     part_number[PARTS_KEY_SIZE];    /* offset  0 - primary key   */
    char     description[PARTS_DESC_SIZE];    /* offset  8                  */
    uint32_t quantity;                        /* offset 48 - stock on hand */
    uint32_t price_cents;                     /* offset 52 - unit price     */
} __attribute__((packed));

#define PARTS_REC_SIZE   ((uint16_t)sizeof(struct part_record))

/*
 * Opaque-ish database handle. Holds the RMS control blocks (FAB/RAB/XABKEY)
 * for one open indexed file. The record buffers live in the caller; the
 * handle carries no per-record state.
 */
struct parts_db {
    void    *fab;    /* struct FAB *    */
    void    *rab;    /* struct RAB *    */
    void    *xab;    /* struct XABKEY * */
    char     spec[256];
    int      open;
};

/*
 * Database layer (PARTS_DB.C). Every function returns a VMS status code
 * (odd = success). On error the caller can print status via parts_db_status().
 */

/* Create a brand-new indexed file (fails if it exists) and connect a stream. */
uint32_t parts_db_create(struct parts_db *db, const char *filespec);

/* Open an existing indexed file and connect a stream (for later lookups). */
uint32_t parts_db_open(struct parts_db *db, const char *filespec);

/* Write one record (keyed by rec->part_number). */
uint32_t parts_db_put(struct parts_db *db, const struct part_record *rec);

/* Random keyed read: exact match on an 8-char part number. */
uint32_t parts_db_get(struct parts_db *db, const char *part_number,
                      struct part_record *rec_out);

/* Flush and close. */
void     parts_db_close(struct parts_db *db);

/*
 * Format a part number "PNnnnnnn" (exactly PARTS_KEY_SIZE bytes, no NUL) into
 * key_out for the given ordinal. key_out must be at least PARTS_KEY_SIZE bytes.
 */
void     parts_make_key(char *key_out, unsigned ordinal);

#endif /* OVMX_APPS_PARTS_H */
