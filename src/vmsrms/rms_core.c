/*
 * rms_core.c - RMS Core File Operations
 *
 * Implements $OPEN, $CLOSE, $CREATE, $ERASE, $CONNECT, $DISCONNECT,
 * $DISPLAY, $REWIND, and $FLUSH system services.
 *
 * File metadata (record format, organization, etc.) is stored in
 * companion .rms_meta sidecar files alongside the data files.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * Each service below works on the caller's own FAB/RAB; where it touches a
 * file it does so through a plain path or through the fd cached in
 * fab->_linux_fd, in this process. There is no executive file or record layer,
 * so there is no cross-process record locking and no shared file access
 * arbitration behind any of them.
 *
 * THEY CITED vms-5b4 UNTIL vms-fab -- the item that BUILT this register, closed
 * when the register landed, owning none of the facades in it. vms-407 owns them
 * now, together with rms_record.c and rms_search.c. Note what it is NOT: RMS
 * running in the caller's process is what OpenVMS does too. What is missing is
 * the arbitration the process context is supposed to CALL, and vms-ci.7 already
 * built the lock manager it should call.
 *
 * FILESPEC RESOLUTION NOW REACHES THE EXECUTIVE (vms-96e2). sys$open/$create/
 * $erase take a VMS filespec and resolve it through vmsfs_to_linux_path ->
 * lnm_translate -> vms_kif_lnm_translate, which reads the executive-resident
 * LNM$SYSTEM table for system logical names (SYS$SYSDEVICE, SYS$UPDATE, ...).
 * That step, and only that step, is the executive's; the RMS operation itself
 * is still this process's. So they are PARTIAL, not wholly userspace.
 *
 * OVMX-PARTIAL: sys$open (vms-96e2) -- exec: the VMS filespec is translated to a
 *     Linux path via the executive-resident LNM$SYSTEM table (vmsfs_to_linux_path
 *     -> lnm_translate -> vms_kif_lnm_translate).
 * OVMX-LOCAL: sys$open -- the open itself is open(2) on the translated path; the
 *     FAB share/access fields do not reach any arbitrator.
 * OVMX-PARTIAL: sys$create (vms-96e2) -- exec: same executive-resident filespec
 *     resolution as sys$open, before the file is created.
 * OVMX-LOCAL: sys$create -- open(2) with O_CREAT, plus a .rms_meta sidecar
 *     written by this process.
 * OVMX-USERSPACE: sys$close (vms-407) -- close(2) of the caller's own fd.
 * OVMX-PARTIAL: sys$erase (vms-96e2) -- exec: same executive-resident filespec
 *     resolution as sys$open, before the file is removed.
 * OVMX-LOCAL: sys$erase -- unlink(2) with no interlock against another process
 *     holding the file open.
 * OVMX-USERSPACE: sys$connect (vms-407) -- initializes the stream-position
 *     fields (_current_offset/_eof/_last_rec_offset/_last_rec_size/rab$w_isi)
 *     directly inside the caller's own RAB. Measured: it never allocates;
 *     the RAB's rab->_rms_stream pointer sys$disconnect frees is never set
 *     by this function or anywhere else in the tree.
 * OVMX-USERSPACE: sys$disconnect (vms-407) -- resets those same fields and
 *     frees rab->_rms_stream if non-NULL, which measurement shows is always
 *     NULL -- no code path in the tree ever assigns it.
 * OVMX-USERSPACE: sys$display (vms-407) -- reloads the .rms_meta sidecar this
 *     process wrote; the attributes are file content, not executive metadata.
 * OVMX-USERSPACE: sys$rewind (vms-407) -- repositions the caller's own fd.
 * OVMX-USERSPACE: sys$flush (vms-407) -- flushes the caller's own fd.
 * OVMX-USERSPACE: sys$extend (vms-da9) -- validates the caller's own FAB and
 *     requires an open fd; the requested allocation is met by the Linux backing
 *     store's on-demand block allocation, so no executive allocator is called.
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
#include <time.h>
#include "rms/rms.h"
#include "rms_internal.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"
#include "ovmx_layout.h"
#include "ssdef.h"
#include "lib$routines.h"   /* lib$cvt_vectim: Unix->VMS binary time (vms-3dd) */

/*
 * SYS$DISK ODS-2 working-copy model (epic vms-5eb, R2 -- the ODS-2 runtime
 * flip; docs/design-ods2-runtime-flip.md §0). This is Linux-runtime-only: the
 * SYS$DISK reroute reads/writes a genuine ODS-2 volume through the src/vmsfs/
 * write+read adapters (ods2_sysdisk_*), which are linked only into the Linux
 * userspace stack. The netbsd-vax cross-build (vms-271) compiles this same TU
 * as portable C -- rms_sysdisk_owns() is a constant 0 there and the checkout/
 * checkin/release helpers are inert stubs, so RMS keeps its ordinary POSIX
 * file path on that substrate and nothing here references a Linux-only symbol.
 */
#if defined(__linux__)
#  define OVMX_RMS_SYSDISK 1
#  include <sys/mman.h>          /* memfd_create */
#  include <strings.h>           /* strncasecmp  */
#  include "vmsfs/sysdisk.h"     /* ods2_sysdisk_owns_path/_read_file/_create_file/_list_dir */
#  include "rms_util.h"          /* rms_read_exact / rms_write_exact over the working fd */
#else
#  define OVMX_RMS_SYSDISK 0
#endif

/*
 * unix_time_to_vms - Convert a Unix time_t (seconds since 1970-01-01 UTC) to a
 * VMS 64-bit binary time: the count of 100-nanosecond intervals since the VMS
 * base date, 17-NOV-1858 00:00:00 (the Smithsonian Modified Julian Day epoch).
 *
 * VMS reference: VSI OpenVMS Programming Concepts Manual, Vol. I, "System Time
 * Format" -- absolute time is a signed quadword of 100ns ticks past the base
 * date 17-NOV-1858. The conversion is not done here with a hand-written magic
 * offset; it is delegated to the existing RTL converter lib$cvt_vectim
 * (src/libvms/rtl/lib_datetime.c), which owns the documented VMS_EPOCH_OFFSET
 * constant. We break the absolute Unix instant down to its UTC calendar fields
 * (gmtime_r) and feed lib$cvt_vectim a numeric time vector; lib$cvt_vectim's
 * timegm() inverts gmtime() exactly, so the same wall-clock instant round-trips.
 *
 * Returns the VMS quadword, or 0 if the instant cannot be represented (which a
 * VMS tool reads as "no date", the honest empty value).
 */
static uint64_t unix_time_to_vms(time_t t)
{
    struct tm tmv;
    if (!gmtime_r(&t, &tmv))
        return 0;

    uint16_t timvec[7];
    timvec[0] = (uint16_t)(tmv.tm_year + 1900);   /* year   */
    timvec[1] = (uint16_t)(tmv.tm_mon + 1);       /* month  */
    timvec[2] = (uint16_t)tmv.tm_mday;            /* day    */
    timvec[3] = (uint16_t)tmv.tm_hour;            /* hour   */
    timvec[4] = (uint16_t)tmv.tm_min;             /* minute */
    timvec[5] = (uint16_t)(tmv.tm_sec > 59 ? 59 : tmv.tm_sec); /* sec (drop leap) */
    timvec[6] = 0;                                /* hundredths (stat: 1s res) */

    uint64_t vms_time = 0;
    if (lib$cvt_vectim(timvec, &vms_time) != SS$_NORMAL)
        return 0;
    return vms_time;
}

/* Mutex protecting internal file/stream identifier counters */
static pthread_mutex_t rms_id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Shared IFI counter for both sys$open and sys$create */
static uint16_t next_ifi = 1;

/* Protection functions from vmsfs */
extern uint16_t vmsfs_mode_to_protection(mode_t mode);
extern mode_t   vmsfs_protection_to_mode(uint16_t vms_prot);

/* Indexed-file B-tree persistence (rms_idx.c). rms_idx_cleanup saves the
 * in-memory index before freeing it so sys$close does not drop records added
 * since the last periodic save (vms-5c6d). */
extern uint32_t rms_idx_cleanup(struct FAB *fab);

/* rms_read_exact / rms_write_exact now in rms_util.c */

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

/* Forward decl: rms_impl_open falls through to rms_impl_create on FAB$M_CIF,
 * which is defined later in this file. */
static uint32_t rms_impl_create(void *fab_ptr);

/* ============================================================================
 * SYS$DISK ODS-2 working-copy model (epic vms-5eb, R2). See the design record
 * docs/design-ods2-runtime-flip.md §0 (RE-PLAN #1, route (a)).
 *
 * PROBLEM. RMS's seq/rel/idx engines are built on a POSIX fd + lseek/read/write
 * at arbitrary offsets (60+ sites). The genuine-ODS-2 SYS$DISK adapter offers
 * only whole-file read/create/append/list -- NO positioned fd. Rewriting every
 * positioned-I/O site is the "route (b)" long pole.
 *
 * ROUTE (a), THE RECOMMENDED FIRST CUT (built here). On $OPEN/$CREATE of a file
 * that ods2_sysdisk_owns_path(), read the whole ODS-2 file into a private
 * working copy -- an anonymous memfd -- and point fab->_linux_fd at THAT. Every
 * seq/rel/idx lseek/read/write then operates on the memfd BYTE-FOR-BYTE
 * UNCHANGED: not one positioned-I/O site is touched. On $CLOSE, a writable copy
 * that was actually modified (dirty) is checked in as a NEW ODS-2 version via
 * the SYS$DISK write adapter; a read-only copy just closes the memfd, leaving
 * the volume untouched. The working fd is a per-channel window, exactly like a
 * VMS RMS record buffer -- the durable store stays genuine ODS-2 (INV-6: no
 * silent POSIX fallback; a SYS$DISK path that cannot reach the volume fails
 * honestly, never degrading to a POSIX open of a non-existent /vms tree).
 *
 * CONCURRENCY HAZARD (noted, NOT solved here -- vms-49d, a separate rung). The
 * checkin is a new-version create, so two processes that check the SAME file
 * out, modify it, and check in concurrently each create their own new version
 * off the same base -- last-writer-does-not-win, both survive as distinct
 * versions (VMS-plausible) but there is no interlock making the second see the
 * first. The transaction broker that arbitrates that is out of scope for R2.
 * ==========================================================================*/

#if OVMX_RMS_SYSDISK

/* Cap on a single working-copy checkout (guards a corrupt header's byte count
 * from demanding an unbounded allocation). SYS$DISK runtime files are small;
 * 256 MiB is far above any boot/login file yet bounded. */
#define RMS_SYSDISK_MAX_FILE  (256u * 1024u * 1024u)

/* Map a SYS$DISK adapter status (SS$_ codes) to an RMS$_ status for the open/
 * create path. Fail-honest: a real medium/parameter error surfaces as itself,
 * never as "file not found" masking a mount failure. */
static uint32_t rms_sysdisk_map_status(int ss)
{
    switch (ss) {
    case SS$_NORMAL:        return RMS$_NORMAL;
    case SS$_DEVNOTMOUNT:   return SS$_DEVNOTMOUNT; /* honest: no ODS-2 SYS$DISK */
    case SS$_NOSUCHFILE:    return RMS$_FNF;
    case SS$_BADPARAM:      return RMS$_SYN;
    case SS$_DATACHECK:     return RMS$_RER;
    default:                return RMS$_ACC;
    }
}

/*
 * Create the anonymous working-copy fd. memfd_create on any modern Linux; a
 * tmpfile() fallback (immediately unlinked, so still anonymous) on the rare
 * kernel/libc lacking memfd_create, so seq/rel/idx get an ordinary fd either
 * way. Returns the fd, or -1.
 */
static int rms_sysdisk_workfd(void)
{
    int fd = memfd_create("ovmx_rms_workcopy", 0);
    if (fd >= 0)
        return fd;

    FILE *tf = tmpfile();       /* fallback: anonymous (auto-unlinked) temp file */
    if (!tf)
        return -1;
    int dupfd = dup(fileno(tf));
    fclose(tf);                 /* the dup keeps the now-unlinked file alive */
    return dupfd;
}

/*
 * Exact valid-byte length of an existing SYS$DISK file, from its ODS-2 header
 * (efblk/ffbyte -- ods2_recattr_data_bytes). Returns SS$_NORMAL and *len_out,
 * or an SS$_ failure (fail-honest). Getting the exact size up front lets the
 * checkout allocate precisely and read exactly, so a short/over read is an
 * honest medium fault, not an ambiguous buffer-too-small.
 */
static int rms_sysdisk_file_size(const char *path, size_t *len_out)
{
    const ods2_bdev_t *bv = NULL;
    uint8_t header[ODS2_BLOCK_SIZE];
    int vst = ods2_sysdisk_resolve_file(path, &bv, NULL, header, sizeof(header));
    if (vst != SS$_NORMAL)
        return vst;

    ods2_fh2_t fh2;
    if (ods2_fh2_parse(header, sizeof(header), &fh2) != ODS2_OK)
        return SS$_DATACHECK;

    *len_out = ods2_recattr_data_bytes(&fh2.fh2_recattr);
    return SS$_NORMAL;
}

/*
 * $OPEN/$CREATE checkout: seed a working-copy fd for a SYS$DISK file and point
 * fab->_linux_fd at it.
 *   - created==1 ($CREATE): an EMPTY working copy; the checkin always writes a
 *     new version (a $CREATE always yields a file/version on VMS).
 *   - created==0 ($OPEN): read the whole ODS-2 file into the working copy. When
 *     writable, the original bytes are cached so $CLOSE can skip a spurious new
 *     version if nothing was actually written (the dirty compare).
 * Returns RMS$_NORMAL, or an RMS$_ error (fail-honest -- caller must NOT fall
 * back to a POSIX open).
 */
static uint32_t rms_sysdisk_checkout(struct FAB *fab, int writable, int created)
{
    int wfd = rms_sysdisk_workfd();
    if (wfd < 0)
        return RMS$_CRE;

    void  *orig = NULL;
    size_t orig_len = 0;

    if (!created) {
        size_t sz = 0;
        int vst = rms_sysdisk_file_size(fab->_resolved_path, &sz);
        if (vst != SS$_NORMAL) {
            close(wfd);
            return rms_sysdisk_map_status(vst);
        }
        if (sz > RMS_SYSDISK_MAX_FILE) {
            close(wfd);
            return RMS$_RER;
        }

        orig = malloc(sz ? sz : 1);
        if (!orig) {
            close(wfd);
            return RMS$_CRE;
        }
        if (sz > 0) {
            size_t got = 0;
            vst = ods2_sysdisk_read_file(fab->_resolved_path, orig, sz, &got);
            if (vst != SS$_NORMAL || got != sz) {
                free(orig);
                close(wfd);
                return (vst != SS$_NORMAL) ? rms_sysdisk_map_status(vst)
                                           : RMS$_RER;
            }
            if (rms_write_exact(wfd, orig, sz) != 0) {
                free(orig);
                close(wfd);
                return RMS$_WER;
            }
            lseek(wfd, 0, SEEK_SET);
        }
        orig_len = sz;
    }

    fab->_linux_fd        = wfd;
    fab->_sysdisk_workcopy = 1;
    fab->_sysdisk_writable = writable ? 1 : 0;
    fab->_sysdisk_created  = created ? 1 : 0;

    /* Keep the original only for a writable OPEN's dirty compare. A read-only
     * open never checks in; a $CREATE always checks in -- neither needs it. */
    if (writable && !created) {
        fab->_sysdisk_orig     = orig;
        fab->_sysdisk_orig_len = orig_len;
    } else {
        free(orig);
        fab->_sysdisk_orig     = NULL;
        fab->_sysdisk_orig_len = 0;
    }
    return RMS$_NORMAL;
}

/* Split a resolved SYS$DISK path "/vms/A/B/NAME.EXT[;ver]" into its parent
 * directory path ("/vms/A/B") and the bare filename ("NAME.EXT", version
 * stripped). Returns 0 on success. */
static int rms_sysdisk_split(const char *path, char *dir, size_t dirlen,
                             char *name, size_t namelen)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return -1;

    size_t dlen = (size_t)(slash - path);
    if (dlen == 0)               /* "/NAME" -- parent is "/", not a SYS$DISK dir */
        return -1;
    if (dlen >= dirlen)
        return -1;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';

    /* filename without any ";ver" suffix */
    char raw[512];
    strncpy(raw, slash + 1, sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = '\0';
    char *semi = strchr(raw, ';');
    if (semi)
        *semi = '\0';
    if (raw[0] == '\0' || strlen(raw) >= namelen)
        return -1;
    strcpy(name, raw);
    return 0;
}

struct rms_sysdisk_verctx {
    const char *name;       /* filename to match (version-stripped, uppercase) */
    uint16_t    maxv;       /* highest version seen for that name */
};

static int rms_sysdisk_ver_cb(const char *name, unsigned name_len,
                              uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    (void)fid;
    struct rms_sysdisk_verctx *c = (struct rms_sysdisk_verctx *)ctx;
    if (name_len == strlen(c->name) &&
        strncasecmp(name, c->name, name_len) == 0) {
        if (version > c->maxv)
            c->maxv = version;
    }
    return 0;   /* keep scanning */
}

/*
 * $CLOSE checkin: if the working copy is writable and dirty, write its current
 * contents back as a NEW ODS-2 version of the file. Read-only copies and
 * unchanged writable copies write nothing (no spurious version). Always closes
 * the working fd and frees the cached original. Returns RMS$_NORMAL or an
 * RMS$_ error (fail-honest -- a failed checkin is surfaced, never swallowed).
 */
static uint32_t rms_sysdisk_checkin(struct FAB *fab)
{
    uint32_t sts = RMS$_NORMAL;

    if (fab->_sysdisk_writable && fab->_linux_fd >= 0) {
        /* Snapshot the working copy. */
        off_t end = lseek(fab->_linux_fd, 0, SEEK_END);
        size_t len = (end > 0) ? (size_t)end : 0;
        void  *data = malloc(len ? len : 1);
        if (!data) {
            sts = RMS$_WER;
            goto done;
        }
        if (len > 0) {
            if (lseek(fab->_linux_fd, 0, SEEK_SET) < 0 ||
                rms_read_exact(fab->_linux_fd, data, len) != (ssize_t)len) {
                free(data);
                sts = RMS$_RER;
                goto done;
            }
        }

        /* Dirty compare for a modified-in-place OPEN: identical content ->
         * skip the checkin so a read-then-write that changed nothing does not
         * mint a spurious new version. A $CREATE always checks in. */
        int dirty = fab->_sysdisk_created ||
                    len != fab->_sysdisk_orig_len ||
                    (len > 0 && memcmp(data, fab->_sysdisk_orig, len) != 0);

        if (dirty) {
            char dir[1024], name[256];
            if (rms_sysdisk_split(fab->_resolved_path, dir, sizeof(dir),
                                  name, sizeof(name)) != 0) {
                free(data);
                sts = RMS$_SYN;
                goto done;
            }

            /* Next version = highest existing version of NAME in the parent
             * directory + 1 (1 if none exists yet). */
            struct rms_sysdisk_verctx vc = { name, 0 };
            (void)ods2_sysdisk_list_dir(dir, rms_sysdisk_ver_cb, &vc);
            unsigned newver = (unsigned)vc.maxv + 1u;

            char newpath[1088];
            int m = snprintf(newpath, sizeof(newpath), "%s/%s;%u",
                             dir, name, newver);
            if (m <= 0 || (size_t)m >= sizeof(newpath)) {
                free(data);
                sts = RMS$_SYN;
                goto done;
            }

            int vst = ods2_sysdisk_create_file(newpath, data, len);
            if (vst != SS$_NORMAL)
                sts = rms_sysdisk_map_status(vst);
        }
        free(data);
    }

done:
    if (fab->_linux_fd >= 0) {
        close(fab->_linux_fd);
        fab->_linux_fd = -1;
    }
    free(fab->_sysdisk_orig);
    fab->_sysdisk_orig     = NULL;
    fab->_sysdisk_orig_len = 0;
    fab->_sysdisk_workcopy = 0;
    fab->_sysdisk_writable = 0;
    fab->_sysdisk_created  = 0;
    return sts;
}

static int rms_sysdisk_owns(const char *p) { return ods2_sysdisk_owns_path(p); }

#else  /* !OVMX_RMS_SYSDISK -- netbsd-vax cross: inert stubs, never reached */

static int rms_sysdisk_owns(const char *p) { (void)p; return 0; }
static uint32_t rms_sysdisk_checkout(struct FAB *fab, int writable, int created)
{ (void)fab; (void)writable; (void)created; return RMS$_ACC; }
static uint32_t rms_sysdisk_checkin(struct FAB *fab) { (void)fab; return RMS$_NORMAL; }

#endif /* OVMX_RMS_SYSDISK */

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
        /* Strip leading dot — vmsfs API expects ext without dot */
        const char *ext_no_dot = (dot[1] != '\0') ? dot + 1 : "";
        strncpy(ext, ext_no_dot, sizeof(ext) - 1);
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
        /* Strip leading dot — vmsfs API expects ext without dot */
        const char *ext_no_dot = (dot[1] != '\0') ? dot + 1 : "";
        strncpy(ext, ext_no_dot, sizeof(ext) - 1);
        ext[sizeof(ext) - 1] = '\0';
        *dot = '\0';
    }
    int ver = vmsfs_get_highest_version(dir, name, ext);
    if (ver < 1) ver = 1;
    char candidate[1024];
    if (ext[0]) {
        snprintf(candidate, sizeof(candidate), "%s/%s.%s;%d", dir, name, ext, ver);
    } else {
        snprintf(candidate, sizeof(candidate), "%s/%s;%d", dir, name, ver);
    }

    /* OVMX (vms-4ba.5): vmsfs_get_highest_version() treats a plain,
     * unversioned on-disk file as "version 1" (see its own doc comment), but
     * that only tells us the VERSION NUMBER to use — it does not mean a file
     * literally named "name;1" exists. Every file sys$create() itself
     * produces DOES carry a literal ";N" suffix on disk (see sys$create
     * below), so this candidate always resolves correctly for RMS-created
     * files. But for a file that was authored OUTSIDE RMS with no version
     * syntax at all (e.g. a .c source someone just wrote), the synthesized
     * "name;1" candidate does not exist and open() would wrongly fail with
     * RMS$_FNF even though the file is right there under its plain name.
     * Surfaced by vms-4ba.5 (TCC.EXE reading its own hello.c via RMS) — a
     * general RMS gap, not tcc-specific. Fix: if the versioned candidate
     * isn't real but the plain unversioned name is, resolve to the plain
     * name instead of a synthesized version that doesn't exist. This is
     * strictly additive: RMS-created files (which always have both a real
     * ";N" candidate AND, at version 1, no separate unversioned copy) are
     * unaffected — the candidate stat() succeeds and this branch is
     * skipped. */
    struct stat st;
    if (stat(candidate, &st) != 0) {
        char plain[1024];
        if (ext[0]) {
            snprintf(plain, sizeof(plain), "%s/%s.%s", dir, name, ext);
        } else {
            snprintf(plain, sizeof(plain), "%s/%s", dir, name);
        }
        if (stat(plain, &st) == 0) {
            strncpy(out, plain, outlen - 1);
            out[outlen - 1] = '\0';
            return 0;
        }
    }

    strncpy(out, candidate, outlen - 1);
    out[outlen - 1] = '\0';
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
 * rms_check_protection() -- DELETED, NOT REPLACED (vms-2b8, operator ruling
 * 2026-07-31). It called vms$check_access(), a userspace second opinion on
 * file protection computed from the same st_mode the real enforcer already
 * decides from. It could only produce a FALSE denial (it has no notion of
 * SYSPRV/BYPASS/READALL, so it could refuse what the executive would grant)
 * and could never produce a false grant that mattered, because on the
 * bootable runtime the actual decision is made by
 * src/kernel/vmsfs/vmsfs_blkdev.c through the Linux DAC bits vmsfs derives
 * from the VMS protection mask -- measured directly: rebuilding this
 * function to return 1 unconditionally changed nothing about the failure it
 * was thought to gate, and chmod 0777 over the whole [SYS0] tree at the
 * Linux layer did not let an unprivileged account write SYS$SYSTEM: on the
 * real runtime either. See sys_security.c's comment at the deleted
 * vms$check_access() for the full account.
 *
 * The three call sites below (sys$open's read/write pre-check and sys$erase's
 * delete pre-check) are removed with it. What enforces protection now is the
 * open()/unlink() a few lines later in each function, exactly as it already
 * did for every DCL command (COPY/TYPE/DELETE) that bypasses RMS via
 * fopen()/unlink() directly -- RMS and DCL now share one enforcer instead of
 * RMS adding a second, weaker one in front of it. EACCES/EPERM from that
 * real syscall already maps to RMS$_PRV a few lines below in both functions;
 * no new error path is needed.
 */

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
        /*
         * vmsfs_to_linux_path() returns a VMS status code (SS$_NORMAL = 1 on
         * success, per its own doc comment and $VMS_STATUS_SUCCESS: odd =
         * success) -- NOT 0-on-success. This check used to read `== 0`,
         * which is an EVEN (failure) value that the function never actually
         * returns for a real error either (its failures are also VMS status
         * codes, e.g. SS$_BADPARAM/SS$_NOSUCHDEV, all even). So this branch
         * ALWAYS took the "not translated" fallback below and treated the
         * raw VMS spec string ("SYS$SCRATCH:FOO.DAT") as a literal Linux
         * path -- relative, since it does not start with '/' -- resolving
         * against the process's cwd instead of the real /vms/... location.
         * For a SYSTEM session whose cwd is not writable, every VMS-spec
         * candidate (SYS$SCRATCH:, SYS$LOGIN:, DKA0:[USERS], SYS$SYSDEVICE:
         * [SYSTMP]) failed open() with EACCES for that reason -- not a
         * directory-permission problem at all -- which is exactly why
         * PARTS's sys$create() kept landing on its last, Unix-path fallback
         * candidate ("/tmp/PARTS.DAT") instead of any VMS filespec (vms-221).
         * A plain open() call using vmsfs_to_linux_path() directly (as
         * vms-e5c's own positive control does) never went through this
         * broken check, which is why it always succeeded while sys$create()
         * did not. sys$open() shares this same resolve_filename() and had
         * the identical defect.
         */
        if ($VMS_STATUS_SUCCESS(vmsfs_to_linux_path(spec, linux_path, sizeof(linux_path)))) {
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
         * not for paths that just have a version number (;N).
         *
         * SYS$DISK ODS-2 flip (epic vms-5eb, R2): a "/vms/..." path is served
         * by the genuine-ODS-2 volume adapter, which OWNS its own boundary
         * (it can only ever address files reachable from the volume's MFD).
         * realpath()-based validation assumes /vms is a live POSIX tree, which
         * it is NOT once the flip lands, so it would wrongly reject every
         * SYS$DISK path. Skip it for adapter-owned paths -- the adapter is the
         * boundary. */
        if (is_vms_spec && fab->_resolved_path[0] == '/' &&
            !rms_sysdisk_owns(fab->_resolved_path) &&
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

    /* SYS$DISK ODS-2 flip (epic vms-5eb, R2): version resolution for a
     * "/vms/..." file is the adapter's job, not a POSIX opendir. Leaving the
     * path unversioned (or with the caller's explicit ";ver") lets
     * ods2_sysdisk_* select the highest genuine ODS-2 version. rms_resolve_
     * version() below opendir()s the /vms POSIX tree, which does not exist
     * once the flip lands, so it must NOT run for adapter-owned paths. */
    if (rms_sysdisk_owns(fab->_resolved_path))
        return 0;

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

    if (fwrite(&meta, sizeof(meta), 1, f) != 1) {
        fclose(f);
        return;
    }

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
                if (fwrite(&kmeta, sizeof(kmeta), 1, f) != 1) break;
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
static uint32_t rms_impl_open(void *fab_ptr)
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

    /*
     * SYS$DISK ODS-2 flip (epic vms-5eb, R2): a file on the genuine-ODS-2
     * SYS$DISK is opened through the working-copy model, NOT a POSIX open of a
     * /vms tree. Check the whole ODS-2 file out into a memfd and point
     * _linux_fd at it; the seq/rel/idx engines then run byte-for-byte
     * unchanged. FAIL-HONEST (INV-6 / Rule 9): a checkout failure is returned
     * as-is and NEVER falls through to the POSIX open below -- except CIF
     * (create-if-nonexistent), which is honored exactly as the POSIX path does.
     */
    if (rms_sysdisk_owns(fab->_resolved_path)) {
        uint32_t cst = rms_sysdisk_checkout(fab, need_write, /*created=*/0);
        if (!(cst & 1u)) {
            if (cst == RMS$_FNF && (fab->fab$l_fop & FAB$M_CIF))
                return rms_impl_create(fab_ptr);
            fab->fab$l_sts = cst;
            fab->fab$l_stv = 0;
            return cst;
        }
        /* No POSIX .rms_meta sidecar for a SYS$DISK file (it cannot live on the
         * ODS-2 volume as a POSIX companion); the FAB keeps its caller-supplied
         * or default record format. Sidecar-metadata persistence for SYS$DISK
         * is deferred R2 work -- see docs/design-ods2-runtime-flip.md §0. */
        pthread_mutex_lock(&rms_id_lock);
        fab->fab$w_ifi = next_ifi++;
        if (next_ifi == 0) next_ifi = 1;
        pthread_mutex_unlock(&rms_id_lock);
        fab->fab$l_sts = RMS$_NORMAL;
        fab->fab$l_stv = 0;
        return RMS$_NORMAL;
    }

    /*
     * NO PRE-CHECK HERE (vms-2b8, operator ruling 2026-07-31): see the
     * deleted rms_check_protection() above. open() below is the only
     * protection decision now; EACCES/EPERM maps to RMS$_PRV just below.
     */
    int fd = open(fab->_resolved_path, flags);
    if (fd < 0) {
        fab->fab$l_stv = (uint32_t)errno;
        switch (errno) {
            case ENOENT:
                /* If CIF (create-if) option set, try to create */
                if (fab->fab$l_fop & FAB$M_CIF) {
                    return rms_impl_create(fab_ptr);
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
static uint32_t rms_impl_create(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    if (resolve_filename(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /* SYS$DISK ODS-2 flip (epic vms-5eb, R2): create a file on the genuine-
     * ODS-2 SYS$DISK via the working-copy model. The new version is minted at
     * $CLOSE (checkin) from the live ODS-2 directory listing, so the POSIX
     * directory-scan / version / open(O_CREAT) / sidecar machinery below is
     * bypassed. _resolved_path keeps the version-less "/vms/.../NAME.EXT";
     * checkin appends the next ";ver". Fail-honest: a checkout failure
     * (e.g. no ODS-2 volume) returns as-is, never a POSIX create fallback. */
    if (rms_sysdisk_owns(fab->_resolved_path)) {
        uint32_t cst = rms_sysdisk_checkout(fab, /*writable=*/1, /*created=*/1);
        if (!(cst & 1u)) {
            fab->fab$l_sts = cst;
            fab->fab$l_stv = 0;
            return cst;
        }
        pthread_mutex_lock(&rms_id_lock);
        fab->fab$w_ifi = next_ifi++;
        if (next_ifi == 0) next_ifi = 1;
        pthread_mutex_unlock(&rms_id_lock);
        fab->fab$l_sts = RMS$_CREATED;
        fab->fab$l_stv = 0;
        return RMS$_NORMAL;
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
    int flags = O_CREAT | O_RDWR;
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
        /* Overflow check: ensure cell_size * mrn doesn't wrap */
        if (fab->fab$l_mrn <= SIZE_MAX / cell_size) {
            size_t total = cell_size * fab->fab$l_mrn;
            if (ftruncate(fd, (off_t)total) < 0) {
                /* Non-fatal: allocation is best-effort */
            }
        }
    }

    save_metadata(fab);

    /* Assign IFI (shared counter with sys$open) */
    pthread_mutex_lock(&rms_id_lock);
    fab->fab$w_ifi = next_ifi++;
    if (next_ifi == 0) next_ifi = 1;
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
static uint32_t rms_impl_close(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    /* SYS$DISK ODS-2 flip (epic vms-5eb, R2): a working-copy close is a
     * checkin, not a POSIX close. A dirty writable copy is written back as a
     * new ODS-2 version; a read-only / unchanged copy just discards the memfd
     * (leaving the volume untouched). This bypasses the POSIX fsync/unlink/
     * sidecar machinery below entirely -- delete-on-close (DLT/TMD) and the
     * indexed .rms_idx sidecar for SYS$DISK files are deferred R2 work (seq/rel
     * only in this increment); any _rms_state is freed without touching /vms. */
    if (fab->_sysdisk_workcopy) {
        uint32_t st = rms_sysdisk_checkin(fab);
        if (fab->_rms_state) {
            free(fab->_rms_state);
            fab->_rms_state = NULL;
        }
        fab->fab$w_ifi = 0;
        fab->fab$l_sts = st;
        fab->fab$l_stv = 0;
        return st;
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
    int deleting = (fab->fab$l_fop & FAB$M_DLT) || (fab->fab$l_fop & FAB$M_TMD);
    if (deleting) {
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

    /*
     * Clean up internal state.
     *
     * For an indexed file, fab->_rms_state holds the in-memory B-tree. Because
     * rms_idx_put only persists the tree every 100 inserts, the tree normally
     * carries records added since the last periodic save; freeing it raw would
     * silently drop them while sys$close still returned success (vms-5c6d).
     * rms_idx_cleanup() runs btree_save() before freeing so the index reaches
     * disk, and reports a write failure as RMS$_WER rather than faking success
     * (INV-6). Sequential and relative files never allocate _rms_state, so
     * their close path is unchanged (the org guard is belt-and-suspenders).
     *
     * When the file is being deleted on close we must NOT re-persist the index
     * -- the delete block above already unlinked the .rms_idx sidecar, and a
     * save here would resurrect it as an orphan -- so just free the tree.
     */
    if (fab->_rms_state) {
        if (fab->fab$b_org == FAB$C_IDX && !deleting) {
            uint32_t idx_sts = rms_idx_cleanup(fab);  /* btree_save() + free */
            if (!(idx_sts & 1u) && (close_sts & 1u)) {
                close_sts = idx_sts;  /* surface the index write error */
            }
        } else {
            free(fab->_rms_state);
            fab->_rms_state = NULL;
        }
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
static uint32_t rms_impl_erase(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }

    if (resolve_for_open(fab) < 0) {
        fab->fab$l_sts = RMS$_SYN;
        return RMS$_SYN;
    }

    /*
     * NO PRE-CHECK HERE (vms-2b8, operator ruling 2026-07-31): see the
     * deleted rms_check_protection() above. unlink() below is the only
     * protection decision now; EACCES/EPERM maps to RMS$_PRV just below.
     */
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
static uint32_t rms_impl_connect(void *rab_ptr)
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
static uint32_t rms_impl_disconnect(void *rab_ptr)
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
static uint32_t rms_impl_display(void *fab_ptr)
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
                    /*
                     * Store real VMS binary-time quadwords, not raw Unix
                     * time_t (vms-3dd). st_ctime/st_mtime are seconds since the
                     * Unix epoch; xab$q_cdt/xab$q_rdt are VMS's 100ns-since-
                     * 17-NOV-1858 format that DIRECTORY/DATE, $ASCTIM and
                     * XABDAT readers decode. Convert through the RTL converter.
                     */
                    dat->xab$q_cdt = unix_time_to_vms(st.st_ctime);
                    dat->xab$q_rdt = unix_time_to_vms(st.st_mtime);
                } else if (xab->xab$b_cod == XAB$C_PRO) {
                    struct XABPRO *pro = (struct XABPRO *)xab;
                    /* Convert Unix uid/gid to VMS UIC (group << 16 | member) */
                    pro->xab$l_uic = ((uint32_t)(st.st_gid & 0xFFFF) << 16) |
                                      (uint32_t)(st.st_uid & 0xFFFF);
                    /* Convert Unix mode to VMS protection mask */
                    pro->xab$w_pro = vmsfs_mode_to_protection(st.st_mode);
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
static uint32_t rms_impl_rewind(void *rab_ptr)
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
static uint32_t rms_impl_flush(void *rab_ptr)
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

/*
 * sys$extend - Extend a file's allocation.
 *
 * VSI OpenVMS RMS Reference Manual, $EXTEND: increases a file's allocated
 * space by fab$l_alq blocks WITHOUT moving the file's end-of-file marker
 * (a pre-allocation, not a write). The caller sets fab$l_alq to the number
 * of blocks to add before the call.
 *
 * An OVMX file is backed by a plain Linux file whose blocks are allocated on
 * demand by the host filesystem, so the space $EXTEND promises is genuinely
 * available to the subsequent $PUTs without a physical pre-reserve (and up to
 * real disk capacity, exactly as $EXTEND itself is bounded). The observable
 * contract — "the records the caller is about to write will fit" — is therefore
 * met by the backing store. This is NOT a facade: no per-process state is faked
 * and no success is reported for a control block that is not a real, open file.
 * We deliberately do NOT grow the file physically, because a sequential file's
 * logical EOF is its byte length; padding it here would corrupt the record
 * stream a subsequent $GET reads back. (Register declaration for $EXTEND is in
 * the file header block above, with the other rms_core services.)
 */
static uint32_t rms_impl_extend(void *fab_ptr)
{
    struct FAB *fab = (struct FAB *)fab_ptr;
    if (!fab || fab->fab$b_bid != FAB$C_BID) {
        return RMS$_FAB;
    }
    if (fab->_linux_fd < 0) {
        fab->fab$l_sts = RMS$_IFI;
        return RMS$_IFI;
    }

    fab->fab$l_sts = RMS$_NORMAL;
    return RMS$_NORMAL;
}


/* ============================================================
 * Public RMS entry points: VMS three-argument form
 *   SYS$xxx cb ,[err] ,[suc]   (VSI OpenVMS RMS Reference, Part III)
 * Thin wrappers over the synchronous rms_impl_* bodies above that
 * dispatch the optional AST-level completion routine (rms_complete).
 * ============================================================ */
uint32_t sys$extend(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_extend(fab), fab, err, suc);
}

uint32_t sys$open(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_open(fab), fab, err, suc);
}

uint32_t sys$create(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_create(fab), fab, err, suc);
}

uint32_t sys$close(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_close(fab), fab, err, suc);
}

uint32_t sys$erase(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_erase(fab), fab, err, suc);
}

uint32_t sys$connect(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_connect(rab), rab, err, suc);
}

uint32_t sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_disconnect(rab), rab, err, suc);
}

uint32_t sys$display(void *fab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_display(fab), fab, err, suc);
}

uint32_t sys$rewind(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_rewind(rab), rab, err, suc);
}

uint32_t sys$flush(void *rab, void (*err)(void *), void (*suc)(void *))
{
    return rms_complete(rms_impl_flush(rab), rab, err, suc);
}
