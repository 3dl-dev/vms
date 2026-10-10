/*
 * sysgen_factory.h - THE SYSGEN FACTORY PARAMETER TABLE, in one place.
 *
 * WHY THIS HEADER EXISTS (rd vms-025, the 2026-10-09 lab finding)
 * ---------------------------------------------------------------
 * On a real-VAX mixed-cluster lab run the operator typed, at the OVMX node's
 * conversational boot:
 *
 *     SYSBOOT> SET LOCKDIRWT 1
 *     %SYSGEN-E-NOSUCHP, no such parameter "LOCKDIRWT"
 *
 * ...and the whole interim sole-directory configuration the mixed-cluster DLM
 * arm stands behind (vms_ldwv_sole_directory) was therefore UNCONFIGURABLE on a
 * booted node. The cause was not the cluster code: SYSBOOT's SET looked the
 * name up in the PARAMETER FILE it had just loaded, and the shipped seed
 * SYS$SYSTEM:OVMXVMSSYS.PAR was authored before LOCKDIRWT existed -- 31 rows,
 * none of them LOCKDIRWT, QDSKVOTES, TIMVCFAIL, CLUSTER_CREDITS,
 * NISCS_MAX_PKTSZ, MSCP_LOAD, MSCP_SERVE_ALL or DISK_QUORUM. Every one of those
 * is a parameter load_cluster_sysgen_params() READS at boot, so each was
 * silently unsettable on every disk written before it was added.
 *
 * sysgen_params.h already states the rule this fixes, twice: "a parameter the
 * system knows but the file has never held takes the system's default, exactly
 * as SYSBOOT's parameter table supplies one on VMS." A stored .PAR carries
 * VALUES; the PARAMETER TABLE -- what the system knows -- belongs to the
 * system, and on VMS SYSBOOT has it built in. So:
 *
 *   - this header holds that table, ONCE (INV-LEDGER: it used to be a private
 *     array in tools/vms_sysgen.c, which is why SYSBOOT could not see it);
 *   - SYSGEN.EXE (tools/vms_sysgen.c) and SYSBOOT (src/ovmx_init/sysboot.c)
 *     both build their factory set from it;
 *   - sysgen_factory_merge() UNIONS the rows a loaded file lacks into the
 *     working set, so a parameter the system knows is SHOWable, SETtable and
 *     persisted by WRITE even on a .PAR written before it existed.
 *
 * The merged row carries the FACTORY value, which is also what the boot-time
 * reader falls back to when the store has no record -- so a merge never changes
 * the running configuration, it only makes the knob reachable.
 *
 * The values themselves are NOT re-derived here: they are the rows that were in
 * tools/vms_sysgen.c, with their grounding comments, moved verbatim.
 */
#ifndef SYSGEN_FACTORY_H
#define SYSGEN_FACTORY_H

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "sysgen_params.h"

/* ================================================================== */
/*                    Factory Parameter Table                          */
/* ================================================================== */

static const struct sysgen_param ovmx_sysgen_factory_params[] = {
    {"MAXPROCESSCNT",   64,      64,      4,       1024,     0,
     "Maximum number of concurrent processes", SYSGEN_TYPE_NUMERIC, "", ""},
    {"CHANNELCNT",      16,      16,      4,       256,      SYSGEN_F_DYNAMIC,
     "Number of I/O channels per process", SYSGEN_TYPE_NUMERIC, "", ""},
    {"DEFPRI",          4,       4,       0,       31,       SYSGEN_F_DYNAMIC,
     "Default process priority", SYSGEN_TYPE_NUMERIC, "", ""},
    {"MAXPRI",          31,      31,      0,       31,       0,
     "Maximum process priority", SYSGEN_TYPE_NUMERIC, "", ""},
    {"MAXBUF",          8192,    8192,    512,     65536,    SYSGEN_F_DYNAMIC,
     "Maximum buffered I/O byte count", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DWSDEFAULT",  256,     256,     64,      65536,    SYSGEN_F_DYNAMIC,
     "Default working set size", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DWSQUOTA",    512,     512,     64,      65536,    SYSGEN_F_DYNAMIC,
     "Working set quota", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DWSEXTENT",   2048,    2048,    64,      262144,   SYSGEN_F_DYNAMIC,
     "Working set extent", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DENQLM",      200,     200,     4,       32767,    SYSGEN_F_DYNAMIC,
     "Default enqueue limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DFILLM",      100,     100,     4,       8192,     SYSGEN_F_DYNAMIC,
     "Default open file limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DTQELM",      20,      20,      1,       1024,     SYSGEN_F_DYNAMIC,
     "Default timer queue entry limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DBIOLM",      40,      40,      4,       4096,     SYSGEN_F_DYNAMIC,
     "Default buffered I/O limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DDIOLM",      40,      40,      4,       4096,     SYSGEN_F_DYNAMIC,
     "Default direct I/O limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DBYTLM",      65536,   65536,   1024,    16777216, SYSGEN_F_DYNAMIC,
     "Default buffered I/O byte limit", SYSGEN_TYPE_NUMERIC, "", ""},
    {"PQL_DPGFLQUOTA",  50000,   50000,   1024,    4194304,  SYSGEN_F_DYNAMIC,
     "Default page file quota", SYSGEN_TYPE_NUMERIC, "", ""},
    {"VIRTUALPAGECNT",  1048576, 1048576, 1024,    67108864, 0,
     "Virtual page count", SYSGEN_TYPE_NUMERIC, "", ""},
    {"GBLPAGES",        8192,    8192,    256,     4194304,  SYSGEN_F_DYNAMIC,
     "Global pages", SYSGEN_TYPE_NUMERIC, "", ""},
    {"GBLSECTIONS",     256,     256,     16,      4096,     SYSGEN_F_DYNAMIC,
     "Global sections", SYSGEN_TYPE_NUMERIC, "", ""},
    {"LNMPHASHTBL",     128,     128,     16,      8192,     SYSGEN_F_DYNAMIC,
     "Logical name hash table size", SYSGEN_TYPE_NUMERIC, "", ""},
    {"ACP_MAPCACHE",    32,      32,      4,       256,      SYSGEN_F_DYNAMIC,
     "ACP map cache size", SYSGEN_TYPE_NUMERIC, "", ""},
    {"BALSETCNT",       16,      16,      4,       256,      0,
     "Maximum number of processes in balance set", SYSGEN_TYPE_NUMERIC, "", ""},
    {"IRPCOUNT",        256,     256,     32,      4096,     0,
     "Number of I/O request packets", SYSGEN_TYPE_NUMERIC, "", ""},
    {"SRPCOUNT",        256,     256,     32,      4096,     0,
     "Number of small request packets", SYSGEN_TYPE_NUMERIC, "", ""},
    {"LRPCOUNT",        32,      32,      4,       512,      0,
     "Number of large request packets", SYSGEN_TYPE_NUMERIC, "", ""},

    /* --- vms-ci.8: cluster node-identity parameters ---
     * OVMX-defined defaults (NOT VMS-authentic values) — see item vms-ci.8.
     * SCSSYSTEMID/ALLOCLASS/VOTES/EXPECTED_VOTES/VAXCLUSTER mirror the real
     * VMS SYSGEN parameter names; SCSNODE is the only string-typed param. */
    { .name = "SCSSYSTEMID", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 65535, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster system ID (OVMX default 0)",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "ALLOCLASS", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 255, .flags = SYSGEN_F_DYNAMIC,
      .description = "Allocation class for shared cluster devices",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "VOTES", .current = 1, .default_val = 1,
      .min_val = 0, .max_val = 32767, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster quorum votes contributed by this node",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "EXPECTED_VOTES", .current = 1, .default_val = 1,
      .min_val = 1, .max_val = 32767, .flags = SYSGEN_F_DYNAMIC,
      .description = "Expected total cluster quorum votes",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "VAXCLUSTER", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 2, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster participation (0=disabled,1=enabled,2=auto)",
      .type = SYSGEN_TYPE_NUMERIC },
    /* --- vms-c3b: RECNXINTERVAL, the cluster reconnection interval ---
     * GROUNDED (CLAUDE.md Rule 8) from PUBLIC OpenVMS docs, NOT VSI source:
     * the VSI/HPE OpenVMS System Management Utilities Reference Manual (SYSGEN
     * Parameters) documents RECNXINTERVAL as the polling interval, in seconds,
     * during which the OpenVMS Cluster software attempts to restore a lost
     * connection -- default 20, and (Appendix J, "System Parameters by
     * Category") a CLUSTER parameter marked Dynamic. Its documented SYSGEN
     * range is minimum 1, maximum 32767 seconds. The default 20 matches
     * scs_recnx.h's SCS_RECNX_DEFAULT_RECNXINTERVAL (the runtime reconnect
     * loop's fallback, vms-c7d), so an unconfigured store and the runtime
     * agree. Authored here so src/ovmx_init/ovmx_init.c's sysgen_read_param()
     * call picks up the operator's value on (re)boot the same way it reads
     * SCSNODE/SCSSYSTEMID/ALLOCLASS, feeding VMS_IOCTL_SYSGEN_LOAD and, from
     * there, vms_cnxman_recnx_fsm.c; this is the AUTHORING surface only -- the
     * reconnect wire behavior is vms-694's (scs_recnx.c), unchanged. */
    { .name = "RECNXINTERVAL", .current = 20, .default_val = 20,
      .min_val = 1, .max_val = 32767, .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster reconnection interval, in seconds",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "SCSNODE", .flags = SYSGEN_F_DYNAMIC,
      .description = "Cluster node name (SCS system name, max 6 chars)",
      .type = SYSGEN_TYPE_STRING,
      .str_current = "OVMX", .str_default = "OVMX" },

    /* --- FC-P0.10: the remaining VMS_IOCTL_SYSGEN_LOAD parameters --- */

    /* LOCKDIRWT: OVMX's OWN default is 0, not a VMS-published number --
     * design D-DLM-1 (docs/design-faithful-cluster-executive.md): "join with
     * LOCKDIRWT=0 and advertise it honestly" is the smallest faithful
     * footprint (never a lock directory node unless the operator opts in). */
    { .name = "LOCKDIRWT", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 255, .flags = SYSGEN_F_DYNAMIC,
      .description = "Lock directory weight (0 = never a directory node, D-DLM-1)",
      .type = SYSGEN_TYPE_NUMERIC },
    /* QDSKVOTES: 0 matches the DISK_QUORUM default below (no quorum disk
     * configured -- it contributes no votes until one is). OVMX-defined. */
    { .name = "QDSKVOTES", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 32767, .flags = SYSGEN_F_DYNAMIC,
      .description = "Quorum disk votes (0 = no quorum disk configured)",
      .type = SYSGEN_TYPE_NUMERIC },
    /* CLUSTER_CREDITS: OVMX's OWN default for a port whose SYSGEN value has
     * not been loaded -- vms_pe_fsm.h's "CLUSTER_CREDITS, 10 in the lab". Not
     * a published VMS constant; disclosed here as an OVMX choice matching the
     * lab's own value so an unconfigured store and a captured configuration
     * agree. */
    /* TIMVCFAIL: GROUNDED (rd vms-b98) by SYSGEN SHOW TIMVCFAIL on a real
     * OpenVMS VAX V7.3, which prints current 1600, default 1600, min 100,
     * max 65535, unit "10Ms", dynamic. The executive converts the 10 ms unit
     * (cluster_sysgen_timvcfail_ms) and the port runs on the loaded value. */
    { .name = "TIMVCFAIL", .current = 1600, .default_val = 1600,
      .min_val = 100, .max_val = 65535, .flags = SYSGEN_F_DYNAMIC,
      .description = "Virtual circuit failure detection time, 10 ms units",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "CLUSTER_CREDITS",
      .current = SYSGEN_DEFAULT_CLUSTER_CREDITS,
      .default_val = SYSGEN_DEFAULT_CLUSTER_CREDITS,
      .min_val = 1, .max_val = 65535, .flags = SYSGEN_F_DYNAMIC,
      .description = "Per-circuit send credit (OVMX default; matches the lab capture)",
      .type = SYSGEN_TYPE_NUMERIC },
    /* NISCS_MAX_PKTSZ: 1498, GROUNDED in the cluster protocol spec sec 4(k)
     * (src/kernel-core/vms_cluster_codec_hello.h's VMS_HELLO_PADDED_MAX_SCA
     * comment: "NISCS_MAX_PKTSZ 1498+2"). Clamped to the interface MTU by
     * the port at CLUSTER_START; this is the SYSGEN ceiling only. */
    { .name = "NISCS_MAX_PKTSZ", .current = 1498, .default_val = 1498,
      .min_val = 512, .max_val = 65535, .flags = SYSGEN_F_DYNAMIC,
      .description = "Maximum NISCA packet size, bytes (spec sec 4(k))",
      .type = SYSGEN_TYPE_NUMERIC },
    /* MSCP_LOAD / MSCP_SERVE_ALL: published OpenVMS SYSGEN defaults (VSI/HPE
     * OpenVMS System Management Utilities Reference Manual, SYSGEN
     * Parameters) -- MSCP_LOAD defaults to load the MSCP server, MSCP_
     * SERVE_ALL defaults to NOT serving every disk automatically. */
    { .name = "MSCP_LOAD", .current = 1, .default_val = 1,
      .min_val = 0, .max_val = 1, .flags = SYSGEN_F_DYNAMIC,
      .description = "Load the MSCP server (published OpenVMS default: enabled)",
      .type = SYSGEN_TYPE_NUMERIC },
    { .name = "MSCP_SERVE_ALL", .current = 0, .default_val = 0,
      .min_val = 0, .max_val = 1, .flags = SYSGEN_F_DYNAMIC,
      .description = "Serve every disk via MSCP (published OpenVMS default: disabled)",
      .type = SYSGEN_TYPE_NUMERIC },
    /* DISK_QUORUM: the quorum disk's device name, empty = none configured.
     * String-typed like SCSNODE; NOTE the shared store's SYSGEN_STRVAL_LEN
     * (8 bytes) is sized for SCSNODE-class strings and can TRUNCATE a longer
     * real device name (e.g. "$102$DGA1023") -- disclosed here, not silently
     * widened: FC-P0.10 wires the load path, not a store-format change. */
    { .name = "DISK_QUORUM", .flags = SYSGEN_F_DYNAMIC,
      .description = "Quorum disk device name (\"\" = none configured)",
      .type = SYSGEN_TYPE_STRING,
      .str_current = "", .str_default = "" },

    /* OVMX_CLEAN_DEPART (rd vms-abd): the clean-departure kill switch the
     * boot ALSO reads (ovmx_init.c's clean_depart_requested()). It was the
     * ONE loader-read parameter still missing from this table, found by
     * tests/ovmx_init/test_sysgen_factory.c's scan of the loader's own text --
     * which is the same hole LOCKDIRWT was in (rd vms-025): the switch worked
     * at its default and no operator could ever turn it OFF, because SYSBOOT
     * and SYSGEN had no row to SET. OVMX's OWN parameter, disclosed by the
     * OVMX_ prefix (CLAUDE.md Rule 8); the default is the sysgen_params.h
     * SSOT, so the table and the reader's absent-record fallback agree. Not
     * dynamic: it is consulted when this node leaves, off the value the boot
     * loaded. */
    { .name = "OVMX_CLEAN_DEPART",
      .current = SYSGEN_DEFAULT_OVMX_CLEAN_DEPART,
      .default_val = SYSGEN_DEFAULT_OVMX_CLEAN_DEPART,
      .min_val = 0, .max_val = 1, .flags = 0,
      .description = "Announce this node's cluster departure at SCS (OVMX)",
      .type = SYSGEN_TYPE_NUMERIC },
};

#define OVMX_SYSGEN_FACTORY_COUNT                                           \
    ((uint32_t)(sizeof(ovmx_sysgen_factory_params) /                        \
                sizeof(ovmx_sysgen_factory_params[0])))

/* The factory row for `name`, or NULL when this system does not know it. */
static inline const struct sysgen_param *sysgen_factory_find(const char *name)
{
    if (name == NULL)
        return NULL;
    for (uint32_t i = 0; i < OVMX_SYSGEN_FACTORY_COUNT; i++) {
        if (strcasecmp(ovmx_sysgen_factory_params[i].name, name) == 0)
            return &ovmx_sysgen_factory_params[i];
    }
    return NULL;
}

/* Does `ws` already carry a row for `name`? */
static inline int sysgen_file_has(const struct sysgen_file *ws, const char *name)
{
    uint32_t n = (ws->count > SYSGEN_MAX_PARAMS) ? SYSGEN_MAX_PARAMS : ws->count;

    for (uint32_t i = 0; i < n; i++) {
        if (strcasecmp(ws->params[i].name, name) == 0)
            return 1;
    }
    return 0;
}

/* Load the complete factory set into `ws` (SYSGEN's USE DEFAULT, SYSBOOT's
 * no-parameter-file path). */
static inline void sysgen_factory_load(struct sysgen_file *ws)
{
    uint32_t n = OVMX_SYSGEN_FACTORY_COUNT;

    memset(ws, 0, sizeof(*ws));
    ws->magic   = SYSGEN_MAGIC;
    ws->version = SYSGEN_VERSION;
    if (n > SYSGEN_MAX_PARAMS)
        n = SYSGEN_MAX_PARAMS;
    ws->count = n;
    for (uint32_t i = 0; i < n; i++)
        ws->params[i] = ovmx_sysgen_factory_params[i];
}

/*
 * UNION the factory rows `ws` lacks into it, at their FACTORY values.
 *
 * This is the "SYSBOOT's parameter table supplies one" half of the rule: a
 * parameter THIS SYSTEM KNOWS is in the working set even when the stored file
 * -- written by an older system -- has never held a record for it. Rows the
 * file DOES carry are untouched: the operator's stored value always wins over
 * the factory default, which is the whole point of the file.
 *
 * Returns the number of rows appended. A table already at SYSGEN_MAX_PARAMS
 * appends nothing and says so by returning less than it would have: that is a
 * bounded store refusing to overflow, not a parameter being hidden, and the
 * caller's own count is how an operator sees it.
 */
static inline uint32_t sysgen_factory_merge(struct sysgen_file *ws)
{
    uint32_t added = 0;

    if (ws == NULL)
        return 0;
    if (ws->count > SYSGEN_MAX_PARAMS)
        ws->count = SYSGEN_MAX_PARAMS;
    for (uint32_t i = 0; i < OVMX_SYSGEN_FACTORY_COUNT; i++) {
        const struct sysgen_param *f = &ovmx_sysgen_factory_params[i];

        if (sysgen_file_has(ws, f->name))
            continue;
        if (ws->count >= SYSGEN_MAX_PARAMS)
            break;
        ws->params[ws->count++] = *f;
        added++;
    }
    return added;
}

#endif /* SYSGEN_FACTORY_H */
