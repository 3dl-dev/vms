/*
 * dnet_nodedb.h - DECnet Phase IV node database (the NCP node table).
 *
 * The persisted "known nodes" table an NCP user builds with DEFINE/SET NODE and
 * reads with SHOW KNOWN NODES / SHOW NODE, plus the name<->address resolution
 * the SET HOST / NODE:: filespec paths need to turn a node NAME into the
 * area.node address the routing/NSP layers address. Pure logic + a simple
 * documented on-disk format; no socket, no engine, no allocation beyond the
 * fixed table -- unit-testable in isolation (rd vms-1e9, epic vms-30e).
 *
 * PROVENANCE (Rule 8): the NCP command grammar and the SHOW output layout are
 * public (VSI/DEC DECnet for OpenVMS Networking Manual, NCP chapter). The
 * on-disk file format below is an OVMX implementation choice (a documented
 * plain-text record per node), NOT a VMS-authentic NETNODE_REMOTE.DAT binary
 * layout -- labelled as such, never presented as the VMS on-disk format.
 */
#ifndef DNET_NODEDB_H
#define DNET_NODEDB_H

#include <stddef.h>
#include <stdint.h>

#include "dnet_hello.h"   /* dnet_area_of/dnet_node_of + DNET_ADDR_LEN */

#ifdef __cplusplus
extern "C" {
#endif

#define DNET_NODEDB_NAMEMAX  6            /* NCP node name: 1..6 chars (DNA) */
#define DNET_NODEDB_MAX      256          /* max remote nodes held (OVMX cap) */

/* Return codes (distinct namespace). */
#define DNET_NODEDB_OK        0
#define DNET_NODEDB_EINVAL  (-1)          /* null / bad argument */
#define DNET_NODEDB_EFULL   (-2)          /* table full */
#define DNET_NODEDB_ENOENT  (-3)          /* no such node */
#define DNET_NODEDB_EIO     (-4)          /* file I/O / parse error */

struct dnet_node_entry {
    uint16_t addr;                        /* DECnet address (area<<10 | node) */
    char     name[DNET_NODEDB_NAMEMAX + 1]; /* NCP node name ("" if unnamed) */
    int      in_use;
};

struct dnet_nodedb {
    struct dnet_node_entry ent[DNET_NODEDB_MAX];
    unsigned count;
};

/* Parse "area.node" (1..63 . 1..1023) into a 16-bit DECnet address. Returns
 * DNET_NODEDB_OK, or DNET_NODEDB_EINVAL on a malformed / out-of-range value. */
int dnet_nodedb_parse_addr(const char *s, uint16_t *addr_out);

void dnet_nodedb_init(struct dnet_nodedb *db);

/* DEFINE/SET NODE: add the node, or update the name of an existing address.
 * A name may be "" (address only). A name already bound to a DIFFERENT address
 * is rejected (DNET_NODEDB_EINVAL) -- NCP node names are unique. */
int dnet_nodedb_set(struct dnet_nodedb *db, uint16_t addr, const char *name);

/* CLEAR/PURGE NODE by address or by name. ENOENT if absent. */
int dnet_nodedb_clear_addr(struct dnet_nodedb *db, uint16_t addr);
int dnet_nodedb_clear_name(struct dnet_nodedb *db, const char *name);

/* Resolution (what SET HOST / NODE:: need). NULL if not found. */
const struct dnet_node_entry *dnet_nodedb_by_name(const struct dnet_nodedb *db,
                                                  const char *name);
const struct dnet_node_entry *dnet_nodedb_by_addr(const struct dnet_nodedb *db,
                                                  uint16_t addr);

/* Ordered iteration for SHOW KNOWN NODES (by ascending address). Returns the
 * i-th in-use entry (0-based) or NULL when i >= count. */
const struct dnet_node_entry *dnet_nodedb_at(const struct dnet_nodedb *db, unsigned i);

/* Persist / restore the whole table to a plain-text file (OVMX format). save
 * writes atomically (temp + rename). load tolerates a missing file (empties the
 * table, returns OK) but fails EIO on a corrupt one. */
int dnet_nodedb_save(const struct dnet_nodedb *db, const char *path);
int dnet_nodedb_load(struct dnet_nodedb *db, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DNET_NODEDB_H */
