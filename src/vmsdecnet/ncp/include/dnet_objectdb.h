/*
 * dnet_objectdb.h - the persisted DECnet OBJECT database (rd vms-f52, child of
 * vms-67fb, epic vms-30e). The NCP object table: the DNA Session Control
 * objects this node offers to inbound connects -- each a NUMBER (the wire
 * object id, e.g. 42=CTERM, 17=FAL) plus an optional NAME and the FILE (image)
 * that services it.
 *
 * Pure table logic + a documented plain-text on-disk format; no socket, no
 * engine, unit-testable in isolation (mirrors dnet_nodedb.h exactly). The NCP
 * command grammar + SHOW layout are public (DECnet for OpenVMS Networking
 * Manual, Rule 8 clean-room); the on-disk format is an OVMX choice (labelled --
 * NOT the VMS permanent object database binary layout).
 *
 * HONEST SCOPE (INV-6): this is the object REGISTRY (number/name/file) that
 * SET/DEFINE/SHOW/CLEAR/PURGE OBJECT manages. Access-control fields (USER/
 * PROXY/PASSWORD) and the volatile-vs-permanent (SET vs DEFINE) two-database
 * split are cross-cutting follow-ons, tracked separately; this slice keeps one
 * persisted DB, exactly as the node database does today.
 */
#ifndef DNET_OBJECTDB_H
#define DNET_OBJECTDB_H

#include <stdint.h>

#define DNET_OBJECTDB_NAMEMAX  16         /* DNA object name: 1..16 chars       */
#define DNET_OBJECTDB_FILEMAX  255        /* activated image filespec           */
#define DNET_OBJECTDB_MAX      128        /* max objects held (OVMX cap)        */

#define DNET_OBJECTDB_OK        0
#define DNET_OBJECTDB_EINVAL  (-1)        /* null / bad argument                */
#define DNET_OBJECTDB_EFULL   (-2)        /* table full                         */
#define DNET_OBJECTDB_ENOENT  (-3)        /* no such object                     */
#define DNET_OBJECTDB_EIO     (-4)        /* file I/O / parse error             */

struct dnet_object_entry {
    uint8_t number;                            /* object number 1..255          */
    char    name[DNET_OBJECTDB_NAMEMAX + 1];   /* object name ("" if unnamed)   */
    char    file[DNET_OBJECTDB_FILEMAX + 1];   /* servicing image ("" if none)  */
    int     in_use;
};

struct dnet_objectdb {
    struct dnet_object_entry ent[DNET_OBJECTDB_MAX];
    unsigned count;
};

void dnet_objectdb_init(struct dnet_objectdb *db);

/* Add or update the object keyed by NUMBER. name may be "" (number-only); a
 * non-empty name must be unique (reject binding it to a second number). file
 * may be "". Returns OK / EINVAL (bad number 0, bad/duplicate name, over-long
 * file) / EFULL. */
int dnet_objectdb_set(struct dnet_objectdb *db, uint8_t number,
                      const char *name, const char *file);

int dnet_objectdb_clear_number(struct dnet_objectdb *db, uint8_t number);
int dnet_objectdb_clear_name(struct dnet_objectdb *db, const char *name);

const struct dnet_object_entry *dnet_objectdb_by_number(const struct dnet_objectdb *db,
                                                        uint8_t number);
const struct dnet_object_entry *dnet_objectdb_by_name(const struct dnet_objectdb *db,
                                                      const char *name);

/* The i-th in-use object in ascending-NUMBER order (SHOW ordering). */
const struct dnet_object_entry *dnet_objectdb_at(const struct dnet_objectdb *db,
                                                 unsigned i);

int dnet_objectdb_save(const struct dnet_objectdb *db, const char *path);
int dnet_objectdb_load(struct dnet_objectdb *db, const char *path);

#endif /* DNET_OBJECTDB_H */
