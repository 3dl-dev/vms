/*
 * test_rightslist_rms.c - vms-3e0 (epic vms-d0c): genuine binary $RDBDEF
 * RIGHTSLIST over the Files-11 Prolog-3 indexed engine. The binary sibling of
 * the SYSUAF rung.
 *
 * Authors a real Prolog-3 indexed RIGHTSLIST (primary key = identifier VALUE,
 * secondary key = identifier NAME) with the ORACLE's real identifiers
 * (docs/oracle/vax73-rights-database.md: the environmental IDs at %X8000000x,
 * the SYSTEM/DEFAULT UIC identifiers, plus the purpose-built RESOURCE
 * identifier OVMXRES from docs/oracle/vax73-alpha84-rdbdef.md §2a), then:
 *   1. re-binds the on-disk prologue and proves it is a real 2-key Prolog-3
 *      image (VALUE primary + NAME secondary),
 *   2. reads every identifier BACK BY NAME (secondary key -> SIDR -> RFA ->
 *      primary record) and asserts a byte-exact 48-byte record,
 *   3. reads every identifier BACK BY VALUE (primary key) and asserts the same,
 *   4. asserts the record FIELDS land at the exact oracle offsets -- value@0x00,
 *      attributes@0x04, holder@0x08 (==0 for a definition record), name@0x10 --
 *      byte-for-byte,
 *   5. resolves VALUE -> NAME (read by value, then rdb_ident_name),
 *   6. checks the RESOURCE attribute bit (oracle-pinned bit 0) round-trips.
 * 9 identifiers with 48-byte records and 1-block buckets force a genuine
 * data-bucket + SIDR split, so this is a real index, NOT a flat scan.
 *
 * Host-only: the engine reads/writes blocks via rms_io_* wrapping a plain fd
 * (host ctest has no /dev/vms); the /dev/vms ACP end-to-end is the shared
 * Prolog-3 ACP positive (tests/qemu/test_syssvc_rms_p3_acp.c). No ASCII colon
 * format, no flat file, no /vms passthrough, no stub.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

#include "rightslist_rms.h"
#include "rmsdef.h"
#include "ssdef.h"

static int failures = 0;
static void check(int cond, const char *name)
{
    if (cond) { printf("  OK: %s\n", name); }
    else { printf("  FAIL: %s\n", name); failures++; }
}

/* The oracle's identifiers: {name, value, attributes}. Environmental IDs and
 * the two UIC identifiers from docs/oracle/vax73-rights-database.md §1; OVMXRES
 * (RESOURCE) from docs/oracle/vax73-alpha84-rdbdef.md §2a. */
struct id_ent {
    const char *name;
    uint32_t    value;
    uint32_t    attr;
};

static struct id_ent g_ids[] = {
    { "BATCH",       0x80000001u, 0 },                 /* environmental */
    { "DIALUP",      0x80000002u, 0 },
    { "INTERACTIVE", 0x80000003u, 0 },
    { "LOCAL",       0x80000004u, 0 },
    { "NETWORK",     0x80000005u, 0 },
    { "REMOTE",      0x80000006u, 0 },
    { "SYSTEM",      0x00010004u, 0 },                 /* UIC [1,4] octal */
    { "DEFAULT",     0x00800080u, 0 },                 /* UIC [200,200] octal */
    { "OVMXRES",     0x80010003u, RDB$M_RESOURCE },    /* /ATTRIBUTES=RESOURCE */
};
#define NIDS ((int)(sizeof(g_ids) / sizeof(g_ids[0])))

static void build_record(const struct id_ent *e, rdb_identifier_record_t *r)
{
    memset(r, 0, sizeof(*r));           /* holder field @0x08 stays 0 (def rec) */
    rdb_ident_set_value(r, e->value);
    rdb_ident_set_attributes(r, e->attr);
    rdb_ident_set_name(r, e->name);
}

/* Byte-exact field check at the oracle offsets on the raw 48 bytes. */
static void assert_oracle_offsets(const struct id_ent *e,
                                  const rdb_identifier_record_t *out,
                                  const char *tag)
{
    const uint8_t *b = (const uint8_t *)out;
    char label[96];

    snprintf(label, sizeof(label), "%s: value@0x00 byte-exact", tag);
    check(p3_le32(b + RDB$K_IDENTIFIER_OFF) == e->value, label);

    snprintf(label, sizeof(label), "%s: attributes@0x04 byte-exact", tag);
    check(p3_le32(b + RDB$K_ATTRIB_OFF) == e->attr, label);

    /* holder@0x08 must be 0 for a definition record (all 8 bytes) */
    int holder_zero = 1;
    for (int i = 0; i < 8; i++)
        if (b[RDB$K_HOLDER_OFF + i] != 0) holder_zero = 0;
    snprintf(label, sizeof(label), "%s: holder@0x08 == 0 (definition record)", tag);
    check(holder_zero, label);

    /* name@0x10 upcased, blank-padded to 32 */
    uint8_t want[RDB$K_NAME_LEN];
    size_t n = strlen(e->name);
    for (size_t i = 0; i < RDB$K_NAME_LEN; i++)
        want[i] = (i < n) ? (uint8_t)e->name[i] : (uint8_t)' ';  /* names given upcase */
    snprintf(label, sizeof(label), "%s: name@0x10 byte-exact (32, blank-padded)", tag);
    check(memcmp(b + RDB$K_NAME_OFF, want, RDB$K_NAME_LEN) == 0, label);
}

int main(void)
{
    printf("test_rightslist_rms (vms-3e0): binary $RDBDEF RIGHTSLIST, indexed\n");

    /* record layout is the oracle's 48-byte $RDBDEF -- proven at compile time
     * by the _Static_asserts in rightslist_rms.h; restate the size here. */
    check(sizeof(rdb_identifier_record_t) == 48, "identifier record is 48 bytes");
    check(sizeof(rdb_holder_record_t) == 16, "holder record is 16 bytes");

    char path[] = "/tmp/ovmx_rdb_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }
    rms_file_t *f = rms_io_posix_wrap(fd);
    check(f != NULL, "wrap fd");
    if (!f) { close(fd); unlink(path); return 1; }

    /* ---- $CREATE the 2-key RIGHTSLIST ---- */
    rightslist_rms_file_t rf;
    uint32_t st = rightslist_rms_create(f, &rf);
    check(st == RMS$_CREATED && rf.ctx, "rightslist_rms_create (VALUE + NAME keys)");
    if (!rf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }
    check(rf.ctx->num_keys == 2, "two keys: VALUE (primary) + NAME (secondary)");

    /* ---- $PUT every oracle identifier ---- */
    for (int i = 0; i < NIDS; i++) {
        rdb_identifier_record_t r;
        build_record(&g_ids[i], &r);
        uint32_t ps = rightslist_put_identifier(&rf, &r);
        char label[64];
        snprintf(label, sizeof(label), "put %s", g_ids[i].name);
        check(ps == RMS$_NORMAL, label);
    }
    rightslist_rms_close(&rf);

    /* ---- re-bind: prove the on-disk 2-key Prolog-3 image ---- */
    st = rightslist_rms_open(f, &rf);
    check($VMS_STATUS_SUCCESS(st) && rf.ctx, "rightslist_rms_open re-binds prologue");
    check(rf.ctx && rf.ctx->num_keys == 2 &&
          rf.ctx->keys[0].ref == RIGHTSLIST_KRF_VALUE &&
          rf.ctx->keys[0].key_size == 4 &&
          rf.ctx->keys[0].seg0_pos == RDB$K_IDENTIFIER_OFF &&
          rf.ctx->keys[1].ref == RIGHTSLIST_KRF_NAME &&
          rf.ctx->keys[1].key_size == RDB$K_NAME_LEN &&
          rf.ctx->keys[1].seg0_pos == RDB$K_NAME_OFF,
          "prologue: key0=VALUE@0x00/4, key1=NAME@0x10/32");

    /* ---- read BY NAME (secondary key) + assert oracle offsets ---- */
    for (int i = 0; i < NIDS; i++) {
        rdb_identifier_record_t out;
        memset(&out, 0xEE, sizeof(out));
        uint32_t gs = rightslist_get_by_name(&rf, g_ids[i].name, &out);
        char label[80];
        snprintf(label, sizeof(label), "get_by_name %s (secondary)", g_ids[i].name);
        check(gs == RMS$_NORMAL, label);
        if (gs == RMS$_NORMAL) {
            char tag[64];
            snprintf(tag, sizeof(tag), "by-name %s", g_ids[i].name);
            assert_oracle_offsets(&g_ids[i], &out, tag);
        }
    }

    /* ---- read BY VALUE (primary key) + assert oracle offsets + value->name -- */
    for (int i = 0; i < NIDS; i++) {
        rdb_identifier_record_t out;
        memset(&out, 0xEE, sizeof(out));
        uint32_t gs = rightslist_get_by_value(&rf, g_ids[i].value, &out);
        char label[80];
        snprintf(label, sizeof(label), "get_by_value %s (primary)", g_ids[i].name);
        check(gs == RMS$_NORMAL, label);
        if (gs == RMS$_NORMAL) {
            char tag[64];
            snprintf(tag, sizeof(tag), "by-value %s", g_ids[i].name);
            assert_oracle_offsets(&g_ids[i], &out, tag);

            /* value -> name resolution */
            char nm[RDB$K_NAME_LEN + 1];
            rdb_ident_name(&out, nm);
            snprintf(label, sizeof(label), "value 0x%08X -> name \"%s\"",
                     g_ids[i].value, g_ids[i].name);
            check(strcmp(nm, g_ids[i].name) == 0, label);
        }
    }

    /* ---- RESOURCE attribute bit (oracle-pinned bit 0) round-trips ---- */
    {
        rdb_identifier_record_t out;
        uint32_t gs = rightslist_get_by_name(&rf, "OVMXRES", &out);
        check(gs == RMS$_NORMAL &&
              (rdb_ident_attributes(&out) & RDB$M_RESOURCE) != 0 &&
              rdb_ident_value(&out) == 0x80010003u,
              "OVMXRES carries RESOURCE (attr bit 0) + value 0x80010003");
    }

    /* ---- lookups for absent keys fail honestly ---- */
    {
        rdb_identifier_record_t out;
        check(rightslist_get_by_name(&rf, "NOSUCHID", &out) == RMS$_RNF,
              "absent name -> RMS$_RNF (honest miss)");
        check(rightslist_get_by_value(&rf, 0x80009999u, &out) == RMS$_RNF,
              "absent value -> RMS$_RNF (honest miss)");
    }

    /* ---- case folding: lookup with a lowercase name still matches ---- */
    {
        rdb_identifier_record_t out;
        check(rightslist_get_by_name(&rf, "system", &out) == RMS$_NORMAL &&
              rdb_ident_value(&out) == 0x00010004u,
              "case-insensitive name lookup (system -> SYSTEM)");
    }

    rightslist_rms_close(&rf);
    rms_io_posix_unwrap(f);
    unlink(path);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
