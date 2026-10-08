/*
 * dnet_netshow.h - the NETACP volatile-database SNAPSHOT record behind NCP SHOW
 *                  and DCL SHOW NETWORK (rd vms-30e).
 *
 * On VMS, NCP and SHOW NETWORK read the RUNNING network: they $ASSIGN _NET: and
 * issue $QIO IO$_ACPCONTROL, and NETACP answers from its volatile database. OVMX
 * does the same over the T1 broker (dnet_broker.h): a _NET: IO$_ACPCONTROL is
 * brokered to NETACP as DNET_BROKER_OP_SHOW, and NETACP answers with ONE bounded,
 * versioned snapshot record read from its live state -- its executor engine, its
 * adjacency table, its logical-link pool, the node database it resolves names
 * against, and the counters it actually keeps. This header is that record and the
 * NCP-layout formatters that print it.
 *
 * HONEST SCOPE (INV-6). The record carries ONLY what NETACP really holds. NCP
 * fields a real executor has and OVMX does not (delay factor/weight, routing and
 * broadcast-routing timers, maximum address/cost/hops/visits/area, pipeline quota,
 * buffer size, alias, path split, the routing-loss counters, ...) are NOT in the
 * record and are NOT printed -- omitted, never filled with a plausible constant.
 *
 * CLEAN-ROOM (Rule 8). The record is an OVMX-internal transport between two OVMX
 * components, not the DNA NICE / NFB field-id encoding (that real NFB layout is a
 * follow-up). The PRINTED layout -- headers, the "Executor node = a.n (NAME)"
 * line, the "Name = value" column at 25, the KNOWN NODES table columns, the
 * "No information in database" line, the SHOW NETWORK product line -- is copied
 * from OBSERVED output of a real OpenVMS VAX V7.3 (docs/oracle/vax-ncp-show/).
 *
 * BOUNDS (the broker's A2/A8 discipline). The record arrives from the other side
 * of a mailbox and is untrusted: dnet_netshow_rsp_decode gates the version and
 * entity, bounds every count and string length against the fixed layout, checks
 * the total length before reading a byte, rejects non-printable string bytes (a
 * snapshot can never smuggle terminal escapes into NCP's output), and zeroes *out
 * first so a refused record leaves nothing half-filled.
 *
 * Linkage: DNET_NETSHOW_API, like DNET_BROKER_API -- external for the NETACP and
 * NCP builds, `static` for a consumer (DCL) that compiles a private copy of the
 * SAME source (one record format, no new LIBVMS$SHR universal).
 */
#ifndef DNET_NETSHOW_H
#define DNET_NETSHOW_H

#include <stddef.h>
#include <stdint.h>

#ifndef DNET_NETSHOW_API
#define DNET_NETSHOW_API
#endif

#define DNET_NETSHOW_OK        0
#define DNET_NETSHOW_ETRUNC  (-1)   /* shorter than its declared content           */
#define DNET_NETSHOW_EBADLEN (-2)   /* a count / string length over its bound      */
#define DNET_NETSHOW_EVERS   (-3)   /* unknown record version                       */
#define DNET_NETSHOW_EINVAL  (-4)   /* null argument / unknown entity / bad byte    */
#define DNET_NETSHOW_ENOSPACE (-5)  /* output buffer too small                       */

/* 2: adds the CIRCUITS / LINES / OBJECTS entities (rd vms-2d0). */
#define DNET_NETSHOW_VERSION   2u

/* What is being asked for (the NCP entity). */
#define DNET_NETSHOW_ENT_EXECUTOR 1u
#define DNET_NETSHOW_ENT_NODES    2u
#define DNET_NETSHOW_ENT_LINKS    3u
#define DNET_NETSHOW_ENT_CIRCUITS 4u   /* NETACP's circuit (its datalink)        */
#define DNET_NETSHOW_ENT_LINES    5u   /* the line under that circuit            */
#define DNET_NETSHOW_ENT_OBJECTS  6u   /* the objects NETACP serves (volatile DB)*/

/* Field bounds (fixed-width slots in the record). */
#define DNET_NETSHOW_NAMEMAX   6    /* DNA Phase IV node name                     */
#define DNET_NETSHOW_IDENTMAX  32   /* executor Identification                     */
#define DNET_NETSHOW_CIRCMAX   15   /* circuit name (DNET_DEVNAME_MAX)              */
#define DNET_NETSHOW_OBJNAMEMAX 16  /* DNA object name (DNET_OBJECTDB_NAMEMAX)      */
#define DNET_NETSHOW_FILEMAX   39   /* object image file as NCP prints it           */

/* Entries per record (a page); the client walks pages by cursor. Chosen so a
 * full page fits DNET_NSP_MAX_DATA (1024) with room to spare. */
#define DNET_NETSHOW_NODES_PER 48
#define DNET_NETSHOW_LINKS_PER 32
#define DNET_NETSHOW_CIRCS_PER 8
#define DNET_NETSHOW_LINES_PER 8
#define DNET_NETSHOW_OBJS_PER  12

/* Wire sizes. */
#define DNET_NETSHOW_REQ_LEN   4u   /* version, entity, cursor(2)                  */
#define DNET_NETSHOW_HDR_LEN   12u  /* version entity total(2) first(2) count flags as_of(4) */
#define DNET_NETSHOW_EXEC_LEN  75u
#define DNET_NETSHOW_NODE_LEN  15u
#define DNET_NETSHOW_LINK_LEN  20u
#define DNET_NETSHOW_CIRC_LEN  26u  /* name(1+15) state(1) adj addr(2) adj name(1+6) */
#define DNET_NETSHOW_LINE_LEN  17u  /* name(1+15) state(1)                           */
#define DNET_NETSHOW_OBJ_LEN   62u  /* name(1+16) number(1) file(1+39) pid(4)        */
/* The largest page of any entity (12 objects = 744 > 48 nodes = 720 bytes). */
#define DNET_NETSHOW_PAGE_MAX  (DNET_NETSHOW_OBJS_PER * DNET_NETSHOW_OBJ_LEN)
#define DNET_NETSHOW_RSP_MAX   (DNET_NETSHOW_HDR_LEN + DNET_NETSHOW_EXEC_LEN + \
                                DNET_NETSHOW_PAGE_MAX)

/* Node-entry flags. */
#define DNET_NETSHOW_NF_INDB     0x01u  /* in the node database NETACP resolves by */
#define DNET_NETSHOW_NF_ADJ      0x02u  /* in NETACP's adjacency table              */

/* Adjacency state of a node entry (DNET_NETSHOW_NF_ADJ set). */
#define DNET_NETSHOW_ADJ_NONE    0u
#define DNET_NETSHOW_ADJ_INIT    1u
#define DNET_NETSHOW_ADJ_UP      2u

/* Executor type (NCP "Type"). */
#define DNET_NETSHOW_TYPE_NONROUTING 0u
#define DNET_NETSHOW_TYPE_ROUTING    1u

struct dnet_netshow_req {
    uint8_t  version;
    uint8_t  entity;
    uint16_t cursor;                        /* first entry wanted (list entities) */
};

/* The executor block: carried in EVERY response (so a page is self-contained). */
struct dnet_netshow_exec {
    uint16_t addr;                          /* area<<10 | node                     */
    uint8_t  state_on;                      /* NETACP serving = 1                  */
    uint8_t  type;                          /* DNET_NETSHOW_TYPE_*                 */
    char     name[DNET_NETSHOW_NAMEMAX + 1];
    char     ident[DNET_NETSHOW_IDENTMAX + 1];
    char     circuit[DNET_NETSHOW_CIRCMAX + 1]; /* "" = NETACP has no circuit      */
    uint8_t  nsp_ver[3];                    /* the NSP version NETACP connects with */
    uint8_t  rtg_ver[3];                    /* the routing version its hellos carry */
    uint16_t max_links;                     /* NETACP's logical-link pool size     */
    uint16_t max_links_active;              /* counter: high-water of links in use */
    uint16_t active_links;                  /* links in use now                    */
    uint8_t  have_dr;                       /* endnode: a designated router chosen */
    uint16_t dr_addr;
};

struct dnet_netshow_node {
    uint16_t addr;
    char     name[DNET_NETSHOW_NAMEMAX + 1];
    uint8_t  flags;                         /* DNET_NETSHOW_NF_*                   */
    uint8_t  adj_state;                     /* DNET_NETSHOW_ADJ_*                  */
    uint16_t active_links;                  /* NETACP links to this node now       */
    uint16_t next_node;                     /* routing next hop; 0 = none          */
};

struct dnet_netshow_link {
    uint16_t local_link;                    /* our NSP logical-link address        */
    uint16_t remote_link;                   /* the remote's link address           */
    uint16_t node;                          /* remote node address                 */
    char     name[DNET_NETSHOW_NAMEMAX + 1];/* its name, if the node DB names it   */
    uint32_t pid;                           /* the local process on the link       */
    uint8_t  object;                        /* Session Control object (0 = named)  */
    uint8_t  outbound;                      /* 1 = a local process's _NET: link    */
    uint8_t  running;                       /* 1 = link up (RUN), 0 = connecting   */
};

/* A circuit NETACP runs (rd vms-2d0). Its name is the engine's circuit, named
 * after the VMS device NETACP's datalink rides (device-native naming, vms-47d:
 * the executive's ETH0: -> circuit ETH-0). The adjacent ROUTING node is the
 * designated router an endnode NETACP has selected on it; 0 = none known
 * (NETACP's adjacency table does not record whether a neighbour is a router,
 * so a routing NETACP reports none rather than guess). */
struct dnet_netshow_circuit {
    char     name[DNET_NETSHOW_CIRCMAX + 1];
    uint8_t  state_on;                      /* the datalink is open            */
    uint16_t adj_addr;                      /* adjacent routing node; 0 = none */
    char     adj_name[DNET_NETSHOW_NAMEMAX + 1];
};

/* The line under a circuit (on a LAN, VMS names it like the circuit). */
struct dnet_netshow_line {
    char     name[DNET_NETSHOW_CIRCMAX + 1];
    uint8_t  state_on;
};

/* An object NETACP serves: the table its inbound-connect dispatch consults.
 * File = the image a connect activates; pid = the process that serves it
 * in-place (a declared object), 0 = none. The record carries NO user or
 * password: NETACP's objects use the connect's own access control, and a
 * password never crosses this record. */
struct dnet_netshow_object {
    char     name[DNET_NETSHOW_OBJNAMEMAX + 1];
    uint8_t  number;
    char     file[DNET_NETSHOW_FILEMAX + 1];
    uint32_t pid;
};

struct dnet_netshow_rsp {
    uint8_t  version;
    uint8_t  entity;
    uint16_t total;                         /* entries in the whole table          */
    uint16_t first;                         /* index of entry[0] in that table     */
    uint8_t  count;                         /* entries in THIS record              */
    uint32_t as_of;                         /* NETACP's clock (Unix seconds)       */
    struct dnet_netshow_exec exec;
    union {
        struct dnet_netshow_node node[DNET_NETSHOW_NODES_PER];
        struct dnet_netshow_link link[DNET_NETSHOW_LINKS_PER];
        struct dnet_netshow_circuit circ[DNET_NETSHOW_CIRCS_PER];
        struct dnet_netshow_line line[DNET_NETSHOW_LINES_PER];
        struct dnet_netshow_object obj[DNET_NETSHOW_OBJS_PER];
    } u;
};

/* Request codec. decode: datalen must be exactly DNET_NETSHOW_REQ_LEN, version
 * DNET_NETSHOW_VERSION, entity one of DNET_NETSHOW_ENT_*. */
DNET_NETSHOW_API int dnet_netshow_req_encode(const struct dnet_netshow_req *q,
                                             uint8_t *buf, size_t cap, size_t *outlen);
DNET_NETSHOW_API int dnet_netshow_req_decode(const uint8_t *buf, size_t len,
                                             struct dnet_netshow_req *out);

/* Response codec (see the BOUNDS note above). encode refuses an over-bound count
 * or string (EBADLEN) rather than truncating it. */
DNET_NETSHOW_API int dnet_netshow_rsp_encode(const struct dnet_netshow_rsp *r,
                                             uint8_t *buf, size_t cap, size_t *outlen);
DNET_NETSHOW_API int dnet_netshow_rsp_decode(const uint8_t *buf, size_t len,
                                             struct dnet_netshow_rsp *out);

/* ---------------------------------------------------------------------------
 * The CLIENT: gather a whole entity (walking pages) through a query callback --
 * libvms $QIO IO$_ACPCONTROL on a _NET: channel in NCP/DCL, the in-process broker
 * in the host selftest. query() returns the VMS completion status and the raw
 * response bytes; odd = success.
 * ------------------------------------------------------------------------- */
typedef uint32_t (*dnet_netshow_query_fn)(void *ctx, const uint8_t *req, size_t reqlen,
                                          uint8_t *rsp, size_t rspcap, size_t *rsplen);

#define DNET_NETSHOW_MAX_NODES 512
#define DNET_NETSHOW_MAX_LINKS 256
#define DNET_NETSHOW_MAX_CIRCS 16
#define DNET_NETSHOW_MAX_LINES 16
#define DNET_NETSHOW_MAX_OBJS  128

struct dnet_netshow_view {
    uint32_t as_of;
    struct dnet_netshow_exec exec;
    unsigned nnodes;
    struct dnet_netshow_node nodes[DNET_NETSHOW_MAX_NODES];
    unsigned nlinks;
    struct dnet_netshow_link links[DNET_NETSHOW_MAX_LINKS];
    unsigned ncircs;
    struct dnet_netshow_circuit circs[DNET_NETSHOW_MAX_CIRCS];
    unsigned nlines;
    struct dnet_netshow_line lines[DNET_NETSHOW_MAX_LINES];
    unsigned nobjs;
    struct dnet_netshow_object objs[DNET_NETSHOW_MAX_OBJS];
};

/* Status for a snapshot that failed the bounds-validated decode or a page walk
 * that does not add up (SS$_BADPARAM -- named here so this file needs no ssdef). */
#define DNET_NETSHOW_ST_BADREC  20u

/* Fetch `entity` into *v (the executor block always; nodes/links for those
 * entities). Returns the VMS status of the first failing query, or
 * DNET_NETSHOW_ST_BADREC for a record that does not decode / is not the entity
 * asked for / whose pages do not chain, or 1 (SS$_NORMAL). */
DNET_NETSHOW_API uint32_t dnet_netshow_fetch(dnet_netshow_query_fn query, void *ctx,
                                             uint8_t entity, struct dnet_netshow_view *v);

/* ---------------------------------------------------------------------------
 * The FORMATTERS (oracle layout). Each line is handed to emit() WITHOUT a
 * newline; an empty string is a blank line. as_of is rendered " 4-OCT-2026
 * 21:19:55" in the process's local time.
 * ------------------------------------------------------------------------- */
typedef void (*dnet_netshow_emit_fn)(void *ctx, const char *line);

/* "D-MMM-YYYY hh:mm:ss" with the day space-padded to two columns. */
DNET_NETSHOW_API void dnet_netshow_asctime(uint32_t t, char *buf, size_t cap);

/* NCP SHOW EXECUTOR [SUMMARY | CHARACTERISTICS | COUNTERS]. */
#define DNET_NETSHOW_EXEC_SUMMARY  0
#define DNET_NETSHOW_EXEC_CHAR     1
#define DNET_NETSHOW_EXEC_COUNTERS 2
DNET_NETSHOW_API void dnet_netshow_fmt_executor(const struct dnet_netshow_view *v, int kind,
                                                dnet_netshow_emit_fn emit, void *ctx);
/* NCP SHOW KNOWN NODES (executor block + the node table). */
DNET_NETSHOW_API void dnet_netshow_fmt_known_nodes(const struct dnet_netshow_view *v,
                                                   dnet_netshow_emit_fn emit, void *ctx);
/* NCP SHOW NODE <x> -- `n` is the entry (NULL: the executor itself). */
DNET_NETSHOW_API void dnet_netshow_fmt_node(const struct dnet_netshow_view *v,
                                            const struct dnet_netshow_node *n,
                                            dnet_netshow_emit_fn emit, void *ctx);
/* NCP SHOW KNOWN LINKS. */
DNET_NETSHOW_API void dnet_netshow_fmt_known_links(const struct dnet_netshow_view *v,
                                                   dnet_netshow_emit_fn emit, void *ctx);
/* NCP SHOW KNOWN CIRCUITS / SHOW CIRCUIT <c> (`c` = the entry; NULL = all). */
DNET_NETSHOW_API void dnet_netshow_fmt_circuits(const struct dnet_netshow_view *v,
                                                const struct dnet_netshow_circuit *c,
                                                dnet_netshow_emit_fn emit, void *ctx);
/* NCP SHOW KNOWN LINES / SHOW LINE <l>. */
DNET_NETSHOW_API void dnet_netshow_fmt_lines(const struct dnet_netshow_view *v,
                                             const struct dnet_netshow_line *l,
                                             dnet_netshow_emit_fn emit, void *ctx);
/* NCP SHOW KNOWN OBJECTS / SHOW OBJECT <o>. */
DNET_NETSHOW_API void dnet_netshow_fmt_objects(const struct dnet_netshow_view *v,
                                               const struct dnet_netshow_object *o,
                                               dnet_netshow_emit_fn emit, void *ctx);
/* The DCL SHOW NETWORK DECNET product line (no newline). */
DNET_NETSHOW_API void dnet_netshow_fmt_network_line(const struct dnet_netshow_exec *ex,
                                                    char *buf, size_t cap);

/* "a.n" for a 16-bit DECnet address. */
DNET_NETSHOW_API void dnet_netshow_addr(uint16_t a, char *buf, size_t cap);

#endif /* DNET_NETSHOW_H */
