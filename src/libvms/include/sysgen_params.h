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
 * ACP-backed parameter I/O (vms-5f0 residual, epic vms-208).
 *
 * SYS$SYSTEM:OVMXVMSSYS.PAR lives on the genuine Files-11 (ODS-2) system volume
 * the executive ACP owns. The atomic flip retired the /vms POSIX passthrough, so
 * the vmsfs_to_linux_path()+fopen() reads/writes below (kept ONLY for the
 * OVMX_SYSGEN_PATH test/staging override) no longer reach the runtime volume.
 * These two functions read/write the file over the ACP instead
 * (src/vmsrms/sysgen_acp.c, RMS-over-ACP).
 *
 * WEAK, exactly like sysuaf.c's ovmx_sysuaf_* seam: resolved in any image that
 * links LIBVMSRMS$SHR (SYSGEN.EXE, SCSD.EXE, SYSMAN.EXE, DCL.EXE, ...), NULL in
 * an image that does not (libvms's own inline callers) -- which then fails
 * honest, NEVER falling through to /vms (CLAUDE.md Rule 9 / INV-6).
 */
uint32_t ovmx_sysgen_acp_read(struct sysgen_file *out);
uint32_t ovmx_sysgen_acp_write(const struct sysgen_file *db, int new_version,
                               int *out_version);
/*
 * OVMX_SYSGEN_ACP_STRONG marks a TU that DEFINES (src/vmsrms/sysgen_acp.c) or
 * definitely LINKS (SYSGEN.EXE / SCSD.EXE / SYSMAN.EXE, via
 * target_compile_definitions) the ACP seam: there the references/definitions
 * stay STRONG. This matters two ways:
 *   - the defining TU's symbols must be 'T', not 'W' -- LIBVMSRMS$SHR's
 *     native-link symbol vector is generated from `nm ... type T`, so a weak
 *     definition would be dropped from the vector and DCL could not resolve it;
 *   - a consumer's reference must be strong, or `ld --as-needed` DROPS
 *     LIBVMSRMS$SHR (a weak reference does not mark a shared lib "needed") and
 *     the link fails %undefined -- and in a static link a weak reference does
 *     not pull the archive member either.
 * Everywhere else (libvms's own inline callers, which cannot link vmsrms) the
 * references are #pragma weak, so they see NULL and fail honest (Rule 9/INV-6)
 * instead of failing to link.
 */
#if defined(__GNUC__) && !defined(OVMX_SYSGEN_ACP_STRONG)
#pragma weak ovmx_sysgen_acp_read
#pragma weak ovmx_sysgen_acp_write
#endif

/*
 * sysgen_load_current_db - load the current parameter database (USE CURRENT
 * semantics) into *db. Honors the OVMX_SYSGEN_PATH literal-file override (tests,
 * and PID 1's staged-copy boot read); otherwise reads the HIGHEST version off
 * the ACP-mounted system volume through ovmx_sysgen_acp_read. Returns 0 on
 * success, -1 on any failure (missing file, no executive, bad magic/version) --
 * there is no /vms fallback.
 */
static inline int sysgen_load_current_db(struct sysgen_file *db)
{
    if (!db) return -1;

    const char *override = getenv("OVMX_SYSGEN_PATH");
    if (override && *override) {
        FILE *fp = fopen(override, "rb");
        if (!fp) return -1;
        int ok = (fread(db, sizeof(*db), 1, fp) == 1);
        fclose(fp);
        if (!ok || db->magic != SYSGEN_MAGIC || db->version != SYSGEN_VERSION)
            return -1;
        if (db->count > SYSGEN_MAX_PARAMS) db->count = SYSGEN_MAX_PARAMS;
        return 0;
    }

#if defined(OVMX_SYSGEN_ACP_STRONG)
    return $VMS_STATUS_SUCCESS(ovmx_sysgen_acp_read(db)) ? 0 : -1;
#else
    if (ovmx_sysgen_acp_read)
        return $VMS_STATUS_SUCCESS(ovmx_sysgen_acp_read(db)) ? 0 : -1;
    return -1;   /* no override and no ACP linked: fail honest (Rule 9) */
#endif
}

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
 * sysgen_read_param - Read a single numeric parameter value from the
 * current database.
 *
 * Returns 0 on success, -1 on error (file not found, parameter not
 * found, or parameter is not numeric-typed).
 */
static inline int sysgen_read_param(const char *name, uint32_t *value)
{
    if (!name || !value) return -1;

    struct sysgen_file db;
    if (sysgen_load_current_db(&db) != 0) return -1;

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

    struct sysgen_file db;
    if (sysgen_load_current_db(&db) != 0) return -1;

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

/*
 * sysgen_load_working - Load the FULL current parameter database into a
 * caller-supplied struct. This is the "work area" that SYSMAN's
 * PARAMETERS USE CURRENT fills (VSI OpenVMS System Management Utilities
 * Reference Manual, SYSMAN PARAMETERS USE: loads the current parameter
 * file into the work area). Honors OVMX_SYSGEN_PATH like the readers
 * above, and reads the HIGHEST version of SYS$SYSTEM:OVMXVMSSYS.PAR
 * otherwise (USE CURRENT semantics).
 *
 * Returns 0 on success, -1 if no current parameter file exists yet or it
 * is invalid (wrong magic/version).
 */
static inline int sysgen_load_working(struct sysgen_file *out)
{
    return sysgen_load_current_db(out);
}

/*
 * sysgen_commit_working - Commit a work area back to the parameter store.
 * This is the shared body of SYSMAN PARAMETERS WRITE {CURRENT|ACTIVE}
 * (and SYSGEN WRITE CURRENT); the messages stay in the callers.
 *
 * @new_version selects the WRITE target's shape:
 *   1 (CURRENT) -> mint a NEW file version of SYS$SYSTEM:OVMXVMSSYS.PAR
 *                  (highest existing + 1, via vmsfs -- the next-boot
 *                  parameter file; matches the oracle's ALPHAVMSSYS.PAR
 *                  versioning, docs/design-boot-faithful.md §3.4).
 *   0 (ACTIVE)  -> overwrite the HIGHEST EXISTING version in place, so the
 *                  change is what the running system's readers (F$GETSYI,
 *                  sysgen_read_param) see immediately, WITHOUT minting a new
 *                  boot generation. If no version exists yet, mint version 1.
 *
 * OVMX has a single parameter store (there is no separate in-memory active
 * parameter set distinct from the on-disk file), so "ACTIVE" here means
 * "update the store the running readers consult" -- labeled as an OVMX model
 * choice (Rule 8/INV-6), not a claim of a byte-for-byte VMS active-set copy.
 *
 * When OVMX_SYSGEN_PATH is set (tests / an explicit literal target) both
 * modes write that single unversioned file and report version 0.
 *
 * Returns a VMS status; on success writes the resolved Linux path to
 * @outpath and the resulting version number to *@outver (0 for the
 * OVMX_SYSGEN_PATH literal case).
 */
static inline int sysgen_commit_working(const struct sysgen_file *db,
                                        int new_version, char *outpath,
                                        size_t outlen, int *outver)
{
    if (!db) return SS$_BADPARAM;

    const char *override = getenv("OVMX_SYSGEN_PATH");
    if (override && *override) {
        FILE *fp = fopen(override, "wb");
        if (!fp) return SS$_NOSUCHFILE;
        if (fwrite(db, sizeof(*db), 1, fp) != 1) { fclose(fp); return SS$_ABORT; }
        fclose(fp);
        if (outpath && outlen) {
            strncpy(outpath, override, outlen - 1);
            outpath[outlen - 1] = '\0';
        }
        if (outver) *outver = 0;
        return SS$_NORMAL;
    }

    /*
     * ATOMIC FLIP (vms-5f0): persist to the genuine ODS-2 system volume over the
     * executive ACP, not a /vms fopen. CURRENT mints a new highest version,
     * ACTIVE overwrites the highest in place -- both in ovmx_sysgen_acp_write.
     * Fail honest if RMS is not linked into this image (Rule 9/INV-6): there is
     * no /vms fallback.
     */
#if !defined(OVMX_SYSGEN_ACP_STRONG)
    if (!ovmx_sysgen_acp_write)
        return SS$_NOSUCHDEV;
#endif

    int version = 0;
    uint32_t st = ovmx_sysgen_acp_write(db, new_version, &version);
    if (!$VMS_STATUS_SUCCESS(st)) return st;

    if (outpath && outlen) {
        int n = snprintf(outpath, outlen, "%s;%d", VMS_PARAMS_PATH, version);
        if (n <= 0 || (size_t)n >= outlen) outpath[0] = '\0';
    }
    if (outver) *outver = version;
    return SS$_NORMAL;
}

#endif /* SYSGEN_PARAMS_H */
