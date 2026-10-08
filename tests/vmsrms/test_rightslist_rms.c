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

    char path[64] = "/tmp/ovmx_rdb_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }
    rms_file_t *f = rms_io_posix_wrap(fd);
    check(f != NULL, "wrap fd");
    if (!f) { close(fd); unlink(path); return 1; }

    /* ---- $CREATE the 3-key RIGHTSLIST (VALUE, HOLDER, NAME: the oracle's keys) ---- */
    rightslist_rms_file_t rf;
    uint32_t st = rightslist_rms_create(f, &rf);
    check(st == RMS$_CREATED && rf.ctx, "rightslist_rms_create (VALUE + HOLDER + NAME keys)");
    if (!rf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }
    check(rf.ctx->num_keys == 3, "three keys: VALUE (primary) + HOLDER + NAME (secondaries)");

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
    check(rf.ctx && rf.ctx->num_keys == 3 &&
          rf.ctx->keys[0].ref == RIGHTSLIST_KRF_VALUE &&
          rf.ctx->keys[0].key_size == 4 &&
          rf.ctx->keys[0].seg0_pos == RDB$K_IDENTIFIER_OFF &&
          (rf.ctx->keys[0].flags & (1u << P3_KEYV_DUPKEYS)) &&
          rf.ctx->keys[1].ref == RIGHTSLIST_KRF_HOLDER &&
          rf.ctx->keys[1].key_size == 8 &&
          rf.ctx->keys[1].seg0_pos == RDB$K_HOLDER_OFF &&
          (rf.ctx->keys[1].flags & (1u << P3_KEYV_NULKEYS)) &&
          rf.ctx->keys[2].ref == RIGHTSLIST_KRF_NAME &&
          rf.ctx->keys[2].key_size == RDB$K_NAME_LEN &&
          rf.ctx->keys[2].seg0_pos == RDB$K_NAME_OFF,
          "prologue: key0=VALUE@0x00/4 dup, key1=HOLDER@0x08/8 null, key2=NAME@0x10/32");
    check(rightslist_rms_is_current(&rf), "the bound file is the current (three-key) layout");
    if (rf.ctx)
        rf.ctx->writable = 1;          /* re-bound read-only; the writes below need it */

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

    /* ---- holder records (vms-7d5a) ---- */
    {
        rdb_holder_record_t hr;
        rdb_identifier_record_t out;
        p3_rfa_t rfa[8];
        uint16_t n = 0;
        uint8_t hk[8];
        uint32_t ovmxres = 0x80010003u;

        rdb_holder_set(&hr, ovmxres, 0, 0x00010004u, 0);
        check(rightslist_put_holder(&rf, &hr) == RMS$_NORMAL, "put holder OVMXRES <- [1,4]");
        rdb_holder_set(&hr, ovmxres, 1, 0x00010007u, 0);
        check(rightslist_put_holder(&rf, &hr) == RMS$_NORMAL, "put holder OVMXRES <- [1,7] (attr 1)");
        rdb_holder_set(&hr, 0x80000003u, 0, 0x00010004u, 0);
        check(rightslist_put_holder(&rf, &hr) == RMS$_NORMAL, "put holder INTERACTIVE <- [1,4]");

        check(rightslist_get_by_value(&rf, ovmxres, &out) == RMS$_NORMAL &&
              rdb_ident_value(&out) == ovmxres,
              "by value the identifier's DEFINITION record comes first, ahead of its holders");
        check(rightslist_get_by_name(&rf, "OVMXRES", &out) == RMS$_NORMAL,
              "the name key still finds it (holder records carry no name)");

        p3_put_le32(hk, 0x00010004u); p3_put_le32(hk + 4, 0);
        check(rms_p3_sidr_lookup(rf.ctx, RIGHTSLIST_KRF_HOLDER, hk, 8, rfa, 8, &n) == RMS$_NORMAL &&
              n == 2, "HOLDER key: [1,4] holds two identifiers");
        p3_put_le32(hk, 0);
        check(rms_p3_sidr_lookup(rf.ctx, RIGHTSLIST_KRF_HOLDER, hk, 8, rfa, 8, &n) == RMS$_RNF,
              "HOLDER key: definition records (holder 0) are not indexed (null key)");

        check(rightslist_delete_holder(&rf, ovmxres, 0x00010007u, 0) == RMS$_NORMAL,
              "delete holder OVMXRES <- [1,7]");
        check(rightslist_delete_holder(&rf, ovmxres, 0x00010007u, 0) == RMS$_RNF,
              "deleting it again is RMS$_RNF");
        check(rightslist_get_by_value(&rf, ovmxres, &out) == RMS$_NORMAL,
              "the identifier record survives a holder delete");

        check(rightslist_delete_identifier(&rf, ovmxres) == RMS$_NORMAL,
              "delete identifier OVMXRES (and its holder records)");
        check(rightslist_get_by_value(&rf, ovmxres, &out) == RMS$_RNF &&
              rightslist_get_by_name(&rf, "OVMXRES", &out) == RMS$_RNF,
              "OVMXRES is gone by value and by name");
        p3_put_le32(hk, 0x00010004u); p3_put_le32(hk + 4, 0);
        check(rms_p3_sidr_lookup(rf.ctx, RIGHTSLIST_KRF_HOLDER, hk, 8, rfa, 8, &n) == RMS$_NORMAL &&
              n == 1, "[1,4] now holds only INTERACTIVE (its OVMXRES holder record went too)");
        check(rightslist_delete_identifier(&rf, ovmxres) == RMS$_RNF,
              "deleting it again is RMS$_RNF");
        {
            rdb_identifier_record_t dup;
            build_record(&g_ids[0], &dup);
            check(rightslist_put_identifier(&rf, &dup) == RMS$_DUP,
                  "a second definition record for a value is RMS$_DUP");
        }
    }

    /* ---- upgrade: a two-key file (written before the HOLDER key) copies over ---- */
    {
        char opath[] = "/tmp/ovmx_rdb_old_XXXXXX";
        int ofd = mkstemp(opath);
        rms_file_t *of = ofd >= 0 ? rms_io_posix_wrap(ofd) : NULL;
        rightslist_rms_file_t orf, crf;
        p3_create_params_t vp, np;
        rdb_identifier_record_t r, out;

        memset(&vp, 0, sizeof(vp));
        vp.key_size = 4; vp.seg0_pos = 0; vp.seg0_siz = 4; vp.bkt_blocks = 2;
        memset(&np, 0, sizeof(np));
        np.key_size = 32; np.seg0_pos = 16; np.seg0_siz = 32; np.bkt_blocks = 2;
        memset(&orf, 0, sizeof(orf));
        check(of && rms_p3_create(of, &vp, &orf.ctx) == RMS$_CREATED &&
              rms_p3_add_secondary_key(orf.ctx, &np) == RMS$_NORMAL,
              "author a two-key VALUE+NAME file (the old layout)");
        build_record(&g_ids[0], &r);
        check(orf.ctx && rms_p3_put(orf.ctx, 0, (const uint8_t *)&r, 48) == RMS$_NORMAL,
              "put one identifier into it");
        if (orf.ctx) rms_p3_free(orf.ctx);
        check(of && rightslist_rms_open(of, &orf) == RMS$_NORMAL && !rightslist_rms_is_current(&orf) &&
              orf.krf_name == 1 && orf.krf_holder == RIGHTSLIST_KRF_NONE,
              "bound: NAME is key 1, no HOLDER key -- not current");
        check(rightslist_get_by_name(&orf, g_ids[0].name, &out) == RMS$_NORMAL,
              "the old file still reads by name");
        rightslist_rms_close(&rf);
        rms_io_posix_unwrap(f);
        unlink(path);
        strcpy(path, "/tmp/ovmx_rdb_new_XXXXXX");
        fd = mkstemp(path);
        f = rms_io_posix_wrap(fd);
        check(rightslist_rms_create(f, &crf) == RMS$_CREATED &&
              rightslist_copy(&orf, &crf) == RMS$_NORMAL,
              "upgrade: every record copied into a new three-key file");
        check(rightslist_get_by_name(&crf, g_ids[0].name, &out) == RMS$_NORMAL &&
              rdb_ident_value(&out) == g_ids[0].value && rightslist_rms_is_current(&crf),
              "the upgraded file reads by name, and is current");
        rightslist_rms_close(&orf);
        rf = crf;
        if (of) rms_io_posix_unwrap(of);
        unlink(opath);
    }

    rightslist_rms_close(&rf);
    rms_io_posix_unwrap(f);
    unlink(path);

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
