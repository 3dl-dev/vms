/*
 * rms_core.c - RMS Core File Operations
 *
 * Implements $OPEN, $CLOSE, $CREATE, $ERASE, $CONNECT, $DISCONNECT,
 * $DISPLAY, $REWIND, and $FLUSH system services.
 *
 * File metadata (record format, organization, etc.) is stored in
 * companion .rms_meta sidecar files alongside the data files.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include "rms/rms.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"
#include "ovmx_layout.h"

/* Mutex protecting internal file/stream identifier counters */
static pthread_mutex_t rms_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Protection functions from vmsfs */
extern uint16_t vmsfs_mode_to_protection(mode_t mode);
extern mode_t   vmsfs_protection_to_mode(uint16_t vms_prot);

/* Security check from libvms */
extern int vms$check_access(uint32_t caller_uic, uint32_t owner_uic,
                            uint32_t protection, int access_type);

/* Protection access type flags (matching sys_security.c) */
#define RMS_PROT_READ    0x08
#define RMS_PROT_WRITE   0x04
#define RMS_PROT_EXECUTE 0x02
#define RMS_PROT_DELETE  0x01

/* RMS metadata sidecar filename suffix */
#define RMS_SIDECAR_SUFFIX ".rms_meta"

/* Index sidecar suffix for indexed files */
#define RMS_INDEX_SUFFIX   ".rms_idx"

/*
 * Internal: RMS file metadata stored in sidecar file.
 * This persists the record format and file organization
 * so that subsequent opens can restore the correct behavior.
 */
#define RMS_META_MAGIC  0x524D5331  /* "RMS1" */
#define RMS_META_VERSION 1

struct rms_metadata {
    uint32_t magic;         /* RMS_META_MAGIC */
    uint8_t  version;       /* Metadata format version */
    uint8_t  org;           /* File organization */
    uint8_t  rfm;           /* Record format */
    uint8_t  rat;           /* Record attributes */
    uint16_t mrs;           /* Maximum record size */
    uint8_t  fsz;           /* Fixed control area size (VFC) */
    uint8_t  reserved1;
    uint32_t mrn;           /* Maximum record number (relative) */
    uint8_t  num_keys;      /* Number of keys (indexed) */
    uint8_t  reserved2[3];
    /* Key definitions follow for indexed files */
};

/* Per-key metadata stored in the sidecar */
struct rms_key_meta {
    uint8_t  ref;           /* Key of reference */
    uint8_t  dtp;           /* Data type */
    uint16_t flg;           /* Flags */
    uint8_t  nseg;          /* Number of segments */
    uint8_t  reserved[3];
    uint16_t pos[8];        /* Segment positions */
    uint8_t  siz[8];        /* Segment sizes */
};

/*
 * Local helper functions bridging RMS needs to vmsfs APIs.
 */

/* Strip version (";N") suffix from a filename */
static int rms_strip_version(const char *filename, char *out, size_t outlen)
{
    const char *semi = strrchr(filename, ';');
    size_t len = semi ? (size_t)(semi - filename) : strlen(filename);
    if (len >= outlen) len = outlen - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
    return 0;
}

/*
 * rms_validate_path_boundary - Ensure a resolved path stays within VMS root.
 *
 * Canonicalizes the path with realpath() and verifies it starts with
 * SYSDISK_MOUNT ("/vms"). For paths to files that don't yet exist,
 * canonicalizes the parent directory instead.
 *
 * Returns 0 if the path is within the VMS root, -1 if it escapes.
 */
static int rms_validate_path_boundary(const char *path)
{
    if (!path || !path[0])
        return -1;

    /* Only validate absolute paths under VMS root */
    if (path[0] != '/')
        return 0;

    char resolved[PATH_MAX];
    if (realpath(path, resolved) != NULL) {
        /* File exists — check the canonical path */
        if (strncmp(resolved, SYSDISK_MOUNT, strlen(SYSDISK_MOUNT)) != 0)
            return -1;
        /* Ensure it's actually under /vms and not just /vmsXYZ */
        size_t mount_len = strlen(SYSDISK_MOUNT);
        if (resolved[mount_len] != '\0' && resolved[mount_len] != '/')
            return -1;
        return 0;
    }

    /*
     * File doesn't exist yet (e.g., $CREATE) — canonicalize the parent
     * directory and verify it's within the VMS root.
     */
    char pathcopy[PATH_MAX];
    strncpy(pathcopy, path, sizeof(pathcopy) - 1);
    pathcopy[sizeof(pathcopy) - 1] = '\0';

    /* Find last slash to get parent directory */
    char *last_slash = strrchr(pathcopy, '/');
    if (!last_slash || last_slash == pathcopy) {
        /* Root-level path or no slash — not under /vms */
        return -1;
    }
    *last_slash = '\0';

    if (realpath(pathcopy, resolved) != NULL) {
        if (strncmp(resolved, SYSDISK_MOUNT, strlen(SYSDISK_MOUNT)) != 0)
            return -1;
        size_t mount_len = strlen(SYSDISK_MOUNT);
        if (resolved[mount_len] != '\0' && resolved[mount_len] != '/')
            return -1;
        return 0;
    }

    /* Parent doesn't exist either — reject */
    return -1;
}

/* Get next version number for a file in a directory */
static int rms_next_version(const char *dir, const char *basename_noversion)
{
    /* Split basename into name and ext for vmsfs API */
    char name[256], ext[64] = "";
    strncpy(name, basename_noversion, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *dot = strrchr(name, '.');
    if (dot) {
        strncpy(ext, dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
        *dot = '\0';
    }
    int highest = vmsfs_get_highest_version(dir, name, ext);
    return (highest > 0) ? highest + 1 : 1;
}

/* Resolve version of a full path: find highest existing version.
 * If found, write resolved path to out buffer. Returns 0 on success. */
static int rms_resolve_version(const char *path, char *out, size_t outlen)
{
    char dir[1024] = ".";
    char base[256];
    const char *slash = strrchr(path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        strncpy(base, slash + 1, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    } else {
        strncpy(base, path, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }
    /* Strip existing version to get name.ext */
    char noversion[256];
    rms_strip_version(base, noversion, sizeof(noversion));
    char name[256], ext[64] = "";
    strncpy(name, noversion, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char *dot = strrchr(name, '.');
    if (dot) {
        strncpy(ext, dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
        *dot = '\0';
    }
    int ver = vmsfs_get_highest_version(dir, name, ext);
    if (ver < 1) ver = 1;
    snprintf(out, outlen, "%s/%s%s;%d", dir, name, ext, ver);
    return 0;
}

/* Merge a filespec with a default spec (simple: just use the spec as-is
 * if it's complete, otherwise prefix with default_spec's directory). */
static int rms_resolve_spec(const char *spec, const char *default_spec,
                            char *result, size_t resultlen)
{
    (void)default_spec;
    strncpy(result, spec, resultlen - 1);
    result[resultlen - 1] = '\0';
    return 0;
}

/*
 * rms_get_session_uic - Get the current session UIC from env vars.
 * Returns packed UIC: (group << 16) | member
 */
static uint32_t rms_get_session_uic(void)
{
    const char *grp = getenv("VMS_UIC_GROUP");
    const char *mem = getenv("VMS_UIC_MEMBER");
    uint16_t group = grp ? (uint16_t)strtoul(grp, NULL, 10) : (uint16_t)(getgid() & 0xFFFF);
    uint16_t member = mem ? (uint16_t)strtoul(mem, NULL, 10) : (uint16_t)(getuid() & 0xFFFF);
    return ((uint32_t)group << 16) | (uint32_t)member;
}

/*
 * rms_check_protection - Check VMS-style protection for a file.
 * Returns 1 if access granted, 0 if denied.
 */
static int rms_check_protection(const char *path, int access_type)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 1;  /* Can't stat, let open() handle the error */
    }

    uint16_t vms_prot = vmsfs_mode_to_protection(st.st_mode);
    uint32_t owner_uic = ((uint32_t)(st.st_gid & 0xFFFF) << 16) |
                          (uint32_t)(st.st_uid & 0xFFFF);
    uint32_t my_uic = rms_get_session_uic();

    return vms$check_access(my_uic, owner_uic, (uint32_t)vms_prot, access_type);
}

/*
 * rms_get_default_protection - Get default protection for new files.
 * Reads VMS_DEFAULT_PROTECTION env var, or returns S:RWED,O:RWED (0xFF00).
 */
static uint16_t rms_get_default_protection(void)
{
    /* Default: S:RWED,O:RWED,G:,W: = system and owner full, group and world none
     * In VMS bit encoding: S=0x00 (all allowed), O=0x00, G=0x0F (all denied), W=0x0F
     * = 0xFF00 */
    return 0xFF00;
}

/*
 * resolve_filename - Extract and resolve the filename from a FAB.
 *
 * Parses the filename from fab$l_fna, resolves VMS filespecs
 * to Linux paths, and resolves file versions. The result is
 * stored in fab->_resolved_path.
 *
 * Returns 0 on success, -1 on failure.
 */
static int resolve_filename(struct FAB *fab)
{
    char spec[1024] = "";

    if (fab->fab$l_fna && fab->fab$b_fns > 0) {
        size_t len = fab->fab$b_fns;
        if (len >= sizeof(spec)) len = sizeof(spec) - 1;
        memcpy(spec, fab->fab$l_fna, len);
        spec[len] = '\0';
    } else {
        return -1;
    }

    /* Apply defaults from fab$l_dna if present */
    if (fab->fab$l_dna && fab->fab$b_dns > 0) {
        char default_spec[1024] = "";
        size_t dlen = fab->fab$b_dns;
        if (dlen >= sizeof(default_spec)) dlen = sizeof(default_spec) - 1;
        memcpy(default_spec, fab->fab$l_dna, dlen);
        default_spec[dlen] = '\0';

        char combined[1024];
        if (rms_resolve_spec(spec, default_spec, combined, sizeof(combined)) == 0) {
            strncpy(spec, combined, sizeof(spec) - 1);
            spec[sizeof(spec) - 1] = '\0';
        }
    }

    /* Check if it's a VMS filespec (contains : or [ — true VMS indicators).
     * A lone ';' (version number) is handled by the VMS translation path
     * but does NOT indicate a user-supplied VMS filespec for security purposes. */
    int is_vms_spec = (strchr(spec, ':') != NULL || strchr(spec, '[') != NULL);
    if (is_vms_spec || strchr(spec, ';')) {
        char linux_path[1024];
        if (vmsfs_to_linux_path(spec, linux_path, sizeof(linux_path)) == 0) {
            strncpy(fab->_resolved_path, linux_path,
                    sizeof(fab->_resolved_path) - 1);
            fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
        } else {
            strncpy(fab->_resolved_path, spec,
                    sizeof(fab->_resolved_path) - 1);
            fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
        }
        /* Security: verify VMS-translated paths stay within VMS root.
         * Only enforce boundary for true VMS filespecs (with : or [),
         * not for paths that just have a version number (;N). */
        if (is_vms_spec && fab->_resolved_path[0] == '/' &&
            rms_validate_path_boundary(fab->_resolved_path) != 0) {
            fab->_resolved_path[0] = '\0';
            return -1;
        }
    } else {
        /* Raw Linux path (no VMS translation) — pass through without
         * boundary check since this is a direct path, not user-supplied
         * VMS filespec that could contain traversal tricks. */
        strncpy(fab->_resolved_path, spec,
                sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
    }

    return 0;
}

/*
 * resolve_for_open - Resolve the filename for open, including version.
 * Returns 0 on success.
 */
static int resolve_for_open(struct FAB *fab)
{
    if (resolve_filename(fab) < 0) return -1;

    /* Resolve version (;0 or unspecified -> highest existing) */
    char resolved[1024];
    if (rms_resolve_version(fab->_resolved_path, resolved,
                              sizeof(resolved)) == 0) {
        strncpy(fab->_resolved_path, resolved,
                sizeof(fab->_resolved_path) - 1);
        fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';
    }

    return 0;
}

/*
 * load_metadata - Load RMS metadata from the sidecar file.
 *
 * If the sidecar exists, loads record format, organization,
 * MRS, etc. into the FAB. If no sidecar exists, the FAB
 * retains its current settings (which may be defaults).
 */
static void load_metadata(struct FAB *fab)
{
    char sidecar[1088];
    snprintf(sidecar, sizeof(sidecar), "%s%s",
             fab->_resolved_path, RMS_SIDECAR_SUFFIX);

    FILE *f = fopen(sidecar, "rb");
    if (!f) return;

    struct rms_metadata meta;
    if (fread(&meta, sizeof(meta), 1, f) == 1) {
        if (meta.magic == RMS_META_MAGIC && meta.version == RMS_META_VERSION) {
            fab->fab$b_org = meta.org;
            fab->fab$b_rfm = meta.rfm;
            fab->fab$w_mrs = meta.mrs;
            fab->fab$l_mrn = meta.mrn;
            fab->fab$b_rat = meta.rat;
            fab->fab$b_fsz = meta.fsz;

            /* Load key definitions into XAB chain if indexed file */
            if (meta.org == FAB$C_IDX && meta.num_keys > 0 &&
                fab->fab$l_xab != NULL) {
                struct XABKEY *xab = fab->fab$l_xab;
                for (uint8_t k = 0; k < meta.num_keys && xab; k++) {
                    if (xab->xab$b_cod != XAB$C_KEY) break;
                    struct rms_key_meta kmeta;
                    if (fread(&kmeta, sizeof(kmeta), 1, f) != 1) break;
                    xab->xab$b_ref = kmeta.ref;
                    xab->xab$b_dtp = kmeta.dtp;
                    xab->xab$w_flg = kmeta.flg;
                    xab->xab$b_nseg = kmeta.nseg;
                    xab->xab$w_pos0 = kmeta.pos[0];
                    xab->xab$w_pos1 = kmeta.pos[1];
                    xab->xab$w_pos2 = kmeta.pos[2];
                    xab->xab$w_pos3 = kmeta.pos[3];
                    xab->xab$w_pos4 = kmeta.pos[4];
                    xab->xab$w_pos5 = kmeta.pos[5];
                    xab->xab$w_pos6 = kmeta.pos[6];
                    xab->xab$w_pos7 = kmeta.pos[7];
                    xab->xab$b_siz0 = kmeta.siz[0];
                    xab->xab$b_siz1 = kmeta.siz[1];
                    xab->xab$b_siz2 = kmeta.siz[2];
                    xab->xab$b_siz3 = kmeta.siz[3];
                    xab->xab$b_siz4 = kmeta.siz[4];
                    xab->xab$b_siz5 = kmeta.siz[5];
                    xab->xab$b_siz6 = kmeta.siz[6];
                    xab->xab$b_siz7 = kmeta.siz[7];
                    /* Compute total key size */
                    uint16_t tks = 0;
                    const uint8_t *szp = &kmeta.siz[0];
                    for (uint8_t s = 0; s < kmeta.nseg && s < 8; s++) {
                        tks += szp[s];
                    }
                    xab->xab$w_tks = tks;
                    xab = (struct XABKEY *)xab->xab$l_nxt;
                }
            }
        }
    }
    fclose(f);
}

/*
 * save_metadata - Save RMS metadata to the sidecar file.
 *
 * Writes the current FAB settings to the companion .rms_meta file.
 * For indexed files, also writes key definitions from the XAB chain.
 */
static void save_metadata(struct FAB *fab)
{
    char sidecar[1088];
    snprintf(sidecar, sizeof(sidecar), "%s%s",
             fab->_resolved_path, RMS_SIDECAR_SUFFIX);

    FILE *f = fopen(sidecar, "wb");
    if (!f) return;

    /* Count keys in XAB chain */
    uint8_t num_keys = 0;
    if (fab->fab$b_org == FAB$C_IDX) {
        struct XABKEY *xab = fab->fab$l_xab;
        while (xab) {
            if (xab->xab$b_cod == XAB$C_KEY) num_keys++;
            xab = (struct XABKEY *)xab->xab$l_nxt;
        }
    }

    struct rms_metadata meta = {
        .magic   = RMS_META_MAGIC,
        .version = RMS_META_VERSION,
        .org     = fab->fab$b_org,
        .rfm     = fab->fab$b_rfm,
        .rat     = fab->fab$b_rat,
        .mrs     = fab->fab$w_mrs,
        .fsz     = fab->fab$b_fsz,
        .mrn     = fab->fab$l_mrn,
        .num_keys = num_keys
    };

    fwrite(&meta, sizeof(meta), 1, f);

    /* Write key definitions for indexed files */
    if (num_keys > 0) {
        struct XABKEY *xab = fab->fab$l_xab;
        while (xab) {
            if (xab->xab$b_cod == XAB$C_KEY) {
                struct rms_key_meta kmeta;
                memset(&kmeta, 0, sizeof(kmeta));
                kmeta.ref = xab->xab$b_ref;
                kmeta.dtp = xab->xab$b_dtp;
                kmeta.flg = xab->xab$w_flg;
                kmeta.nseg = xab->xab$b_nseg;
                kmeta.pos[0] = xab->xab$w_pos0;
                kmeta.pos[1] = xab->xab$w_pos1;
                kmeta.pos[2] = xab->xab$w_pos2;
                kmeta.pos[3] = xab->xab$w_pos3;
                kmeta.pos[4] = xab->xab$w_pos4;
                kmeta.pos[5] = xab->xab$w_pos5;
                kmeta.pos[6] = xab->xab$w_pos6;
                kmeta.pos[7] = xab->xab$w_pos7;
                kmeta.siz[0] = xab->xab$b_siz0;
                kmeta.siz[1] = xab->xab$b_siz1;
                kmeta.siz[2] = xab->xab$b_siz2;
                kmeta.siz[3] = xab->xab$b_siz3;
                kmeta.siz[4] = xab->xab$b_siz4;
                kmeta.siz[5] = xab->xab$b_siz5;
                kmeta.siz[6] = xab->xab$b_siz6;
                kmeta.siz[7] = xab->xab$b_siz7;
                fwrite(&kmeta, sizeof(kmeta), 1, f);
            }
            xab = (struct XABKEY *)xab->xab$l_nxt;
        }
    }

    fclose(f);
}

/*
 * sys$open - Open an existing file.
 *
 * Resolves the filespec from fab$l_fna, opens the underlying
 * Linux file, and loads any stored RMS metadata from the sidecar.
 *
 * On success: fab$l_sts = RMS$_NORMAL, returns RMS$_NORMAL
 * On failure: fab$l_sts set to appropriate error code.
 */
uint32_t sys$open(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    if (resolve_for_open(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        fab->fab$l_stv = 0;
        return RMS$_SYN;
    }

    /* Determine open flags from file access mode */
    int flags = O_RDONLY;
    int need_write = 0;
    if ((fab->fab$b_fac & FAB$M_PUT) || (fab->fab$b_fac & FAB$M_UPD) ||
        (fab->fab$b_fac & FAB$M_DEL) || (fab->fab$b_fac & FAB$M_TRN)) {
        flags = O_RDWR;
        need_write = 1;
    }

    /* VMS protection check before open */
    if (!rms_check_protection(fab->_resolved_path, RMS_PROT_READ)) {
        fab->fab$l_sts = RMS$_PRV;
        fab->fab$l_stv = 0;
        return RMS$_PRV;
    }
    if (need_write && !rms_check_protection(fab->_resolved_path, RMS_PROT_WRITE)) {
        fab->fab$l_sts = RMS$_PRV;
        fab->fab$l_stv = 0;
        return RMS$_PRV;
    }

    int fd = open(fab->_resolved_path, flags);
    if (fd < 0) {
        fab->fab$l_stv = (uint32_t)errno;
        switch (errno) {
            case ENOENT:
                /* If CIF (create-if) option set, try to create */
                if (fab->fab$l_fop & FAB$M_CIF) {
                    return sys$create(fab_ptr);
                }
                fab->fab$l_sts = RMS$_FNF;
                return RMS$_FNF;
            case EACCES:
            case EPERM:
                fab->fab$l_sts = RMS$_PRV;
                return RMS$_PRV;
            case EISDIR:
                fab->fab$l_sts = RMS$_DIR;
                return RMS$_DIR;
            default:
                fab->fab$l_sts = RMS$_ACC;
                return RMS$_ACC;
        }
    }

    fab->_linux_fd = fd;
    load_metadata(fab);

    /* Assign an internal file identifier */
    static uint16_t next_ifi = 1;
    pthread_mutex_lock(&rms_id_lock);
    fab->fab$w_ifi = next_ifi++;
    if (next_ifi == 0) next_ifi = 1;
    pthread_mutex_unlock(&rms_id_lock);

    fab->fab$l_sts = RMS$_NORMAL;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$create - Create a new file.
 *
 * Creates the file with automatic version numbering. Writes
 * the RMS metadata sidecar. Handles FAB$M_SUP (supersede) and
 * FAB$M_MXV (maximize version).
 */
uint32_t sys$create(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    if (resolve_filename(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /* Split path into directory and base filename */
    char dir[1024] = ".";
    char base[256];
    const char *last_slash = strrchr(fab->_resolved_path, '/');
    if (last_slash) {
        size_t dlen = (size_t)(last_slash - fab->_resolved_path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, fab->_resolved_path, dlen);
        dir[dlen] = '\0';
        strncpy(base, last_slash + 1, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    } else {
        strncpy(base, fab->_resolved_path, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    }

    /* Strip any existing version number */
    char base_noversion[256];
    rms_strip_version(base, base_noversion, sizeof(base_noversion));

    /* Determine the version number for the new file */
    int next_ver = rms_next_version(dir, base_noversion);
    if (next_ver < 1) next_ver = 1;

    /* Build the versioned filename */
    char versioned[1024];
    snprintf(versioned, sizeof(versioned), "%s/%s;%d",
             dir, base_noversion, next_ver);
    strncpy(fab->_resolved_path, versioned,
            sizeof(fab->_resolved_path) - 1);
    fab->_resolved_path[sizeof(fab->_resolved_path) - 1] = '\0';

    /* Determine creation flags */
    int flags = O_CREAT | O_RDWR | O_TRUNC;
    if (fab->fab$l_fop & FAB$M_SUP) {
        flags |= O_TRUNC;
    }

    /* Ensure parent directory exists */
    struct stat st;
    if (stat(dir, &st) < 0) {
        fab->fab$l_sts = RMS$_DNF;
        fab->fab$l_stv = (uint32_t)errno;
        return RMS$_DNF;
    }

    /* Use VMS default protection converted to mode_t instead of hardcoded 0644 */
    uint16_t default_prot = rms_get_default_protection();
    mode_t create_mode = vmsfs_protection_to_mode(default_prot);
    int fd = open(fab->_resolved_path, flags, create_mode);
    if (fd < 0) {
        fab->fab$l_sts = RMS$_CRE;
        fab->fab$l_stv = (uint32_t)errno;
        return RMS$_CRE;
    }

    fab->_linux_fd = fd;

    /* Pre-allocate space for relative files */
    if (fab->fab$b_org == FAB$C_REL && fab->fab$l_mrn > 0 &&
        fab->fab$w_mrs > 0) {
        size_t cell_size = (size_t)fab->fab$w_mrs + 1;  /* +1 for status */
        size_t total = cell_size * fab->fab$l_mrn;
        if (ftruncate(fd, (off_t)total) < 0) {
            /* Non-fatal: allocation is best-effort */
        }
    }

    save_metadata(fab);

    /* Assign IFI */
    static uint16_t create_ifi = 1;
    pthread_mutex_lock(&rms_id_lock);
    fab->fab$w_ifi = create_ifi++;
    if (create_ifi == 0) create_ifi = 1;
    pthread_mutex_unlock(&rms_id_lock);

    fab->fab$l_sts = RMS$_CREATED;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$close - Close an open file.
 *
 * Flushes any pending data, closes the file descriptor,
 * and cleans up internal state. Handles TMD (temp delete on close)
 * and DLT (delete on close) options.
 */
uint32_t sys$close(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    uint32_t close_sts = RMS$_NORMAL;
    if (fab->_linux_fd >= 0) {
        /* Flush before closing */
        if (fsync(fab->_linux_fd) < 0) {
            close_sts = RMS$_WER;
        }
        close(fab->_linux_fd);
        fab->_linux_fd = -1;
    }

    /* Handle delete-on-close options */
    if ((fab->fab$l_fop & FAB$M_DLT) || (fab->fab$l_fop & FAB$M_TMD)) {
        if (fab->_resolved_path[0]) {
            unlink(fab->_resolved_path);
            /* Also remove sidecar */
            char sidecar[1088];
            snprintf(sidecar, sizeof(sidecar), "%s%s",
                     fab->_resolved_path, RMS_SIDECAR_SUFFIX);
            unlink(sidecar);
            /* Remove index sidecar if present */
            char idxfile[1088];
            snprintf(idxfile, sizeof(idxfile), "%s%s",
                     fab->_resolved_path, RMS_INDEX_SUFFIX);
            unlink(idxfile);
        }
    }

    /* Clean up internal state */
    if (fab->_rms_state) {
        free(fab->_rms_state);
        fab->_rms_state = NULL;
    }

    fab->fab$w_ifi = 0;
    fab->fab$l_sts = close_sts;
    fab->fab$l_stv = 0;
    return close_sts;
}

/*
 * sys$erase - Delete a file.
 *
 * Resolves the filespec and deletes the file along with
 * its metadata sidecar and index sidecar.
 */
uint32_t sys$erase(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    if (resolve_for_open(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /* VMS protection check for delete access */
    if (!rms_check_protection(fab->_resolved_path, RMS_PROT_DELETE)) {
        fab->fab$l_sts = RMS$_PRV;
        fab->fab$l_stv = 0;
        return RMS$_PRV;
    }

    if (unlink(fab->_resolved_path) < 0) {
        fab->fab$l_stv = (uint32_t)errno;
        switch (errno) {
            case ENOENT:
                fab->fab$l_sts = RMS$_FNF;
                return RMS$_FNF;
            case EACCES:
            case EPERM:
                fab->fab$l_sts = RMS$_PRV;
                return RMS$_PRV;
            default:
                fab->fab$l_sts = RMS$_ACC;
                return RMS$_ACC;
        }
    }

    /* Remove the metadata sidecar (ignore errors) */
    char sidecar[1088];
    snprintf(sidecar, sizeof(sidecar), "%s%s",
             fab->_resolved_path, RMS_SIDECAR_SUFFIX);
    unlink(sidecar);

    /* Remove index sidecar (ignore errors) */
    char idxfile[1088];
    snprintf(idxfile, sizeof(idxfile), "%s%s",
             fab->_resolved_path, RMS_INDEX_SUFFIX);
    unlink(idxfile);

    fab->fab$l_sts = RMS$_NORMAL;
    fab->fab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$connect - Connect a RAB to its FAB, establishing a record stream.
 *
 * Initializes the internal stream state in the RAB. Validates the
 * RAB-FAB linkage. If RAB$M_EOF is set, positions to end of file.
 */
uint32_t sys$connect(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        rab->rab$l_sts = RMS$_FAB;
        return RMS$_FAB;
    }

    if (fab->_linux_fd < 0) {
        rab->rab$l_sts = RMS$_ACC;
        return RMS$_ACC;
    }

    /* Initialize stream state */
    rab->_current_offset = 0;
    rab->_eof = 0;
    rab->_last_rec_offset = 0;
    rab->_last_rec_size = 0;

    /* Assign an internal stream identifier */
    static uint16_t next_isi = 1;
    pthread_mutex_lock(&rms_id_lock);
    rab->rab$w_isi = next_isi++;
    if (next_isi == 0) next_isi = 1;
    pthread_mutex_unlock(&rms_id_lock);

    /* If RAB$M_EOF is set, position to end of file */
    if (rab->rab$l_rop & RAB$M_EOF) {
        rab->_current_offset = lseek(fab->_linux_fd, 0, SEEK_END);
        if (rab->_current_offset < 0) rab->_current_offset = 0;
    }

    rab->rab$l_sts = RMS$_NORMAL;
    rab->rab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$disconnect - Disconnect a RAB from its FAB.
 *
 * Cleans up the stream state. The file remains open in the FAB.
 */
uint32_t sys$disconnect(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    /* Clean up internal stream state */
    if (rab->_rms_stream) {
        free(rab->_rms_stream);
        rab->_rms_stream = NULL;
    }

    rab->_current_offset = 0;
    rab->_eof = 0;
    rab->_last_rec_offset = 0;
    rab->_last_rec_size = 0;
    rab->rab$w_isi = 0;

    rab->rab$l_sts = RMS$_NORMAL;
    rab->rab$l_stv = 0;
    return RMS$_NORMAL;
}

/*
 * sys$display - Display/retrieve file attributes.
 *
 * Loads metadata from the sidecar into the FAB and any XAB chain.
 * Also fills in XAB date/time and protection info from the filesystem.
 */
uint32_t sys$display(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    /* Reload metadata from sidecar */
    load_metadata(fab);

    /* Fill in date/time XAB if present in the XAB chain */
    if (fab->_resolved_path[0]) {
        struct stat st;
        if (stat(fab->_resolved_path, &st) == 0) {
            /* Walk XAB chain looking for date and protection XABs */
            struct XABKEY *xab = fab->fab$l_xab;
            while (xab) {
                if (xab->xab$b_cod == XAB$C_DAT) {
                    struct XABDAT *dat = (struct XABDAT *)xab;
                    /* Store Unix timestamps as VMS-compatible values */
                    dat->xab$q_cdt = (uint64_t)st.st_ctime;
                    dat->xab$q_rdt = (uint64_t)st.st_mtime;
                } else if (xab->xab$b_cod == XAB$C_PRO) {
                    struct XABPRO *pro = (struct XABPRO *)xab;
                    /* Convert Unix permissions to VMS protection */
                    pro->xab$l_uic = (uint32_t)st.st_uid;
                    pro->xab$w_pro = (uint16_t)(st.st_mode & 0xFFFF);
                }
                xab = (struct XABKEY *)xab->xab$l_nxt;
            }
        }
    }

    fab->fab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

/*
 * sys$rewind - Rewind a record stream to the beginning of the file.
 */
uint32_t sys$rewind(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || fab->_linux_fd < 0) {
        rab->rab$l_sts = RMS$_ACC;
        return RMS$_ACC;
    }

    lseek(fab->_linux_fd, 0, SEEK_SET);
    rab->_current_offset = 0;
    rab->_eof = 0;
    rab->_last_rec_offset = 0;
    rab->_last_rec_size = 0;

    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}

/*
 * sys$flush - Flush buffered data to disk.
 */
uint32_t sys$flush(void *rab_ptr)
{
    struct RAB *rab = (struct RAB *)rab_ptr;
    if (!rab || rab->rab$b_bid != RAB$C_BID) {
        return RMS$_RAB;
    }

    struct FAB *fab = rab->rab$l_fab;
    if (!fab || fab->_linux_fd < 0) {
        rab->rab$l_sts = RMS$_ACC;
        return RMS$_ACC;
    }

    if (fsync(fab->_linux_fd) < 0) {
        rab->rab$l_sts = RMS$_WER;
        rab->rab$l_stv = (uint32_t)errno;
        return RMS$_WER;
    }

    rab->rab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}
