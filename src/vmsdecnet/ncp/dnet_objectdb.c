/*
 * dnet_objectdb.c - the persisted DECnet OBJECT database (rd vms-f52). Read
 * dnet_objectdb.h first. This is a direct parallel of dnet_nodedb.c: pure table
 * logic over a documented plain-text file, keyed by object NUMBER (as the node
 * table is keyed by address), with a unique optional NAME and an optional
 * servicing FILE. No socket, no engine, no /dev/vms -- unit-testable in full.
 */

#include "dnet_objectdb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>   /* strtol */
#include <string.h>

/* Normalise an object name: uppercase, DNA object characters only (alphanumeric
 * plus '$' and '_', as real object/image names use). Empty name is allowed
 * (a number-only object). Returns 0 ok, -1 if too long / bad char. */
static int name_norm(const char *name, char dst[DNET_OBJECTDB_NAMEMAX + 1])
{
    dst[0] = '\0';
    if (!name || name[0] == '\0')
        return 0;
    size_t n = strlen(name);
    if (n > DNET_OBJECTDB_NAMEMAX)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '$' && c != '_')
            return -1;
        dst[i] = (char)toupper(c);
    }
    dst[n] = '\0';
    return 0;
}

/* A servicing filespec is stored verbatim (case-preserving) but must be a
 * single whitespace-free, control-free token so the line-based format stays
 * parseable. Empty is allowed. Returns 0 ok, -1 if too long / bad char. */
static int file_norm(const char *file, char dst[DNET_OBJECTDB_FILEMAX + 1])
{
    dst[0] = '\0';
    if (!file || file[0] == '\0')
        return 0;
    size_t n = strlen(file);
    if (n > DNET_OBJECTDB_FILEMAX)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)file[i];
        if (c <= ' ' || c == 0x7f)
            return -1;                    /* no whitespace/control in a token   */
    }
    memcpy(dst, file, n + 1);
    return 0;
}

void dnet_objectdb_init(struct dnet_objectdb *db)
{
    if (!db)
        return;
    memset(db, 0, sizeof(*db));
}

static struct dnet_object_entry *find_number(struct dnet_objectdb *db,
                                             uint8_t number)
{
    for (unsigned i = 0; i < DNET_OBJECTDB_MAX; i++)
        if (db->ent[i].in_use && db->ent[i].number == number)
            return &db->ent[i];
    return NULL;
}

static struct dnet_object_entry *find_name(struct dnet_objectdb *db,
                                           const char name[DNET_OBJECTDB_NAMEMAX + 1])
{
    if (name[0] == '\0')
        return NULL;
    for (unsigned i = 0; i < DNET_OBJECTDB_MAX; i++)
        if (db->ent[i].in_use && strcmp(db->ent[i].name, name) == 0)
            return &db->ent[i];
    return NULL;
}

int dnet_objectdb_set(struct dnet_objectdb *db, uint8_t number,
                      const char *name, const char *file)
{
    if (!db || number == 0)
        return DNET_OBJECTDB_EINVAL;
    char norm[DNET_OBJECTDB_NAMEMAX + 1];
    char fnorm[DNET_OBJECTDB_FILEMAX + 1];
    if (name_norm(name, norm) != 0)
        return DNET_OBJECTDB_EINVAL;
    if (file_norm(file, fnorm) != 0)
        return DNET_OBJECTDB_EINVAL;

    /* A name must be unique: reject binding it to a second object number. */
    struct dnet_object_entry *byname = find_name(db, norm);
    if (byname && byname->number != number)
        return DNET_OBJECTDB_EINVAL;

    struct dnet_object_entry *e = find_number(db, number);
    if (e) {                              /* update name + file of an existing  */
        memcpy(e->name, norm, sizeof(norm));
        memcpy(e->file, fnorm, sizeof(fnorm));
        return DNET_OBJECTDB_OK;
    }
    for (unsigned i = 0; i < DNET_OBJECTDB_MAX; i++) {
        if (!db->ent[i].in_use) {
            db->ent[i].in_use = 1;
            db->ent[i].number = number;
            memcpy(db->ent[i].name, norm, sizeof(norm));
            memcpy(db->ent[i].file, fnorm, sizeof(fnorm));
            db->count++;
            return DNET_OBJECTDB_OK;
        }
    }
    return DNET_OBJECTDB_EFULL;
}

int dnet_objectdb_clear_number(struct dnet_objectdb *db, uint8_t number)
{
    if (!db)
        return DNET_OBJECTDB_EINVAL;
    struct dnet_object_entry *e = find_number(db, number);
    if (!e)
        return DNET_OBJECTDB_ENOENT;
    memset(e, 0, sizeof(*e));
    if (db->count)
        db->count--;
    return DNET_OBJECTDB_OK;
}

int dnet_objectdb_clear_name(struct dnet_objectdb *db, const char *name)
{
    if (!db)
        return DNET_OBJECTDB_EINVAL;
    char norm[DNET_OBJECTDB_NAMEMAX + 1];
    if (name_norm(name, norm) != 0 || norm[0] == '\0')
        return DNET_OBJECTDB_EINVAL;
    struct dnet_object_entry *e = find_name(db, norm);
    if (!e)
        return DNET_OBJECTDB_ENOENT;
    memset(e, 0, sizeof(*e));
    if (db->count)
        db->count--;
    return DNET_OBJECTDB_OK;
}

const struct dnet_object_entry *dnet_objectdb_by_number(const struct dnet_objectdb *db,
                                                        uint8_t number)
{
    if (!db)
        return NULL;
    return find_number((struct dnet_objectdb *)db, number);
}

const struct dnet_object_entry *dnet_objectdb_by_name(const struct dnet_objectdb *db,
                                                      const char *name)
{
    char norm[DNET_OBJECTDB_NAMEMAX + 1];
    if (!db || name_norm(name, norm) != 0 || norm[0] == '\0')
        return NULL;
    return find_name((struct dnet_objectdb *)db, norm);
}

const struct dnet_object_entry *dnet_objectdb_at(const struct dnet_objectdb *db,
                                                 unsigned i)
{
    if (!db)
        return NULL;
    /* The i-th in-use entry in ascending-NUMBER order (SHOW ordering), without
     * assuming array order -- the same selection the node table's _at uses. */
    unsigned seen = 0;
    int have_prev = 0;
    unsigned prev = 0;
    const struct dnet_object_entry *best_for_i = NULL;
    for (;;) {
        const struct dnet_object_entry *best = NULL;
        for (unsigned j = 0; j < DNET_OBJECTDB_MAX; j++) {
            const struct dnet_object_entry *c = &db->ent[j];
            if (!c->in_use)
                continue;
            if (have_prev && c->number <= prev)
                continue;
            if (!best || c->number < best->number)
                best = c;
        }
        if (!best)
            return NULL;
        if (seen == i) {
            best_for_i = best;
            break;
        }
        prev = best->number;
        have_prev = 1;
        seen++;
    }
    return best_for_i;
}

const char *const DNET_OBJECTDB_HEADER[2] = {
    "# OVMX DECnet Phase IV object database (NCP DEFINE/SET OBJECT).",
    "# Format: OBJECT <number> [NAME <name>] [FILE <spec>]  -- OVMX"
    " layout, not the VMS permanent object database.",
};

int dnet_objectdb_format_entry(const struct dnet_object_entry *e, char *buf,
                               size_t bufsz)
{
    if (!e || !buf || bufsz == 0)
        return DNET_OBJECTDB_EINVAL;
    int m = snprintf(buf, bufsz, "OBJECT %u%s%s%s%s", (unsigned)e->number,
                     e->name[0] ? " NAME " : "", e->name,
                     e->file[0] ? " FILE " : "", e->file);
    return (m > 0 && (size_t)m < bufsz) ? DNET_OBJECTDB_OK : DNET_OBJECTDB_EIO;
}

int dnet_objectdb_apply_line(struct dnet_objectdb *db, const char *raw)
{
    if (!db || !raw)
        return DNET_OBJECTDB_EINVAL;
    char line[512];
    snprintf(line, sizeof(line), "%s", raw);
    char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '#' || *p == '\n' || *p == '\0' || *p == '\r')
        return DNET_OBJECTDB_OK;          /* comment / blank: nothing to apply */

    char *save = NULL;
    char *kw = strtok_r(p, " \t\r\n", &save);
    char *numtok = strtok_r(NULL, " \t\r\n", &save);
    if (!kw || strcmp(kw, "OBJECT") != 0 || !numtok)
        return DNET_OBJECTDB_EIO;
    /* object number: 1..255 */
    char *end = NULL;
    long num = strtol(numtok, &end, 10);
    if (!end || *end != '\0' || num < 1 || num > 255)
        return DNET_OBJECTDB_EIO;
    const char *name = "";
    const char *file = "";
    char *tok;
    while ((tok = strtok_r(NULL, " \t\r\n", &save)) != NULL) {
        char *val = strtok_r(NULL, " \t\r\n", &save);
        if (!val)
            return DNET_OBJECTDB_EIO;
        if (strcmp(tok, "NAME") == 0)
            name = val;
        else if (strcmp(tok, "FILE") == 0)
            file = val;
        else
            return DNET_OBJECTDB_EIO;
    }
    if (dnet_objectdb_set(db, (uint8_t)num, name, file) != DNET_OBJECTDB_OK)
        return DNET_OBJECTDB_EIO;
    return DNET_OBJECTDB_OK;
}

int dnet_objectdb_save(const struct dnet_objectdb *db, const char *path)
{
    if (!db || !path)
        return DNET_OBJECTDB_EINVAL;
    char tmp[1024];
    int m = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (m <= 0 || (size_t)m >= sizeof(tmp))
        return DNET_OBJECTDB_EIO;
    FILE *f = fopen(tmp, "w");
    if (!f)
        return DNET_OBJECTDB_EIO;
    fprintf(f, "%s\n%s\n", DNET_OBJECTDB_HEADER[0], DNET_OBJECTDB_HEADER[1]);
    for (unsigned i = 0; i < db->count; i++) {
        const struct dnet_object_entry *e = dnet_objectdb_at(db, i);
        char line[512];
        if (!e)
            break;
        if (dnet_objectdb_format_entry(e, line, sizeof(line)) != DNET_OBJECTDB_OK) {
            fclose(f);
            remove(tmp);
            return DNET_OBJECTDB_EIO;
        }
        fprintf(f, "%s\n", line);
    }
    if (fflush(f) != 0 || ferror(f)) {
        fclose(f);
        remove(tmp);
        return DNET_OBJECTDB_EIO;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return DNET_OBJECTDB_EIO;
    }
    return DNET_OBJECTDB_OK;
}

int dnet_objectdb_load(struct dnet_objectdb *db, const char *path)
{
    if (!db || !path)
        return DNET_OBJECTDB_EINVAL;
    dnet_objectdb_init(db);
    FILE *f = fopen(path, "r");
    if (!f)
        return DNET_OBJECTDB_OK;           /* absent DB == empty, not an error  */
    char line[512];
    int rc = DNET_OBJECTDB_OK;
    while (fgets(line, sizeof(line), f)) {
        if (dnet_objectdb_apply_line(db, line) != DNET_OBJECTDB_OK) {
            rc = DNET_OBJECTDB_EIO;
            break;
        }
    }
    fclose(f);
    if (rc != DNET_OBJECTDB_OK)
        dnet_objectdb_init(db);
    return rc;
}
