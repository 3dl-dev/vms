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
 * Invoked one command per call, as DCL drives it: `MCR NCP <command...>`.
 *
 * PROVENANCE (Rule 8): the NCP command grammar + SHOW layout are public (DECnet
 * for OpenVMS Networking Manual, NCP chapter). It also manages the OBJECT
 * database (SET/DEFINE/SHOW/CLEAR/PURGE OBJECT -- the Session Control objects
 * this node offers, e.g. 42=CTERM, 17=FAL; rd vms-f52, dnet_objectdb). HONEST
 * SCOPE, not yet built: OVMX keeps ONE persisted database, so SET (volatile) and
 * DEFINE (permanent) both act on it -- the volatile/permanent split, circuits,
 * lines, counters, LOOP, and live reachability state are later rungs and are not
 * faked here (INV-6: counters/circuit state must be read from the live executive,
 * never invented).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dnet_nodedb.h"
#include "dnet_objectdb.h"

/* --- executor configuration (SET/SHOW EXECUTOR) -------------------------- */
struct ncp_executor {
    int      have_addr;
    uint16_t addr;
    char     name[DNET_NODEDB_NAMEMAX + 1];
    int      state_on;
};

static const char *nodedb_path(void)
{
    const char *p = getenv("OVMX_DECNET_NODEDB");
    return (p && p[0]) ? p : "/etc/ovmx/decnet/netnode_remote.dat";
}
static const char *executor_path(void)
{
    const char *p = getenv("OVMX_DECNET_EXECUTOR");
    return (p && p[0]) ? p : "/etc/ovmx/decnet/executor.dat";
}
static const char *objectdb_path(void)
{
    const char *p = getenv("OVMX_DECNET_OBJECTDB");
    return (p && p[0]) ? p : "/etc/ovmx/decnet/object.dat";
}

static void exec_load(struct ncp_executor *x)
{
    memset(x, 0, sizeof(*x));
    FILE *f = fopen(executor_path(), "r");
    if (!f)
        return;
    char astr[32] = "", name[64] = "", st[16] = "";
    /* Format: "EXECUTOR <a.n|-> NAME <name|-> STATE <on|off>" */
    if (fscanf(f, "EXECUTOR %31s NAME %63s STATE %15s", astr, name, st) == 3) {
        uint16_t a = 0;
        if (dnet_nodedb_parse_addr(astr, &a) == DNET_NODEDB_OK) {
            x->have_addr = 1;
            x->addr = a;
        }
        if (strcmp(name, "-") != 0)
            snprintf(x->name, sizeof(x->name), "%.*s", DNET_NODEDB_NAMEMAX, name);
        x->state_on = (strcasecmp(st, "on") == 0);
    }
    fclose(f);
}

static int exec_save(const struct ncp_executor *x)
{
    FILE *f = fopen(executor_path(), "w");
    if (!f)
        return DNET_NODEDB_EIO;
    char astr[16] = "-";
    if (x->have_addr)
        snprintf(astr, sizeof(astr), "%u.%u", dnet_area_of(x->addr), dnet_node_of(x->addr));
    fprintf(f, "EXECUTOR %s NAME %s STATE %s\n",
            astr, x->name[0] ? x->name : "-", x->state_on ? "on" : "off");
    int ok = (fflush(f) == 0 && !ferror(f));
    fclose(f);
    return ok ? DNET_NODEDB_OK : DNET_NODEDB_EIO;
}

/* --- SHOW output (VMS-faithful shape) ------------------------------------ */
static void show_executor(const struct ncp_executor *x, int characteristics)
{
    char astr[16] = "not configured";
    if (x->have_addr)
        snprintf(astr, sizeof(astr), "%u.%u", dnet_area_of(x->addr), dnet_node_of(x->addr));
    printf("\nNode Volatile %s\n\n", characteristics ? "Characteristics" : "Summary");
    if (x->have_addr && x->name[0])
        printf("Executor node = %s (%s)\n", astr, x->name);
    else
        printf("Executor node = %s\n", astr);
    printf("State                    = %s\n", x->state_on ? "on" : "off");
    if (characteristics)
        printf("Identification           = OVMX DECnet-compatible networking\n");
}

static void show_node(const struct dnet_node_entry *e)
{
    char addr[16];
    snprintf(addr, sizeof(addr), "%u.%u", dnet_area_of(e->addr), dnet_node_of(e->addr));
    printf("%-12s %s\n", addr, e->name[0] ? e->name : "");
}

static void show_known_nodes(const struct dnet_nodedb *db)
{
    printf("\nKnown Node Volatile Summary\n\n");
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
    printf("\nKnown Object Volatile Summary\n\n");
    printf("Object       Number  File\n\n");
    for (unsigned i = 0; i < db->count; i++) {
        const struct dnet_object_entry *e = dnet_objectdb_at(db, i);
        if (e)
            show_object(e);
    }
}

/* --- helpers ------------------------------------------------------------- */
static int ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

static int fail(const char *msg)
{
    fprintf(stderr, "%%NCP-E-%s\n", msg);
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: NCP <command>\n"
        "  SET|DEFINE NODE <area.node> [NAME <name>]\n"
        "  CLEAR|PURGE NODE <area.node>|<name>\n"
        "  SHOW KNOWN NODES\n"
        "  SHOW NODE <area.node>|<name>\n"
        "  SET EXECUTOR ADDRESS <area.node> | NAME <name> | STATE ON|OFF\n"
        "  SHOW EXECUTOR [CHARACTERISTICS]\n"
        "  SET|DEFINE OBJECT <name> NUMBER <1..255> [FILE <spec>]\n"
        "  CLEAR|PURGE OBJECT <name>|<number>\n"
        "  SHOW KNOWN OBJECTS\n"
        "  SHOW OBJECT <name>|<number>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *verb = argv[1];
    const char *ent  = (argc >= 3) ? argv[2] : "";

    /* ---- SHOW ---- */
    if (ieq(verb, "SHOW")) {
        if (ieq(ent, "KNOWN") && argc >= 4 && ieq(argv[3], "NODES")) {
            struct dnet_nodedb db;
            if (dnet_nodedb_load(&db, nodedb_path()) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            show_known_nodes(&db);
            return 0;
        }
        if (ieq(ent, "KNOWN") && argc >= 4 && ieq(argv[3], "OBJECTS")) {
            struct dnet_objectdb db;
            if (dnet_objectdb_load(&db, objectdb_path()) != DNET_OBJECTDB_OK)
                return fail("DBRDERR, object database is corrupt");
            show_known_objects(&db);
            return 0;
        }
        if (ieq(ent, "OBJECT") && argc >= 4) {
            struct dnet_objectdb db;
            if (dnet_objectdb_load(&db, objectdb_path()) != DNET_OBJECTDB_OK)
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
            printf("\nObject Volatile Summary\n\nObject       Number  File\n\n");
            show_object(e);
            return 0;
        }
        if (ieq(ent, "NODE") && argc >= 4) {
            struct dnet_nodedb db;
            if (dnet_nodedb_load(&db, nodedb_path()) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            const struct dnet_node_entry *e = NULL;
            uint16_t a = 0;
            if (dnet_nodedb_parse_addr(argv[3], &a) == DNET_NODEDB_OK)
                e = dnet_nodedb_by_addr(&db, a);
            else
                e = dnet_nodedb_by_name(&db, argv[3]);
            if (!e)
                return fail("UNRNODE, unrecognized node name or address");
            printf("\nNode Volatile Summary\n\nNode         Name\n\n");
            show_node(e);
            return 0;
        }
        if (ieq(ent, "EXECUTOR")) {
            struct ncp_executor x;
            exec_load(&x);
            show_executor(&x, argc >= 4 && ieq(argv[3], "CHARACTERISTICS"));
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
            if (dnet_nodedb_load(&db, nodedb_path()) != DNET_NODEDB_OK)
                return fail("DBRDERR, node database is corrupt");
            int rc = dnet_nodedb_set(&db, a, name);
            if (rc == DNET_NODEDB_EFULL)
                return fail("DBFULL, node database is full");
            if (rc != DNET_NODEDB_OK)
                return fail("INVNAME, bad node name or name already in use");
            if (dnet_nodedb_save(&db, nodedb_path()) != DNET_NODEDB_OK)
                return fail("DBWRERR, could not write the node database");
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
            if (dnet_objectdb_load(&db, objectdb_path()) != DNET_OBJECTDB_OK)
                return fail("DBRDERR, object database is corrupt");
            int rc = dnet_objectdb_set(&db, (uint8_t)num, oname, file);
            if (rc == DNET_OBJECTDB_EFULL)
                return fail("DBFULL, object database is full");
            if (rc != DNET_OBJECTDB_OK)
                return fail("INVOBJ, bad object name/file or name already in use");
            if (dnet_objectdb_save(&db, objectdb_path()) != DNET_OBJECTDB_OK)
                return fail("DBWRERR, could not write the object database");
            return 0;
        }
        if (ieq(ent, "EXECUTOR") && argc >= 4) {
            struct ncp_executor x;
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
            } else {
                usage();
                return 1;
            }
            if (exec_save(&x) != DNET_NODEDB_OK)
                return fail("CFGWRERR, could not write executor config");
            return 0;
        }
        usage();
        return 1;
    }

    /* ---- CLEAR / PURGE NODE ---- */
    if (ieq(verb, "CLEAR") || ieq(verb, "PURGE")) {
        if (ieq(ent, "NODE") && argc >= 4) {
            struct dnet_nodedb db;
            if (dnet_nodedb_load(&db, nodedb_path()) != DNET_NODEDB_OK)
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
            if (dnet_nodedb_save(&db, nodedb_path()) != DNET_NODEDB_OK)
                return fail("DBWRERR, could not write the node database");
            return 0;
        }
        if (ieq(ent, "OBJECT") && argc >= 4) {
            struct dnet_objectdb db;
            if (dnet_objectdb_load(&db, objectdb_path()) != DNET_OBJECTDB_OK)
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
            if (dnet_objectdb_save(&db, objectdb_path()) != DNET_OBJECTDB_OK)
                return fail("DBWRERR, could not write the object database");
            return 0;
        }
        usage();
        return 1;
    }

    usage();
    return 1;
}
