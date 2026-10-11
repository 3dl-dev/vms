/*
 * ncp.c - NCP.EXE: the DECnet Phase IV Network Control Program (configuration).
 *
 * The VMS-authentic NCP surface a DECnet manager uses to configure the local
 * node and the remote-node database: SET/DEFINE NODE, CLEAR/PURGE NODE,
 * SHOW KNOWN NODES / SHOW NODE, and SET/SHOW EXECUTOR. It operates on a
 * persisted node database (dnet_nodedb) and a small executor configuration
 * file. This is the "configuration" pillar of the DECnet layered product
 * (design-decnet-ovmx.md Phase 3; rd vms-1e9, epic vms-30e) -- the layer that
 * gives node NAMES the SET HOST / NODE:: paths resolve to addresses.
 *
 * Invoked one command per call, as DCL drives it through a foreign command:
 * `$ NCP :== $SYS$SYSTEM:NCP.EXE` then `$ NCP <command...>` (the command words
 * arrive as argv). The databases live at SYS$SYSTEM:NETNODE_LOCAL.DAT /
 * NETNODE_REMOTE.DAT / NETOBJECT.DAT through the VMS file layer (rd vms-1f69,
 * dnet_ncpstore.h).
 *
 * PROVENANCE (Rule 8): the NCP command grammar + SHOW layout are public (DECnet
 * for OpenVMS Networking Manual, NCP chapter). It also manages the OBJECT
 * database (SET/DEFINE/SHOW/CLEAR/PURGE OBJECT -- the Session Control objects
 * this node offers, e.g. 42=CTERM, 17=FAL; rd vms-f52, dnet_objectdb).
 *
 * SHOW READS THE RUNNING NETWORK (rd vms-30e). As on VMS, SHOW EXECUTOR [SUMMARY|
 * CHARACTERISTICS|COUNTERS], SHOW KNOWN NODES, SHOW NODE and SHOW KNOWN LINKS
 * ask the RUNNING NETACP: $ASSIGN _NET:, then $QIO IO$_ACPCONTROL, which libvms
 * brokers to NETACP; NETACP answers one snapshot record from its live state
 * (dnet_netshow.h) and NCP prints it in the layout of a real OpenVMS VAX V7.3
 * (docs/oracle/vax-ncp-show/). With no NETACP serving, SHOW fails -- there is no
 * volatile database to read -- and the permanent database is read with LIST,
 * exactly the VMS split.
 *
 * SHOW KNOWN CIRCUITS / LINES / OBJECTS (and SHOW CIRCUIT / LINE / OBJECT) read
 * NETACP the same way (rd vms-2d0): its circuit and line (the datalink it runs)
 * and the objects its inbound dispatch serves. LIST KNOWN OBJECTS / LIST OBJECT
 * read the permanent NETOBJECT.DAT.
 *
 * HONEST SCOPE, not yet built: OVMX keeps ONE persisted node database, so SET
 * (volatile) and DEFINE (permanent) both act on it (a running NETACP re-reads it
 * per request); the permanent object database does not yet feed NETACP's
 * object table; LOOP and circuit/line SET are later rungs and are not faked here
 * (INV-6).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dnet_nodedb.h"
#include "dnet_objectdb.h"
#include "dnet_ncpstore.h"
#include "dnet_netshow.h"   /* the NETACP snapshot record + oracle-layout formatters */

#include "descrip.h"
#include "iodef.h"
#include "ssdef.h"
#include "starlet.h"

/* An image run outside an executive-activated context (the host test) has no
 * process context yet; the system services need one for the channel table. */
struct vms_pcb;
extern struct vms_pcb *vms_pcb_get(void);
extern struct vms_pcb *vms_pcb_init(uint64_t initial_privs);

/* --- the databases (rd vms-1f69) ------------------------------------------
 * Executor, node and object databases live at SYS$SYSTEM:NETNODE_LOCAL.DAT /
 * NETNODE_REMOTE.DAT / NETOBJECT.DAT and are read/written THROUGH THE VMS FILE
 * LAYER (RMS over the ODS-2 ACP) by dnet_ncpstore -- the same store NETACP
 * (DECNETD.EXE) reads at startup, so what NCP DEFINEs is what the network
 * starts with, and STARTNET.COM's F$SEARCH gate on NETNODE_LOCAL.DAT sees it.
 * The record layout is OVMX's documented text (labelled in each file's header
 * comment), not the VMS binary indexed format. A write that cannot reach the
 * VMS file fails honestly, naming the file -- never redirected to a Linux path
 * (the pre-vms-1f69 /etc/ovmx/decnet default is gone). OVMX_DECNET_EXECUTOR /
 * _NODEDB / _OBJECTDB remain as the HOST-TEST hook only. */

static void exec_load(struct dnet_executor *x)
{
    /* A corrupt executor database is reported, never silently replaced by a
     * guessed address; SHOW/SET then proceed from an empty record. */
    if (dnet_store_load_executor(x) == DNET_STORE_ECORRUPT)
        fprintf(stderr, "%%NCP-W-DBRDERR, executor database %s is corrupt;"
                        " treating the executor as unconfigured\n",
                dnet_store_location(DNET_STORE_EXECUTOR));
}

static int exec_save(const struct dnet_executor *x)
{
    return dnet_store_save_executor(x) == DNET_STORE_OK ? DNET_NODEDB_OK
                                                        : DNET_NODEDB_EIO;
}

/* Load/save shims so the command dispatch below keeps its shape: every node /
 * object database access goes through the store (VMS file, or the host hook). */
static int nodes_load(struct dnet_nodedb *db)
{
    return dnet_store_load_nodes(db) == DNET_STORE_OK ? DNET_NODEDB_OK : DNET_NODEDB_EIO;
}
static int nodes_save(const struct dnet_nodedb *db)
{
    return dnet_store_save_nodes(db) == DNET_STORE_OK ? DNET_NODEDB_OK : DNET_NODEDB_EIO;
}
static int objects_load(struct dnet_objectdb *db)
{
    return dnet_store_load_objects(db) == DNET_STORE_OK ? DNET_OBJECTDB_OK : DNET_OBJECTDB_EIO;
}
static int objects_save(const struct dnet_objectdb *db)
{
    return dnet_store_save_objects(db) == DNET_STORE_OK ? DNET_OBJECTDB_OK : DNET_OBJECTDB_EIO;
}

/* --- LIST output: the PERMANENT database (the files NCP DEFINEs) ---------- */
static void show_executor(const struct dnet_executor *x, int characteristics)
{
    char astr[16] = "not configured";
    if (x->have_addr)
        snprintf(astr, sizeof(astr), "%u.%u", dnet_area_of(x->addr), dnet_node_of(x->addr));
    printf("\nNode Permanent %s\n\n", characteristics ? "Characteristics" : "Summary");
    if (x->have_addr && x->name[0])
        printf("Executor node = %s (%s)\n", astr, x->name);
    else
        printf("Executor node = %s\n", astr);
    printf("State                    = %s\n", x->state_on ? "on" : "off");
    if (characteristics) {
        /* The bound NETACP sizes its logical-link pool from this (vms-f91);
         * unset, the VMS default 32 (dnet_ncpstore.h). */
        printf("Maximum links            = %u\n", dnet_executor_max_links(x));
    }
}

static void show_node(const struct dnet_node_entry *e)
{
    char addr[16];
    snprintf(addr, sizeof(addr), "%u.%u", dnet_area_of(e->addr), dnet_node_of(e->addr));
    printf("%-12s %s\n", addr, e->name[0] ? e->name : "");
}

static void show_known_nodes(const struct dnet_nodedb *db)
{
    printf("\nKnown Node Permanent Summary\n\n");
    printf("Node         Name\n\n");
    for (unsigned i = 0; i < db->count; i++) {
        const struct dnet_node_entry *e = dnet_nodedb_at(db, i);
        if (e)
            show_node(e);
    }
}

static void show_object(const struct dnet_object_entry *e)
{
    /* "Object   Number  File" -- the public NCP OBJECT summary shape. */
    printf("%-12s %-7u %s\n", e->name[0] ? e->name : "", (unsigned)e->number,
           e->file[0] ? e->file : "");
}

static void show_known_objects(const struct dnet_objectdb *db)
{
    printf("\nKnown Object Permanent Summary\n\n");
    printf("Object       Number  File\n\n");
    for (unsigned i = 0; i < db->count; i++) {
        const struct dnet_object_entry *e = dnet_objectdb_at(db, i);
        if (e)
            show_object(e);
    }
}

/* --- helpers ------------------------------------------------------------- */
static int ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

/* An NCP keyword, which may be abbreviated to at least `min` characters. */
static int kw(const char *w, const char *full, size_t min)
{
    size_t n = strlen(w);
    return n >= min && n <= strlen(full) && strncasecmp(w, full, n) == 0;
}

/* --- SHOW: the RUNNING NETACP's volatile database (rd vms-30e) ------------ */
struct ncp_net {
    uint16_t chan;
};

/* One NETACP query: the request goes at p1, NETACP's snapshot comes back in
 * the same buffer (IO$_ACPCONTROL on _NET:, libvms qio_net_op). */
static uint32_t ncp_query(void *ctx, const uint8_t *req, size_t reqlen,
                          uint8_t *rsp, size_t rspcap, size_t *rsplen)
{
    struct ncp_net *n = ctx;
    struct _iosb iosb;
    *rsplen = 0;
    if (reqlen > rspcap)
        return SS$_BADPARAM;
    memmove(rsp, req, reqlen);
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$qiow(0, n->chan, IO$_ACPCONTROL, &iosb, NULL, 0, rsp,
                           (uint32_t)rspcap, (uint32_t)reqlen, 0, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    if (st & 1)
        *rsplen = iosb.iosb$l_dev_depend;
    return st;
}

static void ncp_emit(void *ctx, const char *line)
{
    (void)ctx;
    printf("%s\n", line);
}

/* The network is not up (no _NET: device, or no NETACP behind it): fail the
 * way NCP does, with the status underneath. (%NCP-F-OPEFAI is NCP's "Operation
 * failure" response; the secondary line is the real $ASSIGN/$QIO status text.
 * A capture of real NCP on a node with the network stopped has not been taken
 * -- re-ground this wording when one is.) */
static int ncp_net_fail(uint32_t st)
{
    char txt[256];
    struct dsc$descriptor_s d;
    uint16_t len = 0;
    d.dsc$w_length = (uint16_t)(sizeof txt - 1);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = txt;
    fprintf(stderr, "%%NCP-F-OPEFAI, Operation failure\n");
    if ((sys$getmsg(st, &len, &d, 0x0F, NULL) & 1) && len > 0) {
        txt[len < sizeof txt ? len : sizeof txt - 1] = '\0';
        if (txt[0] == '%')
            txt[0] = '-';
        fprintf(stderr, "%s\n", txt);
    } else {
        fprintf(stderr, "-SYSTEM-F-STATUS, status %%X%08X\n", (unsigned)st);
    }
    return 1;
}

static struct dnet_netshow_view g_view;   /* ~20 KB: not on the stack */

/* Read `entity` from the running NETACP into g_view. 1 = got it. */
static int ncp_fetch(uint8_t entity)
{
    if (!vms_pcb_get())
        vms_pcb_init(0);
    struct ncp_net n = { 0 };
    struct dsc$descriptor_s d;
    static char netdev[] = "_NET:";
    d.dsc$w_length = (uint16_t)strlen(netdev);
    d.dsc$b_dtype = DSC$K_DTYPE_T;
    d.dsc$b_class = DSC$K_CLASS_S;
    d.dsc$a_pointer = netdev;
    uint32_t st = sys$assign(&d, &n.chan, 0, NULL);
    if (!(st & 1)) {
        ncp_net_fail(st);
        return 0;
    }
    st = dnet_netshow_fetch(ncp_query, &n, entity, &g_view);
    (void)sys$dassgn(n.chan);
    if (st == DNET_NETSHOW_ST_BADREC) {
        fprintf(stderr, "%%NCP-F-INVRSP, invalid response from the network ACP"
                        " (its snapshot record did not validate)\n");
        return 0;
    }
    if (!(st & 1)) {
        ncp_net_fail(st);
        return 0;
    }
    return 1;
}

/* SHOW NODE x: the executor itself, or an entry of NETACP's known-node table. */
static int ncp_show_node(const char *arg)
{
    if (!ncp_fetch(DNET_NETSHOW_ENT_NODES))
        return 1;
    uint16_t a = 0;
    int by_addr = dnet_nodedb_parse_addr(arg, &a) == DNET_NODEDB_OK;
    if ((by_addr && a == g_view.exec.addr) ||
        (!by_addr && g_view.exec.name[0] && ieq(arg, g_view.exec.name))) {
        dnet_netshow_fmt_node(&g_view, NULL, ncp_emit, NULL);
        return 0;
    }
    for (unsigned i = 0; i < g_view.nnodes; i++) {
        const struct dnet_netshow_node *e = &g_view.nodes[i];
        if ((by_addr && e->addr == a) || (!by_addr && e->name[0] && ieq(arg, e->name))) {
            dnet_netshow_fmt_node(&g_view, e, ncp_emit, NULL);
            return 0;
        }
    }
    fprintf(stderr, "%%NCP-E-UNRNODE, unrecognized node name or address\n");
    return 1;
}

/* SHOW CIRCUIT c / SHOW LINE l: an entry of NETACP's circuit / line table. */
static int ncp_show_circuit_line(const char *arg, int circuit)
{
    if (!ncp_fetch(circuit ? DNET_NETSHOW_ENT_CIRCUITS : DNET_NETSHOW_ENT_LINES))
        return 1;
    if (circuit) {
        for (unsigned i = 0; i < g_view.ncircs; i++)
            if (ieq(arg, g_view.circs[i].name)) {
                dnet_netshow_fmt_circuits(&g_view, &g_view.circs[i], ncp_emit, NULL);
                return 0;
            }
        fprintf(stderr, "%%NCP-E-UNRCIR, unrecognized circuit\n");
        return 1;
    }
    for (unsigned i = 0; i < g_view.nlines; i++)
        if (ieq(arg, g_view.lines[i].name)) {
            dnet_netshow_fmt_lines(&g_view, &g_view.lines[i], ncp_emit, NULL);
            return 0;
        }
    fprintf(stderr, "%%NCP-E-UNRLIN, unrecognized line\n");
    return 1;
}

/* SHOW OBJECT o: an entry of NETACP's volatile object database, by name or
 * number. */
static int ncp_show_object(const char *arg)
{
    if (!ncp_fetch(DNET_NETSHOW_ENT_OBJECTS))
        return 1;
    char *end = NULL;
    long n = strtol(arg, &end, 10);
    int by_num = end && end != arg && *end == '\0';
    for (unsigned i = 0; i < g_view.nobjs; i++) {
        const struct dnet_netshow_object *o = &g_view.objs[i];
        if ((by_num && n >= 1 && n <= 255 && o->number == (uint8_t)n) ||
            (!by_num && o->name[0] && ieq(arg, o->name))) {
            dnet_netshow_fmt_objects(&g_view, o, ncp_emit, NULL);
            return 0;
        }
    }
    fprintf(stderr, "%%NCP-E-UNROBJ, unrecognized object name or number\n");
    return 1;
}

static int fail(const char *msg)
{
    fprintf(stderr, "%%NCP-E-%s\n", msg);
    return 1;
}

/* A database write that did not land: name the file it was going to, so the
 * operator sees WHERE (on the runtime, the VMS file through RMS over the ACP --
 * an absent executive or system volume is the only way this fails there). */
static int fail_db(const char *msg, enum dnet_store_db which)
{
    fprintf(stderr, "%%NCP-E-%s %s\n", msg, dnet_store_location(which));
    if (!dnet_store_host_override(which))
        fprintf(stderr, "-NCP-I-VMSFILE, written through RMS over the Files-11"
                        " ACP; no executive or system volume reachable\n");
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: NCP <command>\n"
        "  SET|DEFINE NODE <area.node> [NAME <name>]\n"
        "  CLEAR|PURGE NODE <area.node>|<name>\n"
        "  SHOW KNOWN NODES | KNOWN LINKS        (the running network)\n"
        "  SHOW NODE <area.node>|<name>\n"
        "  SHOW EXECUTOR [SUMMARY|CHARACTERISTICS|COUNTERS]\n"
        "  LIST KNOWN NODES | NODE <x> | EXECUTOR (the permanent database)\n"
        "  SET EXECUTOR ADDRESS <area.node> | NAME <name> | STATE ON|OFF | MAXIMUM LINKS <n>\n"
        "  SET|DEFINE OBJECT <name> NUMBER <1..255> [FILE <spec>]\n"
        "  CLEAR|PURGE OBJECT <name>|<number>\n"
        "  SHOW KNOWN CIRCUITS | KNOWN LINES | KNOWN OBJECTS (the running network)\n"
        "  SHOW CIRCUIT <c> | LINE <l> | OBJECT <name>|<number>\n"
        "  LIST KNOWN OBJECTS | OBJECT <name>|<number> (the permanent database)\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *verb = argv[1];
    const char *ent  = (argc >= 3) ? argv[2] : "";

    /* ---- SHOW: the running NETACP's volatile database (rd vms-30e) ---- */
    if (kw(verb, "SHOW", 3)) {
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "NODES", 3)) {
            if (!ncp_fetch(DNET_NETSHOW_ENT_NODES))
                return 1;
            dnet_netshow_fmt_known_nodes(&g_view, ncp_emit, NULL);
            return 0;
        }
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "LINKS", 3)) {
            if (!ncp_fetch(DNET_NETSHOW_ENT_LINKS))
                return 1;
            dnet_netshow_fmt_known_links(&g_view, ncp_emit, NULL);
            return 0;
        }
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "CIRCUITS", 3)) {
            if (!ncp_fetch(DNET_NETSHOW_ENT_CIRCUITS))
                return 1;
            dnet_netshow_fmt_circuits(&g_view, NULL, ncp_emit, NULL);
            return 0;
        }
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "LINES", 3)) {
            if (!ncp_fetch(DNET_NETSHOW_ENT_LINES))
                return 1;
            dnet_netshow_fmt_lines(&g_view, NULL, ncp_emit, NULL);
            return 0;
        }
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "OBJECTS", 3)) {
            if (!ncp_fetch(DNET_NETSHOW_ENT_OBJECTS))
                return 1;
            dnet_netshow_fmt_objects(&g_view, NULL, ncp_emit, NULL);
            return 0;
        }
        if (kw(ent, "CIRCUIT", 3) && argc >= 4)
            return ncp_show_circuit_line(argv[3], 1);
        if (kw(ent, "LINE", 3) && argc >= 4)
            return ncp_show_circuit_line(argv[3], 0);
        if (kw(ent, "OBJECT", 3) && argc >= 4)
            return ncp_show_object(argv[3]);
        if (kw(ent, "NODE", 4) && argc >= 4)
            return ncp_show_node(argv[3]);
        if (kw(ent, "EXECUTOR", 4)) {
            int kind = DNET_NETSHOW_EXEC_SUMMARY;
            if (argc >= 4) {
                if (kw(argv[3], "CHARACTERISTICS", 4))
                    kind = DNET_NETSHOW_EXEC_CHAR;
                else if (kw(argv[3], "COUNTERS", 4))
                    kind = DNET_NETSHOW_EXEC_COUNTERS;
                else if (!kw(argv[3], "SUMMARY", 3)) {
                    usage();
                    return 1;
                }
            }
            if (!ncp_fetch(DNET_NETSHOW_ENT_EXECUTOR))
                return 1;
            dnet_netshow_fmt_executor(&g_view, kind, ncp_emit, NULL);
            return 0;
        }
        usage();
        return 1;
    }

    /* ---- LIST: the permanent database (the files). ---- */
    if (kw(verb, "LIST", 3)) {
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "NODES", 3)) {

            struct dnet_nodedb db;
            if (nodes_load(&db) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            show_known_nodes(&db);
            return 0;
        }
        if (kw(ent, "KNOWN", 3) && argc >= 4 && kw(argv[3], "OBJECTS", 3)) {
            struct dnet_objectdb db;
            if (objects_load(&db) != DNET_OBJECTDB_OK)
                return fail("DBRDERR, object database is corrupt");
            show_known_objects(&db);
            return 0;
        }
        if (kw(ent, "OBJECT", 3) && argc >= 4) {
            struct dnet_objectdb db;
            if (objects_load(&db) != DNET_OBJECTDB_OK)
                return fail("DBRDERR, object database is corrupt");
            const struct dnet_object_entry *e = NULL;
            char *end = NULL;
            long n = strtol(argv[3], &end, 10);
            if (end && *end == '\0' && n >= 1 && n <= 255)
                e = dnet_objectdb_by_number(&db, (uint8_t)n);
            else
                e = dnet_objectdb_by_name(&db, argv[3]);
            if (!e)
                return fail("UNROBJ, unrecognized object name or number");
            printf("\nObject Permanent Summary\n\nObject       Number  File\n\n");
            show_object(e);
            return 0;
        }
        if (kw(ent, "NODE", 4) && argc >= 4) {
            struct dnet_nodedb db;
            if (nodes_load(&db) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            const struct dnet_node_entry *e = NULL;
            uint16_t a = 0;
            if (dnet_nodedb_parse_addr(argv[3], &a) == DNET_NODEDB_OK)
                e = dnet_nodedb_by_addr(&db, a);
            else
                e = dnet_nodedb_by_name(&db, argv[3]);
            if (!e)
                return fail("UNRNODE, unrecognized node name or address");
            printf("\nNode Permanent Summary\n\nNode         Name\n\n");
            show_node(e);
            return 0;
        }
        if (kw(ent, "EXECUTOR", 4)) {
            struct dnet_executor x;
            exec_load(&x);
            show_executor(&x, argc >= 4 && kw(argv[3], "CHARACTERISTICS", 4));
            return 0;
        }
        usage();
        return 1;
    }

    /* ---- SET / DEFINE NODE, SET EXECUTOR ---- */
    if (ieq(verb, "SET") || ieq(verb, "DEFINE")) {
        if (ieq(ent, "NODE") && argc >= 4) {
            uint16_t a = 0;
            if (dnet_nodedb_parse_addr(argv[3], &a) != DNET_NODEDB_OK)
                return fail("INVADDR, address must be area.node (1..63 . 1..1023)");
            const char *name = "";
            if (argc >= 6 && ieq(argv[4], "NAME"))
                name = argv[5];
            struct dnet_nodedb db;
            if (nodes_load(&db) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            int rc = dnet_nodedb_set(&db, a, name);
            if (rc == DNET_NODEDB_EFULL)
                return fail("DBFULL, node database is full");
            if (rc != DNET_NODEDB_OK)
                return fail("INVNAME, bad node name or name already in use");
            if (nodes_save(&db) != DNET_NODEDB_OK)
                return fail_db("DBWRERR, could not write the node database",
                               DNET_STORE_NODES);
            return 0;
        }
        if (ieq(ent, "OBJECT") && argc >= 4) {
            /* SET|DEFINE OBJECT <name> NUMBER <1..255> [FILE <spec>] */
            const char *oname = argv[3];
            long num = -1;
            const char *file = "";
            for (int i = 4; i + 1 < argc; i += 2) {
                if (ieq(argv[i], "NUMBER")) {
                    char *end = NULL;
                    num = strtol(argv[i + 1], &end, 10);
                    if (!end || *end != '\0' || num < 1 || num > 255)
                        return fail("INVOBJNUM, object number must be 1..255");
                } else if (ieq(argv[i], "FILE")) {
                    file = argv[i + 1];
                } else {
                    usage();
                    return 1;
                }
            }
            if (num < 0)
                return fail("MISSOBJNUM, SET OBJECT requires NUMBER <1..255>");
            struct dnet_objectdb db;
            if (objects_load(&db) != DNET_OBJECTDB_OK)
                return fail("DBRDERR, object database is corrupt");
            int rc = dnet_objectdb_set(&db, (uint8_t)num, oname, file);
            if (rc == DNET_OBJECTDB_EFULL)
                return fail("DBFULL, object database is full");
            if (rc != DNET_OBJECTDB_OK)
                return fail("INVOBJ, bad object name/file or name already in use");
            if (objects_save(&db) != DNET_OBJECTDB_OK)
                return fail_db("DBWRERR, could not write the object database",
                               DNET_STORE_OBJECTS);
            return 0;
        }
        if (ieq(ent, "EXECUTOR") && argc >= 4) {
            struct dnet_executor x;
            exec_load(&x);
            if (ieq(argv[3], "ADDRESS") && argc >= 5) {
                uint16_t a = 0;
                if (dnet_nodedb_parse_addr(argv[4], &a) != DNET_NODEDB_OK)
                    return fail("INVADDR, address must be area.node");
                x.have_addr = 1;
                x.addr = a;
            } else if (ieq(argv[3], "NAME") && argc >= 5) {
                snprintf(x.name, sizeof(x.name), "%.*s", DNET_NODEDB_NAMEMAX, argv[4]);
            } else if (ieq(argv[3], "STATE") && argc >= 5) {
                x.state_on = ieq(argv[4], "ON");
            } else if (ieq(argv[3], "MAXIMUM") && argc >= 6 && ieq(argv[4], "LINKS")) {
                char *end = NULL;
                unsigned long v = strtoul(argv[5], &end, 10);
                if (!end || *end != '\0' || v == 0 || v > DNET_EXECUTOR_MAXLINKS_MAX)
                    return fail("INVPVA, invalid parameter value (MAXIMUM LINKS 1 to 65535)");
                x.max_links = (uint16_t)v;
            } else {
                usage();
                return 1;
            }
            if (exec_save(&x) != DNET_NODEDB_OK)
                return fail_db("CFGWRERR, could not write the executor database",
                               DNET_STORE_EXECUTOR);
            return 0;
        }
        usage();
        return 1;
    }

    /* ---- CLEAR / PURGE NODE ---- */
    if (ieq(verb, "CLEAR") || ieq(verb, "PURGE")) {
        if (ieq(ent, "NODE") && argc >= 4) {
            struct dnet_nodedb db;
            if (nodes_load(&db) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            uint16_t a = 0;
            int rc;
            if (dnet_nodedb_parse_addr(argv[3], &a) == DNET_NODEDB_OK)
                rc = dnet_nodedb_clear_addr(&db, a);
            else
                rc = dnet_nodedb_clear_name(&db, argv[3]);
            if (rc == DNET_NODEDB_ENOENT)
                return fail("UNRNODE, no such node in the database");
            if (rc != DNET_NODEDB_OK)
                return fail("INVNODE, bad node name or address");
            if (nodes_save(&db) != DNET_NODEDB_OK)
                return fail_db("DBWRERR, could not write the node database",
                               DNET_STORE_NODES);
            return 0;
        }
        if (ieq(ent, "OBJECT") && argc >= 4) {
            struct dnet_objectdb db;
            if (objects_load(&db) != DNET_OBJECTDB_OK)
                return fail("DBRDERR, object database is corrupt");
            int rc;
            char *end = NULL;
            long n = strtol(argv[3], &end, 10);
            if (end && *end == '\0' && n >= 1 && n <= 255)
                rc = dnet_objectdb_clear_number(&db, (uint8_t)n);
            else
                rc = dnet_objectdb_clear_name(&db, argv[3]);
            if (rc == DNET_OBJECTDB_ENOENT)
                return fail("UNROBJ, no such object in the database");
            if (rc != DNET_OBJECTDB_OK)
                return fail("INVOBJ, bad object name or number");
            if (objects_save(&db) != DNET_OBJECTDB_OK)
                return fail_db("DBWRERR, could not write the object database",
                               DNET_STORE_OBJECTS);
            return 0;
        }
        usage();
        return 1;
    }

    usage();
    return 1;
}
