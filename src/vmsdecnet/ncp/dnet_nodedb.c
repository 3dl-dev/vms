/*
 * dnet_nodedb.c - DECnet Phase IV node database (see dnet_nodedb.h).
 *
 * Pure table logic + a documented plain-text on-disk format. No engine, no
 * socket. NCP node names are case-insensitive and held uppercase (DNA/VMS).
 */
#include "dnet_nodedb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dnet_nodedb_parse_addr(const char *s, uint16_t *addr_out)
{
    if (!s || !addr_out)
        return DNET_NODEDB_EINVAL;
    char *end = NULL;
    long area = strtol(s, &end, 10);
    if (end == s || !end || *end != '.')
        return DNET_NODEDB_EINVAL;
    const char *nstr = end + 1;
    char *end2 = NULL;
    long node = strtol(nstr, &end2, 10);
    if (end2 == nstr || !end2 || *end2 != '\0')
        return DNET_NODEDB_EINVAL;
    if (area < 1 || area > 63 || node < 1 || node > 1023)
        return DNET_NODEDB_EINVAL;
    *addr_out = (uint16_t)(((unsigned)area << 10) | (unsigned)node);
    return DNET_NODEDB_OK;
}

/* Uppercase-copy a node name (1..6 chars) into dst[NAMEMAX+1]. Empty name ok
 * (address-only entry). Returns 0 ok, -1 if too long / bad char. */
static int name_norm(const char *name, char dst[DNET_NODEDB_NAMEMAX + 1])
{
    dst[0] = '\0';
    if (!name || name[0] == '\0')
        return 0;
    size_t n = strlen(name);
    if (n > DNET_NODEDB_NAMEMAX)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c))
            return -1;                    /* DNA node names are alphanumeric */
        dst[i] = (char)toupper(c);
    }
    dst[n] = '\0';
    return 0;
}

void dnet_nodedb_init(struct dnet_nodedb *db)
{
    if (!db)
        return;
    memset(db, 0, sizeof(*db));
}

static struct dnet_node_entry *find_addr(struct dnet_nodedb *db, uint16_t addr)
{
    for (unsigned i = 0; i < DNET_NODEDB_MAX; i++)
        if (db->ent[i].in_use && db->ent[i].addr == addr)
            return &db->ent[i];
    return NULL;
}

static struct dnet_node_entry *find_name(struct dnet_nodedb *db,
                                         const char name[DNET_NODEDB_NAMEMAX + 1])
{
    if (name[0] == '\0')
        return NULL;
    for (unsigned i = 0; i < DNET_NODEDB_MAX; i++)
        if (db->ent[i].in_use && strcmp(db->ent[i].name, name) == 0)
            return &db->ent[i];
    return NULL;
}

int dnet_nodedb_set(struct dnet_nodedb *db, uint16_t addr, const char *name)
{
    if (!db || addr == 0)
        return DNET_NODEDB_EINVAL;
    char norm[DNET_NODEDB_NAMEMAX + 1];
    if (name_norm(name, norm) != 0)
        return DNET_NODEDB_EINVAL;

    /* A name must be unique: reject binding it to a second address. */
    struct dnet_node_entry *byname = find_name(db, norm);
    if (byname && byname->addr != addr)
        return DNET_NODEDB_EINVAL;

    struct dnet_node_entry *e = find_addr(db, addr);
    if (e) {                              /* update the name of an existing node */
        memcpy(e->name, norm, sizeof(norm));
        return DNET_NODEDB_OK;
    }
    for (unsigned i = 0; i < DNET_NODEDB_MAX; i++) {
        if (!db->ent[i].in_use) {
            db->ent[i].in_use = 1;
            db->ent[i].addr = addr;
            memcpy(db->ent[i].name, norm, sizeof(norm));
            db->count++;
            return DNET_NODEDB_OK;
        }
    }
    return DNET_NODEDB_EFULL;
}

int dnet_nodedb_clear_addr(struct dnet_nodedb *db, uint16_t addr)
{
    if (!db)
        return DNET_NODEDB_EINVAL;
    struct dnet_node_entry *e = find_addr(db, addr);
    if (!e)
        return DNET_NODEDB_ENOENT;
    memset(e, 0, sizeof(*e));
    db->count--;
    return DNET_NODEDB_OK;
}

int dnet_nodedb_clear_name(struct dnet_nodedb *db, const char *name)
{
    if (!db)
        return DNET_NODEDB_EINVAL;
    char norm[DNET_NODEDB_NAMEMAX + 1];
    if (name_norm(name, norm) != 0 || norm[0] == '\0')
        return DNET_NODEDB_EINVAL;
    struct dnet_node_entry *e = find_name(db, norm);
    if (!e)
        return DNET_NODEDB_ENOENT;
    memset(e, 0, sizeof(*e));
    db->count--;
    return DNET_NODEDB_OK;
}

const struct dnet_node_entry *dnet_nodedb_by_name(const struct dnet_nodedb *db,
                                                  const char *name)
{
    if (!db)
        return NULL;
    char norm[DNET_NODEDB_NAMEMAX + 1];
    if (name_norm(name, norm) != 0 || norm[0] == '\0')
        return NULL;
    return find_name((struct dnet_nodedb *)db, norm);
}

const struct dnet_node_entry *dnet_nodedb_by_addr(const struct dnet_nodedb *db,
                                                  uint16_t addr)
{
    if (!db)
        return NULL;
    return find_addr((struct dnet_nodedb *)db, addr);
}

const struct dnet_node_entry *dnet_nodedb_at(const struct dnet_nodedb *db, unsigned i)
{
    if (!db || i >= db->count)
        return NULL;
    /* Return the i-th in-use entry in ascending-address order (SHOW ordering).
     * O(n^2) over a small fixed table; no state, no allocation. */
    uint16_t prev = 0;
    int have_prev = 0;
    for (unsigned rank = 0; ; rank++) {
        const struct dnet_node_entry *best = NULL;
        for (unsigned j = 0; j < DNET_NODEDB_MAX; j++) {
            const struct dnet_node_entry *c = &db->ent[j];
            if (!c->in_use)
                continue;
            if (have_prev && (c->addr < prev || (c->addr == prev)))
                continue;            /* already emitted at-or-before prev */
            if (!best || c->addr < best->addr)
                best = c;
        }
        if (!best)
            return NULL;
        if (rank == i)
            return best;
        prev = best->addr;
        have_prev = 1;
    }
}

int dnet_nodedb_save(const struct dnet_nodedb *db, const char *path)
{
    if (!db || !path)
        return DNET_NODEDB_EINVAL;
    char tmp[1024];
    int m = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (m <= 0 || (size_t)m >= sizeof(tmp))
        return DNET_NODEDB_EIO;
    FILE *f = fopen(tmp, "w");
    if (!f)
        return DNET_NODEDB_EIO;
    fprintf(f, "# OVMX DECnet Phase IV node database (NCP DEFINE/SET NODE).\n");
    fprintf(f, "# Format: NODE <area>.<node> [NAME <name>]  -- OVMX layout, not"
               " VMS NETNODE_REMOTE.DAT.\n");
    for (unsigned i = 0; i < db->count; i++) {
        const struct dnet_node_entry *e = dnet_nodedb_at(db, i);
        if (!e)
            break;
        unsigned area = dnet_area_of(e->addr), node = dnet_node_of(e->addr);
        if (e->name[0])
            fprintf(f, "NODE %u.%u NAME %s\n", area, node, e->name);
        else
            fprintf(f, "NODE %u.%u\n", area, node);
    }
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        remove(tmp);
        return DNET_NODEDB_EIO;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return DNET_NODEDB_EIO;
    }
    return DNET_NODEDB_OK;
}

int dnet_nodedb_load(struct dnet_nodedb *db, const char *path)
{
    if (!db || !path)
        return DNET_NODEDB_EINVAL;
    dnet_nodedb_init(db);
    FILE *f = fopen(path, "r");
    if (!f)
        return DNET_NODEDB_OK;            /* absent DB == empty, not an error */
    char line[256];
    int rc = DNET_NODEDB_OK;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;
        char kw[16], astr[32], namekw[16], name[64];
        int nf = sscanf(p, "%15s %31s %15s %63s", kw, astr, namekw, name);
        if (nf < 2 || strcmp(kw, "NODE") != 0) {
            rc = DNET_NODEDB_EIO;
            break;
        }
        uint16_t addr = 0;
        if (dnet_nodedb_parse_addr(astr, &addr) != DNET_NODEDB_OK) {
            rc = DNET_NODEDB_EIO;
            break;
        }
        const char *nm = "";
        if (nf >= 4 && strcmp(namekw, "NAME") == 0)
            nm = name;
        if (dnet_nodedb_set(db, addr, nm) != DNET_NODEDB_OK) {
            rc = DNET_NODEDB_EIO;
            break;
        }
    }
    fclose(f);
    if (rc != DNET_NODEDB_OK)
        dnet_nodedb_init(db);
    return rc;
}
