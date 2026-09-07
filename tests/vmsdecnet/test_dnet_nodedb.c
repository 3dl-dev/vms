/*
 * test_dnet_nodedb.c - DECnet Phase IV node database unit tests (rd vms-1e9).
 *
 * Covers address parsing + range checks, DEFINE/SET NODE add + update, node-
 * name uniqueness, name<->address resolution (case-insensitive), CLEAR/PURGE,
 * SHOW ordering (ascending address), and persistence round-trip / corrupt-file
 * handling. Pure logic + a temp file; no socket, no privilege.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dnet_nodedb.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}

int main(void)
{
    printf("test_dnet_nodedb: DECnet Phase IV node database\n");

    /* --- address parsing --- */
    uint16_t a = 0;
    check(dnet_nodedb_parse_addr("1.1", &a) == DNET_NODEDB_OK && a == ((1u << 10) | 1u),
          "parse 1.1");
    check(dnet_nodedb_parse_addr("1.42", &a) == DNET_NODEDB_OK && a == ((1u << 10) | 42u),
          "parse 1.42");
    check(dnet_nodedb_parse_addr("63.1023", &a) == DNET_NODEDB_OK, "parse max 63.1023");
    check(dnet_nodedb_parse_addr("0.1", &a) == DNET_NODEDB_EINVAL, "reject area 0");
    check(dnet_nodedb_parse_addr("64.1", &a) == DNET_NODEDB_EINVAL, "reject area 64");
    check(dnet_nodedb_parse_addr("1.1024", &a) == DNET_NODEDB_EINVAL, "reject node 1024");
    check(dnet_nodedb_parse_addr("1", &a) == DNET_NODEDB_EINVAL, "reject bare 1");
    check(dnet_nodedb_parse_addr("1.x", &a) == DNET_NODEDB_EINVAL, "reject 1.x");
    check(dnet_nodedb_parse_addr("1.2.3", &a) == DNET_NODEDB_EINVAL, "reject 1.2.3");

    /* --- set / update / uniqueness --- */
    struct dnet_nodedb db;
    dnet_nodedb_init(&db);
    uint16_t vax1 = (1u << 10) | 1u, ovmx = (1u << 10) | 42u, vax2 = (1u << 10) | 2u;
    check(dnet_nodedb_set(&db, vax1, "VAX1") == DNET_NODEDB_OK, "SET NODE 1.1 NAME VAX1");
    check(dnet_nodedb_set(&db, ovmx, "OVMX") == DNET_NODEDB_OK, "SET NODE 1.42 NAME OVMX");
    check(dnet_nodedb_set(&db, vax2, "") == DNET_NODEDB_OK, "SET NODE 1.2 (no name)");
    check(db.count == 3, "count == 3");
    /* Re-SET an existing address updates its name (no dup). */
    check(dnet_nodedb_set(&db, vax2, "VAX2") == DNET_NODEDB_OK, "update 1.2 -> NAME VAX2");
    check(db.count == 3, "count still 3 after update");
    /* A name already bound to a different address is rejected. */
    check(dnet_nodedb_set(&db, (1u << 10) | 3u, "VAX1") == DNET_NODEDB_EINVAL,
          "reject VAX1 on a second address");
    /* Bad names. */
    check(dnet_nodedb_set(&db, (1u << 10) | 4u, "TOOLONGNAME") == DNET_NODEDB_EINVAL,
          "reject 7+ char name");
    check(dnet_nodedb_set(&db, (1u << 10) | 4u, "BAD-NM") == DNET_NODEDB_EINVAL,
          "reject non-alnum name");

    /* --- resolution (case-insensitive) --- */
    const struct dnet_node_entry *e = dnet_nodedb_by_name(&db, "vax1");
    check(e && e->addr == vax1, "resolve name 'vax1' (case-insensitive) -> 1.1");
    e = dnet_nodedb_by_name(&db, "OVMX");
    check(e && e->addr == ovmx, "resolve name 'OVMX' -> 1.42");
    e = dnet_nodedb_by_addr(&db, vax2);
    check(e && strcmp(e->name, "VAX2") == 0, "resolve addr 1.2 -> VAX2");
    check(dnet_nodedb_by_name(&db, "NOPE") == NULL, "unknown name -> NULL");

    /* --- SHOW ordering (ascending address) --- */
    const struct dnet_node_entry *e0 = dnet_nodedb_at(&db, 0);
    const struct dnet_node_entry *e1 = dnet_nodedb_at(&db, 1);
    const struct dnet_node_entry *e2 = dnet_nodedb_at(&db, 2);
    check(e0 && e1 && e2 && e0->addr < e1->addr && e1->addr < e2->addr,
          "dnet_nodedb_at iterates in ascending address order");
    check(e0->addr == vax1 && e1->addr == vax2 && e2->addr == ovmx, "order 1.1 < 1.2 < 1.42");
    check(dnet_nodedb_at(&db, 3) == NULL, "at(count) == NULL");

    /* --- persistence round-trip --- */
    char path[] = "/tmp/ovmx_nodedb_test_XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "temp DB path created");
    if (fd >= 0)
        close(fd);
    check(dnet_nodedb_save(&db, path) == DNET_NODEDB_OK, "save DB");
    struct dnet_nodedb db2;
    check(dnet_nodedb_load(&db2, path) == DNET_NODEDB_OK, "load DB");
    check(db2.count == 3, "reloaded count == 3");
    e = dnet_nodedb_by_name(&db2, "VAX1");
    check(e && e->addr == vax1, "reloaded resolves VAX1 -> 1.1");
    e = dnet_nodedb_by_addr(&db2, vax2);
    check(e && strcmp(e->name, "VAX2") == 0, "reloaded 1.2 name VAX2 persisted");

    /* --- clear --- */
    check(dnet_nodedb_clear_name(&db2, "VAX1") == DNET_NODEDB_OK, "CLEAR NODE VAX1");
    check(dnet_nodedb_by_addr(&db2, vax1) == NULL, "1.1 gone after clear");
    check(db2.count == 2, "count 2 after clear");
    check(dnet_nodedb_clear_addr(&db2, ovmx) == DNET_NODEDB_OK, "CLEAR NODE 1.42");
    check(dnet_nodedb_clear_addr(&db2, ovmx) == DNET_NODEDB_ENOENT, "clear again -> ENOENT");

    /* --- missing file == empty (not an error); corrupt file == EIO --- */
    struct dnet_nodedb db3;
    check(dnet_nodedb_load(&db3, "/tmp/ovmx_nodedb_does_not_exist_XYZ") == DNET_NODEDB_OK
          && db3.count == 0, "missing DB loads as empty");
    FILE *bad = fopen(path, "w");
    if (bad) { fprintf(bad, "GARBAGE not a node line\n"); fclose(bad); }
    check(dnet_nodedb_load(&db3, path) == DNET_NODEDB_EIO, "corrupt DB -> EIO");
    remove(path);

    if (failures == 0) { printf("test_dnet_nodedb: ALL CHECKS PASSED\n"); return 0; }
    printf("test_dnet_nodedb: %d CHECK(S) FAILED\n", failures);
    return 1;
}
