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
 *
 * vms-d34: the store moves off the bare Linux path /etc/ovmx/sysparams.dat
 * onto the system disk as SYS$SYSTEM:OVMXVMSSYS.PAR (ovmx_layout.h's
 * VMS_PARAMS_PATH), read through vmsfs and versioned like the oracle's
 * ALPHAVMSSYS.PAR (WRITE creates a new highest version; USE reads the
 * highest — docs/design-boot-faithful.md §3.4). The BINARY LAYOUT below
 * (struct sysgen_param / sysgen_file) is unchanged by that move and remains
 * an OVMX INVENTION, not a VMS-authentic byte format — CLAUDE.md Rule 8:
 * ALPHAVMSSYS.PAR's internal layout is not published anywhere OVMX may
 * read, so OVMX defines and labels its own here. Only the FILE NAME SHAPE
 * and the VERSIONING BEHAVIOR are observed facts being matched.
 */
#ifndef SYSGEN_PARAMS_H
#define SYSGEN_PARAMS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ovmx_layout.h"
#include "ssdef.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"

#define SYSGEN_MAGIC        0x53595347  /* "SYSG" */
#define SYSGEN_VERSION      2            /* v2: typed (numeric/string) params */
#define SYSGEN_MAX_PARAMS   64

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
 * sysgen_split_dir_base - Split a Linux path into its directory and its
 * name/ext parts (the shape vmsfs_get_highest_version() wants). Mirrors the
 * pattern already used at the RMS/DCL layer (rms_core.c's rms_next_version,
 * dcl_filespec.c's resolve_version_suffix).
 */
static inline void sysgen_split_dir_base(const char *linux_path, char *dir,
                                          size_t dirlen, char *name,
                                          size_t namelen, char *ext,
                                          size_t extlen)
{
    const char *slash = strrchr(linux_path, '/');
    const char *base = slash ? slash + 1 : linux_path;

    if (slash) {
        size_t dlen = (size_t)(slash - linux_path);
        if (dlen >= dirlen) dlen = dirlen - 1;
        memcpy(dir, linux_path, dlen);
        dir[dlen] = '\0';
    } else {
        dir[0] = '\0';
    }

    char basebuf[VMSFS_MAX_NAME + VMSFS_MAX_TYPE + 2];
    strncpy(basebuf, base, sizeof(basebuf) - 1);
    basebuf[sizeof(basebuf) - 1] = '\0';

    char *dot = strrchr(basebuf, '.');
    if (dot) {
        *dot = '\0';
        strncpy(ext, dot + 1, extlen - 1);
        ext[extlen - 1] = '\0';
    } else {
        ext[0] = '\0';
    }
    strncpy(name, basebuf, namelen - 1);
    name[namelen - 1] = '\0';
}

/*
 * sysgen_current_path - Resolve the Linux path to READ the current SYSGEN
 * parameter database (USE CURRENT's semantics: highest existing version).
 *
 * Honors OVMX_SYSGEN_PATH when set: tests and tools point the readers at a
 * private, literal, unversioned temp file (no /vms mount needed). Production
 * code paths never set this env var: the store resolves through vmsfs to
 * SYS$SYSTEM:OVMXVMSSYS.PAR's directory (ovmx_layout.h's VMS_PARAMS_PATH)
 * and reads the HIGHEST version on disk there, matching the oracle's
 * ALPHAVMSSYS.PAR behavior (docs/design-boot-faithful.md §3.4).
 *
 * Returns 0 on success (path written to buf), -1 if no version of the file
 * exists yet or the filespec cannot be translated.
 */
static inline int sysgen_current_path(char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return -1;

    const char *override = getenv("OVMX_SYSGEN_PATH");
    if (override && *override) {
        strncpy(buf, override, buflen - 1);
        buf[buflen - 1] = '\0';
        return 0;
    }

    char linux_path[VMSFS_MAX_PATH];
    if (!$VMS_STATUS_SUCCESS(vmsfs_to_linux_path(VMS_PARAMS_PATH, linux_path,
                                                  sizeof(linux_path)))) {
        return -1;
    }

    char dir[VMSFS_MAX_PATH];
    char name[VMSFS_MAX_NAME + 1];
    char ext[VMSFS_MAX_TYPE + 1];
    sysgen_split_dir_base(linux_path, dir, sizeof(dir), name, sizeof(name),
                          ext, sizeof(ext));

    int highest = vmsfs_get_highest_version(dir[0] ? dir : ".", name, ext);
    if (highest < 1) return -1;

    int n = snprintf(buf, buflen, "%s/%s.%s;%d", dir, name, ext, highest);
    return (n > 0 && (size_t)n < buflen) ? 0 : -1;
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

    char path[VMSFS_MAX_PATH];
    if (sysgen_current_path(path, sizeof(path)) != 0) return -1;

    FILE *fp = fopen(path, "rb");
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

    char path[VMSFS_MAX_PATH];
    if (sysgen_current_path(path, sizeof(path)) != 0) return -1;

    FILE *fp = fopen(path, "rb");
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
