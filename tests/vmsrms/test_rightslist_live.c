/*
 * test_rightslist_live.c - vms-f15a (epic vms-d0c): the RIGHTSLIST ATOMIC FLIP
 * proof. The $ASCTOID / $IDTOASC executive-context identifier resolution over
 * the genuine binary $RDBDEF RIGHTSLIST.
 *
 * Authors a real Prolog-3 indexed RIGHTSLIST (primary key = identifier VALUE,
 * secondary key = identifier NAME) with the SAME rightslist_rms engine the
 * seed (tools/mkrightslist) and the runtime use, then resolves through the
 * LIVE seam (rightslist_live_asctoid_rf / rightslist_live_idtoasc_rf -- the
 * testable core the runtime $ASCTOID/$IDTOASC entry points delegate to):
 *
 *   1. $ASCTOID name -> value for every seeded identifier (byte-exact value),
 *   2. $ASCTOID is CASE-FOLDING (lowercase name still resolves),
 *   3. $IDTOASC value -> name for every seeded identifier,
 *   4. an ABSENT name resolves to SS$_NOSUCHID (fail-honest, no fabricated 0),
 *   5. an ABSENT value resolves to SS$_NOSUCHID,
 *   6. NULL arguments -> SS$_BADPARAM.
 *
 * Reads the REAL binary $RDBDEF record through the indexed engine -- no ASCII
 * colon format, no flat scan, no /vms passthrough, no stub. Host-only: the
 * engine reads/writes blocks via rms_io_* wrapping a plain fd (host ctest has
 * no /dev/vms); the /dev/vms ACP end-to-end -- an unprivileged process
 * resolving an identifier out of the WORLD:none rights database over the
 * executive channel -- is the qemu e2e reap gate (test_syssvc_ident).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

#include "rightslist_rms.h"    /* the binary $RDBDEF indexed engine            */
#include "rightslist_live.h"   /* the $ASCTOID/$IDTOASC resolution core        */
#include "rms_io.h"            /* rms_io_posix_wrap / _unwrap                  */
#include "rmsdef.h"
#include "ssdef.h"
#include "stsdef.h"

static int g_fail = 0;
static void check(int cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
        g_fail = 1;
}

/* The six environmental identifiers the seed ships (oracle-measured). */
struct id_ent { const char *name; uint32_t value; };
static struct id_ent g_ids[] = {
    { "BATCH",       0x80000001u },
    { "DIALUP",      0x80000002u },
    { "INTERACTIVE", 0x80000003u },
    { "LOCAL",       0x80000004u },
    { "NETWORK",     0x80000005u },
    { "REMOTE",      0x80000006u },
};
#define NIDS ((int)(sizeof(g_ids) / sizeof(g_ids[0])))

static void build_record(const struct id_ent *e, rdb_identifier_record_t *r)
{
    memset(r, 0, sizeof(*r));           /* holder @0x08 stays 0 (def record) */
    rdb_ident_set_value(r, e->value);
    rdb_ident_set_attributes(r, 0);
    rdb_ident_set_name(r, e->name);
}

int main(void)
{
    char path[] = "/tmp/ovmx_rdb_live_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }

    rms_file_t *f = rms_io_posix_wrap(fd);
    if (!f) { printf("posix_wrap failed\n"); unlink(path); return 1; }

    /* ---- author the binary RIGHTSLIST (seed/runtime code path) ---- */
    rightslist_rms_file_t rf;
    uint32_t st = rightslist_rms_create(f, &rf);
    check(st == RMS$_CREATED && rf.ctx, "rightslist_rms_create (VALUE + NAME keys)");
    if (!rf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }

    for (int i = 0; i < NIDS; i++) {
        rdb_identifier_record_t r;
        build_record(&g_ids[i], &r);
        char label[64];
        snprintf(label, sizeof(label), "put %s", g_ids[i].name);
        check(rightslist_put_identifier(&rf, &r) == RMS$_NORMAL, label);
    }
    rightslist_rms_close(&rf);

    /* ---- re-bind the on-disk prologue (what the runtime opens) ---- */
    st = rightslist_rms_open(f, &rf);
    check($VMS_STATUS_SUCCESS(st) && rf.ctx, "rightslist_rms_open re-binds prologue");
    if (!rf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }

    /* ---- $ASCTOID: name -> value, for every seeded identifier ---- */
    for (int i = 0; i < NIDS; i++) {
        uint32_t v = 0;
        uint32_t s = rightslist_live_asctoid_rf(&rf, g_ids[i].name, &v);
        char label[80];
        snprintf(label, sizeof(label), "$ASCTOID %s -> 0x%08X (binary $RDBDEF)",
                 g_ids[i].name, g_ids[i].value);
        check(s == SS$_NORMAL && v == g_ids[i].value, label);
    }

    /* ---- $ASCTOID folds case (lowercase name still resolves) ---- */
    {
        uint32_t v = 0;
        uint32_t s = rightslist_live_asctoid_rf(&rf, "local", &v);
        check(s == SS$_NORMAL && v == 0x80000004u,
              "$ASCTOID case-folds (local -> LOCAL -> 0x80000004)");
    }

    /* ---- $IDTOASC: value -> name, for every seeded identifier ---- */
    for (int i = 0; i < NIDS; i++) {
        char nm[RDB$K_NAME_LEN + 1];
        memset(nm, 0xEE, sizeof(nm));
        uint32_t s = rightslist_live_idtoasc_rf(&rf, g_ids[i].value, nm, sizeof(nm));
        char label[80];
        snprintf(label, sizeof(label), "$IDTOASC 0x%08X -> \"%s\"",
                 g_ids[i].value, g_ids[i].name);
        check(s == SS$_NORMAL && strcmp(nm, g_ids[i].name) == 0, label);
    }

    /* ---- fail-honest misses (SS$_NOSUCHID, never a fabricated hit) ---- */
    {
        uint32_t v = 0xdeadbeef;
        check(rightslist_live_asctoid_rf(&rf, "NOSUCHIDENT", &v) == SS$_NOSUCHID,
              "$ASCTOID absent name -> SS$_NOSUCHID (no fabricated value)");
        char nm[RDB$K_NAME_LEN + 1] = "sentinel";
        check(rightslist_live_idtoasc_rf(&rf, 0x80009999u, nm, sizeof(nm)) == SS$_NOSUCHID,
              "$IDTOASC absent value -> SS$_NOSUCHID (no fabricated name)");
    }

    /* ---- NULL-argument guards ---- */
    {
        uint32_t v = 0;
        char nm[RDB$K_NAME_LEN + 1];
        check(rightslist_live_asctoid_rf(NULL, "LOCAL", &v) == SS$_BADPARAM,
              "$ASCTOID NULL handle -> SS$_BADPARAM");
        check(rightslist_live_asctoid_rf(&rf, NULL, &v) == SS$_BADPARAM,
              "$ASCTOID NULL name -> SS$_BADPARAM");
        check(rightslist_live_idtoasc_rf(&rf, 0x80000001u, NULL, sizeof(nm)) == SS$_BADPARAM,
              "$IDTOASC NULL buffer -> SS$_BADPARAM");
    }

    rightslist_rms_close(&rf);
    rms_io_posix_unwrap(f);
    unlink(path);

    printf("%s\n", g_fail ? "RIGHTSLIST LIVE FLIP: FAIL" : "RIGHTSLIST LIVE FLIP: ALL PASS");
    return g_fail ? 1 : 0;
}
