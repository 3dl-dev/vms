/*
 * dnet_ncpstore.c - the DECnet Phase IV configuration databases at
 * SYS$SYSTEM:NETNODE_LOCAL.DAT / NETNODE_REMOTE.DAT / NETOBJECT.DAT, reached
 * through the VMS file layer (RMS over the Files-11 ACP), shared by NCP.EXE and
 * NETACP (DECNETD.EXE). See dnet_ncpstore.h for the contract (rd vms-1f69).
 *
 * TWO BACKENDS, chosen per database by ONE rule and never mixed at runtime:
 *   - the host-test override (OVMX_DECNET_* env var set): the path-based POSIX
 *     load/save the node/object unit tests already pin (atomic temp+rename);
 *   - otherwise the VMS file: rms_textfile_open/getline/close to read,
 *     rms_textfile_write_line (supersede: $CREATE + $PUT of the header record)
 *     then rms_textfile_append_line per record to write -- the same writer
 *     idiom the persistent TCPIP$SERVICE.DAT store uses
 *     (src/vmstcpip/mgmt/tcpip_service_db.h).
 * There is no third arm: no /etc/ovmx default, no "try RMS then fall back to a
 * Linux file" (Rule 9 / INV-6).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dnet_ncpstore.h"
#include "rms_textfile.h"

static const char *const k_spec[3] = {
    "SYS$SYSTEM:NETNODE_LOCAL.DAT",
    "SYS$SYSTEM:NETNODE_REMOTE.DAT",
    "SYS$SYSTEM:NETOBJECT.DAT",
};
static const char *const k_env[3] = {
    "OVMX_DECNET_EXECUTOR",
    "OVMX_DECNET_NODEDB",
    "OVMX_DECNET_OBJECTDB",
};

static int db_ok(enum dnet_store_db db)
{
    return (int)db >= 0 && (int)db <= 2;
}

const char *dnet_store_vms_spec(enum dnet_store_db db)
{
    return db_ok(db) ? k_spec[db] : "";
}

const char *dnet_store_env_name(enum dnet_store_db db)
{
    return db_ok(db) ? k_env[db] : "";
}

const char *dnet_store_host_override(enum dnet_store_db db)
{
    if (!db_ok(db))
        return NULL;
    const char *p = getenv(k_env[db]);
    return (p && p[0]) ? p : NULL;
}

const char *dnet_store_location(enum dnet_store_db db)
{
    const char *h = dnet_store_host_override(db);
    return h ? h : dnet_store_vms_spec(db);
}

/* --- generic line I/O over the chosen backend ----------------------------- */

typedef int (*line_fn)(void *ctx, const char *line);   /* 0 ok, <0 corrupt */

/* Read every record of `db`. Absent -> DNET_STORE_OK with no callbacks. */
static int store_read(enum dnet_store_db db, line_fn fn, void *ctx)
{
    char line[512];
    const char *host = dnet_store_host_override(db);

    if (host) {
        FILE *f = fopen(host, "r");
        if (!f)
            return DNET_STORE_OK;              /* absent == empty */
        int rc = DNET_STORE_OK;
        while (fgets(line, sizeof(line), f)) {
            if (fn(ctx, line) < 0) {
                rc = DNET_STORE_ECORRUPT;
                break;
            }
        }
        fclose(f);
        return rc;
    }

    /* The VMS file. rms_textfile_open() returns NULL for an absent file AND
     * when the file layer is unreachable; both read as "no database" here --
     * the honest failure for an unreachable file layer is on the WRITE, which
     * cannot succeed without it (the same split tcpip_service_db.h makes). */
    rms_textfile_t *tf = rms_textfile_open(k_spec[db]);
    if (!tf)
        return DNET_STORE_OK;
    int rc = DNET_STORE_OK;
    int too_long = 0;
    while (rms_textfile_getline(tf, line, sizeof(line), &too_long) == 1) {
        if (too_long || fn(ctx, line) < 0) {
            rc = DNET_STORE_ECORRUPT;
            break;
        }
    }
    rms_textfile_close(tf);
    return rc;
}

/* Write `n` records (after the two header lines) to `db`, superseding it. */
static int store_write(enum dnet_store_db db, const char *const hdr[2],
                       const char *const *recs, unsigned n)
{
    const char *host = dnet_store_host_override(db);

    if (host) {
        char tmp[1024];
        int m = snprintf(tmp, sizeof(tmp), "%s.tmp", host);
        if (m <= 0 || (size_t)m >= sizeof(tmp))
            return DNET_STORE_EWRITE;
        FILE *f = fopen(tmp, "w");
        if (!f)
            return DNET_STORE_EWRITE;
        fprintf(f, "%s\n%s\n", hdr[0], hdr[1]);
        for (unsigned i = 0; i < n; i++)
            fprintf(f, "%s\n", recs[i]);
        if (fflush(f) != 0 || ferror(f)) {
            fclose(f);
            remove(tmp);
            return DNET_STORE_EWRITE;
        }
        fclose(f);
        if (rename(tmp, host) != 0) {
            remove(tmp);
            return DNET_STORE_EWRITE;
        }
        return DNET_STORE_OK;
    }

    /* RMS over the ACP: supersede with the first header record, then append. */
    if (rms_textfile_write_line(k_spec[db], hdr[0]) != 0)
        return DNET_STORE_EWRITE;
    if (rms_textfile_append_line(k_spec[db], hdr[1]) != 0)
        return DNET_STORE_EWRITE;
    for (unsigned i = 0; i < n; i++)
        if (rms_textfile_append_line(k_spec[db], recs[i]) != 0)
            return DNET_STORE_EWRITE;
    return DNET_STORE_OK;
}

/* --- executor ------------------------------------------------------------- */

static const char *const k_exec_hdr[2] = {
    "# OVMX DECnet Phase IV local node (executor) database (NCP SET/DEFINE EXECUTOR).",
    "# Format: EXECUTOR <area>.<node>|- NAME <name>|- STATE on|off  -- OVMX layout,"
    " not the VMS NETNODE_LOCAL.DAT binary format.",
};

int dnet_executor_parse_line(const char *line, struct dnet_executor *x)
{
    if (!line || !x)
        return -1;
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
        return 0;
    char kw[16] = "", astr[32] = "", nkw[16] = "", name[64] = "", skw[16] = "",
         st[16] = "", mkw[16] = "", mval[16] = "", extra[8] = "";
    int nf = sscanf(p, "%15s %31s %15s %63s %15s %15s %15s %15s %7s", kw, astr, nkw, name,
                    skw, st, mkw, mval, extra);
    if ((nf != 6 && nf != 8) ||
        strcmp(kw, "EXECUTOR") != 0 || strcmp(nkw, "NAME") != 0 ||
        strcmp(skw, "STATE") != 0 || (nf == 8 && strcmp(mkw, "MAXLINKS") != 0))
        return -1;
    struct dnet_executor t;
    memset(&t, 0, sizeof(t));
    if (strcmp(astr, "-") != 0) {
        uint16_t a = 0;
        if (dnet_nodedb_parse_addr(astr, &a) != DNET_NODEDB_OK)
            return -1;
        t.have_addr = 1;
        t.addr = a;
    }
    if (strcmp(name, "-") != 0) {
        if (strlen(name) > DNET_NODEDB_NAMEMAX)
            return -1;
        snprintf(t.name, sizeof(t.name), "%s", name);
    }
    if (strcasecmp(st, "on") == 0)
        t.state_on = 1;
    else if (strcasecmp(st, "off") != 0)
        return -1;
    if (nf == 8) {
        char *end = NULL;
        unsigned long v = strtoul(mval, &end, 10);
        if (!end || *end != '\0' || v == 0 || v > DNET_EXECUTOR_MAXLINKS_MAX)
            return -1;
        t.max_links = (uint16_t)v;
    }
    *x = t;
    return 1;
}

unsigned dnet_executor_max_links(const struct dnet_executor *x)
{
    return (x && x->max_links) ? x->max_links : DNET_EXECUTOR_DEFAULT_MAXLINKS;
}

void dnet_executor_format(const struct dnet_executor *x, char *buf, unsigned bufsz)
{
    char astr[16] = "-";
    if (!x || !buf || bufsz == 0)
        return;
    if (x->have_addr)
        snprintf(astr, sizeof(astr), "%u.%u", dnet_area_of(x->addr),
                 dnet_node_of(x->addr));
    if (x->max_links)
        snprintf(buf, bufsz, "EXECUTOR %s NAME %s STATE %s MAXLINKS %u", astr,
                 x->name[0] ? x->name : "-", x->state_on ? "on" : "off",
                 (unsigned)x->max_links);
    else
        snprintf(buf, bufsz, "EXECUTOR %s NAME %s STATE %s", astr,
                 x->name[0] ? x->name : "-", x->state_on ? "on" : "off");
}

struct exec_ctx {
    struct dnet_executor *x;
    int seen;
};

static int exec_line(void *vctx, const char *line)
{
    struct exec_ctx *c = vctx;
    int r = dnet_executor_parse_line(line, c->x);
    if (r < 0)
        return -1;
    if (r > 0) {
        if (c->seen)
            return -1;                 /* a second executor record: corrupt */
        c->seen = 1;
    }
    return 0;
}

int dnet_store_load_executor(struct dnet_executor *x)
{
    if (!x)
        return DNET_STORE_EINVAL;
    memset(x, 0, sizeof(*x));
    struct exec_ctx c = { x, 0 };
    int rc = store_read(DNET_STORE_EXECUTOR, exec_line, &c);
    if (rc != DNET_STORE_OK)
        memset(x, 0, sizeof(*x));       /* never a half-parsed address */
    return rc;
}

int dnet_store_save_executor(const struct dnet_executor *x)
{
    if (!x)
        return DNET_STORE_EINVAL;
    char rec[96];
    const char *recs[1] = { rec };
    dnet_executor_format(x, rec, sizeof(rec));
    return store_write(DNET_STORE_EXECUTOR, k_exec_hdr, recs, 1);
}

/* --- nodes / objects ------------------------------------------------------ */

static int node_line(void *ctx, const char *line)
{
    return dnet_nodedb_apply_line((struct dnet_nodedb *)ctx, line) == DNET_NODEDB_OK
               ? 0 : -1;
}

static int object_line(void *ctx, const char *line)
{
    return dnet_objectdb_apply_line((struct dnet_objectdb *)ctx, line) == DNET_OBJECTDB_OK
               ? 0 : -1;
}

int dnet_store_load_nodes(struct dnet_nodedb *db)
{
    if (!db)
        return DNET_STORE_EINVAL;
    dnet_nodedb_init(db);
    int rc = store_read(DNET_STORE_NODES, node_line, db);
    if (rc != DNET_STORE_OK)
        dnet_nodedb_init(db);
    return rc;
}

int dnet_store_save_nodes(const struct dnet_nodedb *db)
{
    if (!db)
        return DNET_STORE_EINVAL;
    static char lines[DNET_NODEDB_MAX][64];
    static const char *recs[DNET_NODEDB_MAX];
    unsigned n = 0;
    for (unsigned i = 0; i < db->count && n < DNET_NODEDB_MAX; i++) {
        const struct dnet_node_entry *e = dnet_nodedb_at(db, i);
        if (!e)
            break;
        if (dnet_nodedb_format_entry(e, lines[n], sizeof(lines[n])) != DNET_NODEDB_OK)
            return DNET_STORE_EWRITE;
        recs[n] = lines[n];
        n++;
    }
    return store_write(DNET_STORE_NODES, DNET_NODEDB_HEADER, recs, n);
}

int dnet_store_load_objects(struct dnet_objectdb *db)
{
    if (!db)
        return DNET_STORE_EINVAL;
    dnet_objectdb_init(db);
    int rc = store_read(DNET_STORE_OBJECTS, object_line, db);
    if (rc != DNET_STORE_OK)
        dnet_objectdb_init(db);
    return rc;
}

int dnet_store_save_objects(const struct dnet_objectdb *db)
{
    if (!db)
        return DNET_STORE_EINVAL;
    static char lines[DNET_OBJECTDB_MAX][DNET_OBJECTDB_FILEMAX + 64];
    static const char *recs[DNET_OBJECTDB_MAX];
    unsigned n = 0;
    for (unsigned i = 0; i < db->count && n < DNET_OBJECTDB_MAX; i++) {
        const struct dnet_object_entry *e = dnet_objectdb_at(db, i);
        if (!e)
            break;
        if (dnet_objectdb_format_entry(e, lines[n], sizeof(lines[n])) != DNET_OBJECTDB_OK)
            return DNET_STORE_EWRITE;
        recs[n] = lines[n];
        n++;
    }
    return store_write(DNET_STORE_OBJECTS, DNET_OBJECTDB_HEADER, recs, n);
}
