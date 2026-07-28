/*
 * sysgen_params.h - VMS SYSGEN parameter database definitions
 *
 * Defines the binary format for the system parameter database used by
 * SYSGEN.EXE, and provides reader functions for other OVMX components
 * to query current parameter values.
 *
 * v2 (vms-ci.8): adds a TYPE tag (numeric vs. string) so the same
 * database can carry cluster node-identity parameters (SCSNODE, ...)
 * alongside the existing numeric SYSGEN parameters. The version bump
 * is a clean format cut — there is no persisted v1 production data
 * (the v1 reader was dead code), so no migration path is needed.
 */
#ifndef SYSGEN_PARAMS_H
#define SYSGEN_PARAMS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define SYSGEN_MAGIC        0x53595347  /* "SYSG" */
#define SYSGEN_VERSION      2            /* v2: typed (numeric/string) params */
#define SYSGEN_MAX_PARAMS   64
#define SYSGEN_DEFAULT_PATH "/etc/ovmx/sysparams.dat"

/* Parameter flags */
#define SYSGEN_F_DYNAMIC        0x01    /* Can be changed without reboot */
#define SYSGEN_F_INFORMATIONAL  0x02    /* Read-only informational parameter */

/* Parameter value type */
#define SYSGEN_TYPE_NUMERIC     0        /* uses current/default/min/max */
#define SYSGEN_TYPE_STRING      1        /* uses str_current/str_default */

/*
 * Max stored length (incl. NUL) for a string-typed parameter value.
 * Sized for VMS node-name-class strings (SCSNODE is max 6 chars).
 */
#define SYSGEN_STRVAL_LEN   8

struct sysgen_param {
    char        name[32];
    uint32_t    current;
    uint32_t    default_val;
    uint32_t    min_val;
    uint32_t    max_val;
    uint8_t     flags;          /* SYSGEN_F_DYNAMIC, SYSGEN_F_INFORMATIONAL */
    char        description[80];
    /* --- v2 additions (vms-ci.8) --- */
    uint8_t     type;                        /* SYSGEN_TYPE_NUMERIC (default) or _STRING */
    char        str_current[SYSGEN_STRVAL_LEN];
    char        str_default[SYSGEN_STRVAL_LEN];
};

struct sysgen_file {
    uint32_t    magic;
    uint32_t    version;
    uint32_t    count;
    struct sysgen_param params[SYSGEN_MAX_PARAMS];
};

/*
 * sysgen_db_path - Resolve the path to the SYSGEN parameter database.
 *
 * Honors OVMX_SYSGEN_PATH when set, so tests and tools can point the
 * readers at an alternate file without needing write access to
 * /etc/ovmx (which requires root). Production code paths never set
 * this env var, so behavior there is unchanged.
 */
static inline const char *sysgen_db_path(void)
{
    const char *p = getenv("OVMX_SYSGEN_PATH");
    return (p && *p) ? p : SYSGEN_DEFAULT_PATH;
}

/*
 * sysgen_read_param - Read a single numeric parameter value from the
 * current database.
 *
 * Returns 0 on success, -1 on error (file not found, parameter not
 * found, or parameter is not numeric-typed).
 */
static inline int sysgen_read_param(const char *name, uint32_t *value)
{
    if (!name || !value) return -1;

    FILE *fp = fopen(sysgen_db_path(), "rb");
    if (!fp) return -1;

    struct sysgen_file db;
    if (fread(&db, sizeof(db), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (db.magic != SYSGEN_MAGIC || db.version != SYSGEN_VERSION)
        return -1;

    for (uint32_t i = 0; i < db.count && i < SYSGEN_MAX_PARAMS; i++) {
        if (strncasecmp(db.params[i].name, name, 32) == 0) {
            if (db.params[i].type != SYSGEN_TYPE_NUMERIC) return -1;
            *value = db.params[i].current;
            return 0;
        }
    }
    return -1;
}

/*
 * sysgen_read_string - Read a single string-typed parameter value from
 * the current database (e.g. SCSNODE).
 *
 * Returns 0 on success, -1 on error (file not found, parameter not
 * found, or parameter is not string-typed).
 */
static inline int sysgen_read_string(const char *name, char *buf, size_t buflen)
{
    if (!name || !buf || buflen == 0) return -1;

    FILE *fp = fopen(sysgen_db_path(), "rb");
    if (!fp) return -1;

    struct sysgen_file db;
    if (fread(&db, sizeof(db), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    if (db.magic != SYSGEN_MAGIC || db.version != SYSGEN_VERSION)
        return -1;

    for (uint32_t i = 0; i < db.count && i < SYSGEN_MAX_PARAMS; i++) {
        if (strncasecmp(db.params[i].name, name, 32) == 0) {
            if (db.params[i].type != SYSGEN_TYPE_STRING) return -1;
            strncpy(buf, db.params[i].str_current, buflen - 1);
            buf[buflen - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

#endif /* SYSGEN_PARAMS_H */
