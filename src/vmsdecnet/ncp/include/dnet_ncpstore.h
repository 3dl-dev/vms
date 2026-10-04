/*
 * dnet_ncpstore.h - WHERE the DECnet Phase IV configuration databases live,
 * and the one reader/writer NCP.EXE and NETACP (DECNETD.EXE) share for them
 * (rd vms-1f69; subsumes rd vms-20e).
 *
 * THE VMS FILES. On OpenVMS the DECnet Phase IV permanent databases are files
 * in SYS$SYSTEM: -- the local node (executor) database NETNODE_LOCAL.DAT, the
 * remote node database NETNODE_REMOTE.DAT, and the object database
 * NETOBJECT.DAT -- written by NCP DEFINE and read when the network starts
 * (DECnet for OpenVMS Networking Manual; @SYS$MANAGER:STARTNET gates on the
 * local node database). OVMX keeps them at exactly those VMS file
 * specifications and reaches them THROUGH THE VMS FILE LAYER: RMS over the
 * Files-11 ODS-2 ACP (rms_textfile_*, src/libvms/rtl/rms_textfile.c), so a
 * booted node's NCP SET EXECUTOR lands on its genuine system disk, and
 * STARTNET's F$SEARCH("SYS$SYSTEM:NETNODE_LOCAL.DAT") gate sees it.
 *
 * THE LAYOUT IS OVMX's (Rule 8, stated, not implied). The file NAMES are VMS's;
 * the record LAYOUT is not: VMS keeps these as binary RMS indexed files whose
 * format is not published, so OVMX stores the same documented plain-text
 * records its NCP always used (one record per entity; see dnet_nodedb.h /
 * dnet_objectdb.h and the executor record below), each file opening with a
 * comment line that says so. They are never presented as the VMS binary
 * format.
 *
 * FAIL HONEST (CLAUDE.md Rule 9 / INV-6). On the runtime there is no fallback:
 * a database that cannot be written through RMS (no executive, no mounted ACP
 * volume) is reported as a failure -- NCP prints %NCP-E-...WRERR naming the VMS
 * file -- never silently written to a Linux path instead. An ABSENT database
 * reads as empty (a fresh node has none), exactly the starting point real NCP
 * has. The pre-vms-1f69 /etc/ovmx/decnet/ Linux default is GONE.
 *
 * HOST TEST HOOK. The environment overrides OVMX_DECNET_EXECUTOR /
 * OVMX_DECNET_NODEDB / OVMX_DECNET_OBJECTDB, when set and non-empty, name a
 * host POSIX path for that one database instead (atomic temp+rename writes).
 * They exist so the NCP/DECNETD command dispatch can be unit-tested on a build
 * host with no executive; a booted VMS process is never given them.
 */
#ifndef DNET_NCPSTORE_H
#define DNET_NCPSTORE_H

#include <stdint.h>

#include "dnet_nodedb.h"
#include "dnet_objectdb.h"

#ifdef __cplusplus
extern "C" {
#endif

enum dnet_store_db {
    DNET_STORE_EXECUTOR = 0,   /* SYS$SYSTEM:NETNODE_LOCAL.DAT   */
    DNET_STORE_NODES    = 1,   /* SYS$SYSTEM:NETNODE_REMOTE.DAT  */
    DNET_STORE_OBJECTS  = 2    /* SYS$SYSTEM:NETOBJECT.DAT       */
};

/* Return codes (distinct namespace). */
#define DNET_STORE_OK      0
#define DNET_STORE_EINVAL (-1)
#define DNET_STORE_ECORRUPT (-2)  /* the database exists but does not parse      */
#define DNET_STORE_EWRITE (-3)    /* could not write it (no executive/ACP volume) */

/* The VMS file specification of database `db` (always the VMS name). */
const char *dnet_store_vms_spec(enum dnet_store_db db);
/* The host-test override environment variable for `db`. */
const char *dnet_store_env_name(enum dnet_store_db db);
/* The host override path if its env var is set, else NULL (= VMS file layer). */
const char *dnet_store_host_override(enum dnet_store_db db);
/* Where `db` actually lives for THIS process, for messages: the override path,
 * else the VMS file specification. */
const char *dnet_store_location(enum dnet_store_db db);

/* --- the executor (local node) record ------------------------------------- */
struct dnet_executor {
    int      have_addr;
    uint16_t addr;                          /* area<<10 | node */
    char     name[DNET_NODEDB_NAMEMAX + 1]; /* "" if unnamed   */
    int      state_on;
};

/* Parse one executor record line "EXECUTOR <a.n|-> NAME <name|-> STATE
 * <on|off>" (the OVMX layout). Returns 1 if the line is that record, 0 if it is
 * a blank/comment line, -1 if it is malformed. Pure. */
int dnet_executor_parse_line(const char *line, struct dnet_executor *x);
/* Format the executor record (no trailing newline). Pure. */
void dnet_executor_format(const struct dnet_executor *x, char *buf, unsigned bufsz);

/* Load/save the executor database. load: absent -> empty (have_addr 0), OK;
 * present but malformed -> ECORRUPT (and an empty record -- an address is
 * never guessed, INV-6). save: OK or EWRITE. */
int dnet_store_load_executor(struct dnet_executor *x);
int dnet_store_save_executor(const struct dnet_executor *x);

/* Load/save the remote node + object databases (same absent/corrupt rules). */
int dnet_store_load_nodes(struct dnet_nodedb *db);
int dnet_store_save_nodes(const struct dnet_nodedb *db);
int dnet_store_load_objects(struct dnet_objectdb *db);
int dnet_store_save_objects(const struct dnet_objectdb *db);

#ifdef __cplusplus
}
#endif

#endif /* DNET_NCPSTORE_H */
