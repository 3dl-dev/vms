/*
 * mkrightslist.c - generate a binary $RDBDEF RIGHTSLIST.DAT (vms-f15a,
 * epic vms-d0c).
 *
 * The RIGHTSLIST atomic-flip SEED. Writes SYS$SYSTEM:RIGHTSLIST.DAT as a
 * genuine RMS Prolog-3 INDEXED file of 48-byte $RDBDEF identifier records
 * (primary key = identifier VALUE @0x00, secondary key = identifier NAME
 * @0x10), through the SAME rightslist_rms engine the runtime reads back over
 * the ODS-2 ACP. This REPLACES the shipped ASCII colon-delimited
 * RIGHTSLIST.DAT: the distro no longer ships an ASCII rights database.
 *
 * Runs on the BUILD host (executive absent): it writes the indexed image to a
 * plain filesystem path (argv[1]) through rms_io_posix_wrap + the
 * rightslist_rms engine -- the SAME bytes the runtime reads back over the ACP
 * (the record format is substrate-agnostic, vms-5f0). No ACP, no /dev/vms
 * needed to author the seed.
 *
 * Usage:  mkrightslist <output-path>
 *
 * The seeded identifiers MIRROR the six environmental identifiers the ASCII
 * RIGHTSLIST shipped, at their oracle-measured values
 * (docs/oracle/vax73-rights-database.md §1) -- assigned ALPHABETICALLY at
 * %X80000001..%X80000006 with NO attributes:
 *   BATCH        %X80000001
 *   DIALUP       %X80000002
 *   INTERACTIVE  %X80000003
 *   LOCAL        %X80000004
 *   NETWORK      %X80000005
 *   REMOTE       %X80000006
 * NO UIC identifiers are shipped here: on OVMX the UIC identifiers are DERIVED
 * from SYSUAF.DAT at lookup time (src/libvms/rtl/rightslist.c), so that one
 * account has exactly one UIC in the whole system (rightslist.h / vms-e60).
 * The ASCII file this replaces carried no UIC rows either.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#include "rightslist_rms.h"   /* the binary $RDBDEF indexed-file engine        */
#include "rms_io.h"           /* rms_io_posix_wrap / _unwrap                    */
#include "rmsdef.h"
#include "ssdef.h"

struct seed_id {
    const char *name;
    uint32_t    value;
    uint32_t    attr;
};

/* The six environmental identifiers, oracle-measured, no attributes. */
static const struct seed_id g_seed[] = {
    { "BATCH",       0x80000001u, 0 },
    { "DIALUP",      0x80000002u, 0 },
    { "INTERACTIVE", 0x80000003u, 0 },
    { "LOCAL",       0x80000004u, 0 },
    { "NETWORK",     0x80000005u, 0 },
    { "REMOTE",      0x80000006u, 0 },
};

static void build_record(const struct seed_id *s, rdb_identifier_record_t *out)
{
    memset(out, 0, sizeof(*out));           /* holder @0x08 stays 0 (def rec) */
    rdb_ident_set_value(out, s->value);
    rdb_ident_set_attributes(out, s->attr);
    rdb_ident_set_name(out, s->name);       /* upcased, blank-padded to 32     */
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-path>\n", argv[0]);
        return 2;
    }

    int fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) {
        perror("mkrightslist: open");
        return 1;
    }

    rms_file_t *h = rms_io_posix_wrap(fd);
    if (!h) {
        fprintf(stderr, "mkrightslist: rms_io_posix_wrap failed\n");
        close(fd);
        return 1;
    }

    rightslist_rms_file_t rf;
    uint32_t st = rightslist_rms_create(h, &rf);
    if (st != RMS$_CREATED) {
        fprintf(stderr, "mkrightslist: rightslist_rms_create failed (0x%08x)\n", st);
        rms_io_posix_unwrap(h);
        return 1;
    }

    unsigned n = (unsigned)(sizeof(g_seed) / sizeof(g_seed[0]));
    for (unsigned i = 0; i < n; i++) {
        rdb_identifier_record_t rec;
        build_record(&g_seed[i], &rec);
        st = rightslist_put_identifier(&rf, &rec);
        if (!$VMS_STATUS_SUCCESS(st)) {
            fprintf(stderr, "mkrightslist: put %s failed (0x%08x)\n",
                    g_seed[i].name, st);
            rightslist_rms_close(&rf);
            rms_io_posix_unwrap(h);
            return 1;
        }
    }

    rightslist_rms_close(&rf);
    rms_io_posix_unwrap(h);
    printf("mkrightslist: wrote binary $RDBDEF RIGHTSLIST (%u identifiers) to %s\n",
           n, argv[1]);
    return 0;
}
