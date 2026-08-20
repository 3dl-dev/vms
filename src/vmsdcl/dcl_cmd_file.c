/*
 * dcl_cmd_file.c - DCL file operation command implementations
 *
 * DIRECTORY, TYPE, COPY, DELETE, RENAME, CREATE, SEARCH, PURGE, APPEND
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>

#include "dcl/context.h"
#include "dcl/parser.h"
#include "dcl/symbol.h"
#include "dcl/dcl_cmd.h"
#include "dcl/dcl_rms.h"          /* vms-481: file commands reach files via RMS/ACP */
#include "dcl/vms_messages.h"
#include "ssdef.h"
#include "stsdef.h"
#include "rmsdef.h"
#include "vmsfs/filespec.h"
#include "vmsfs/version.h"
#include "vmsfs/device.h"        /* vms-b3e: vmsfs_device_spec_kernel_mounted */
#include "vmsqueue.h"

/* Directory entry for sorting in DIRECTORY command */
struct dir_entry {
    char vms_name[256];  /* Formatted VMS name (UPPERCASE, with version) */
    char raw_name[256];  /* Original d_name for name part comparison */
    int  version;        /* Numeric version for sort (descending) */
    long blocks;
    struct stat st;
    struct timespec btime;  /* real file-creation time (statx STATX_BTIME) */
    int  has_btime;         /* 1 = btime is a genuine creation time; 0 = the
                             * backing volume records none, so DIRECTORY/FULL
                             * must NOT invent one (INV-6). */
    /* vms-481: genuine ODS-2 header data when this entry came from the ACP
     * (from_acp=1). The real File ID DIRECTORY /FULL emits, the on-disk
     * protection/allocation, and the VMS 64-bit dates -- NOT stat()-derived. */
    int      from_acp;      /* 1 = fields below are genuine ACP/ODS-2 data */
    uint16_t fid_num, fid_seq;
    uint8_t  fid_rvn;
    uint16_t vms_prot;      /* ODS-2 file protection (4 nibbles S/O/G/W) */
    long     alloc_blocks;  /* highest allocated VBN (allocation quantity) */
    uint8_t  credate[8];    /* VMS 64-bit creation time (0 => not recorded) */
    uint8_t  revdate[8];    /* VMS 64-bit revision time */
    int      has_cre, has_rev;
};

/* /SIZE[=option] mode. VSI OpenVMS DCL Dictionary, DIRECTORY /SIZE: bare /SIZE
 * (and /SIZE=USED) shows blocks USED; /SIZE=ALLOCATION shows blocks ALLOCATED;
 * /SIZE=ALL shows "used/allocated". */
enum dir_size_mode { DIR_SIZE_USED = 0, DIR_SIZE_ALLOC, DIR_SIZE_ALL };

/*
 * ovmx_file_btime - Fetch the REAL file-creation ("birth") time for a path.
 *
 * DIRECTORY/FULL's "Created:" line must show a genuine creation timestamp, not
 * st_mtime relabelled (INV-6: never present a fabricated value as authentic).
 * The only genuine source on Linux is statx(STATX_BTIME); when the backing
 * filesystem does not record a birth time (the mask bit stays clear) we return
 * 0 and the caller prints an honest "<not recorded>" rather than a made-up date.
 *
 * Non-Linux builds (the netbsd-vax DCL cross, build-vmsdcl-vax.sh) have no
 * statx; this compiles to "no genuine source", which is correct for them.
 */
#if defined(__linux__)
#include <stdint.h>
#include <sys/syscall.h>
#endif
#if defined(__linux__) && defined(SYS_statx)
/* Minimal statx ABI (Linux UAPI, stable since 4.11), declared LOCALLY so this
 * depends on neither the libc statx() wrapper (absent on the musl the static /
 * bootable-runtime DCL links against) nor <linux/stat.h> (whose struct statx
 * redefinition clashes with newer musl's <sys/stat.h>). Names are prefixed to
 * avoid ever colliding with a libc-provided struct statx. */
struct ovmx_statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t __r; };
struct ovmx_statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode, __spare0[1];
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct ovmx_statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint64_t __pad[24];   /* pad past the kernel's 256-byte statx buffer */
};
_Static_assert(sizeof(struct ovmx_statx) >= 256, "statx buffer too small");
#define OVMX_STATX_BTIME 0x00000800U    /* STATX_BTIME */
#define OVMX_AT_FDCWD    (-100)         /* AT_FDCWD */
static int ovmx_file_btime(const char *path, struct timespec *out)
{
    struct ovmx_statx stx;
    /* Raw syscall (AT_STATX_SYNC_AS_STAT == 0). Portable across every DCL build
     * variant; the kernel (>= 4.11) always provides the call. */
    if (syscall(SYS_statx, OVMX_AT_FDCWD, path, 0, OVMX_STATX_BTIME, &stx) != 0)
        return 0;
    if (!(stx.stx_mask & OVMX_STATX_BTIME)) return 0;
    out->tv_sec  = (time_t)stx.stx_btime.tv_sec;
    out->tv_nsec = (long)stx.stx_btime.tv_nsec;
    return 1;
}
#else
static int ovmx_file_btime(const char *path, struct timespec *out)
{
    (void)path; (void)out;
    return 0;
}
#endif

/*
 * dir_format_vmsdate - Format a real timespec as VMS "dd-MMM-yyyy hh:mm:ss.cc".
 *
 * The centiseconds (.cc) are the GENUINE sub-second fraction of the timestamp
 * (tv_nsec / 10^7), not a hardcoded ".00" — this is the DIRECTORY/FULL date
 * fidelity gap vms-5e2 closes. Mirrors lib$format_date_time's cc derivation,
 * formatted inline (as every other DCL date is) to avoid adding a cross-image
 * RTL symbol to the DCL native-link graph.
 */
static void dir_format_vmsdate(struct timespec ts, char *buf, size_t bufsize)
{
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    long cc = ts.tv_nsec / 10000000L;   /* hundredths of a second */
    if (cc < 0) cc = 0;
    if (cc > 99) cc = 99;
    snprintf(buf, bufsize, "%2d-%s-%04d %02d:%02d:%02d.%02ld",
             tm.tm_mday, vms_months[tm.tm_mon], 1900 + tm.tm_year,
             tm.tm_hour, tm.tm_min, tm.tm_sec, cc);
}

/*
 * dir_format_vmsbintime - vms-481: format a VMS 64-bit absolute time (the ODS-2
 * ATR$C_CREDATE / _REVDATE the ACP returns) as "dd-MMM-yyyy hh:mm:ss.cc".
 *
 * A VMS binary time is 100-ns ticks since the Smithsonian base 17-NOV-1858
 * (public, documented -- clean-room, Rule 8); Unix time subtracts the
 * 3506716800-second offset to 01-JAN-1970. Converted inline to a timespec and
 * handed to dir_format_vmsdate, so no new cross-image RTL symbol (sys$numtim)
 * is added to the DCL native-link graph (same reason dir_format_vmsdate formats
 * inline). Zero (unset) => 0 and the caller prints an honest "not recorded".
 */
#define DIR_VMS_UNIX_OFFSET_SEC 3506716800LL
static int dir_format_vmsbintime(const uint8_t vt[8], char *buf, size_t bufsize)
{
    uint64_t ticks;
    memcpy(&ticks, vt, 8);
    if (ticks == 0) { snprintf(buf, bufsize, "<not recorded>"); return 0; }
    long long secs = (long long)(ticks / 10000000ULL) - DIR_VMS_UNIX_OFFSET_SEC;
    long nsec = (long)((ticks % 10000000ULL) * 100ULL);
    if (secs < 0) { snprintf(buf, bufsize, "<not recorded>"); return 0; }
    struct timespec ts;
    ts.tv_sec = (time_t)secs;
    ts.tv_nsec = nsec;
    dir_format_vmsdate(ts, buf, bufsize);
    return 1;
}

/* Per-category access bits within a VMS protection nibble (set = DENIED) and
 * the nibble shifts. Kept as literals (matching src/libvms/include/
 * ovmx_fileprot.h) so no libvms header is pulled onto the DCL native-link
 * include path. */
#define DIR_PROT_R 0x01
#define DIR_PROT_W 0x02
#define DIR_PROT_E 0x04
#define DIR_PROT_D 0x08

/*
 * dir_format_prot_full - Render a VMS protection word in the LONG form real
 * DIRECTORY/FULL prints: "System:RWED, Owner:RWED, Group:RE, World:".
 *
 * Same protection word (from st_mode via vmsfs_mode_to_protection) the
 * columnar /PROTECTION display decodes — only the presentation differs. A
 * clear bit means the access is ALLOWED (VMS convention).
 */
static void dir_format_prot_full(uint16_t prot, char *buf, size_t bufsize)
{
    static const char *cats[]  = { "System", "Owner", "Group", "World" };
    static const int   shift[] = { 0, 4, 8, 12 };
    size_t off = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t bits = (uint8_t)((prot >> shift[i]) & 0x0F);
        char acc[5]; int a = 0;
        if (!(bits & DIR_PROT_R)) acc[a++] = 'R';
        if (!(bits & DIR_PROT_W)) acc[a++] = 'W';
        if (!(bits & DIR_PROT_E)) acc[a++] = 'E';
        if (!(bits & DIR_PROT_D)) acc[a++] = 'D';
        acc[a] = '\0';
        int n = snprintf(buf + off, bufsize - off, "%s%s:%s",
                         (i > 0) ? ", " : "", cats[i], acc);
        if (n < 0 || (size_t)n >= bufsize - off) break;
        off += (size_t)n;
    }
}

/*
 * dcl_filename_component - Extract the raw NAME.TYPE;VERSION text a user
 * typed in a VMS filespec (device:[dir]NAME.TYPE;VERSION), without
 * touching it.
 *
 * This is deliberately NOT the same as taking the basename of whatever
 * dcl_resolve_path()/dcl_resolve_filespec() resolved: for a bare name
 * (no ";N"), filespec resolution may collapse it down to one literal
 * on-disk version (see dcl_filespec.c's resolve_version_suffix(), which
 * exists so single-file commands like TYPE/COPY/DELETE can fopen() the
 * real "name.type;N" file a bare reference implies). DIRECTORY and PURGE
 * must NOT see that collapsed name — VMS's own rule is that a bare name
 * lists/considers ALL versions, only an explicit ";N" narrows to one.
 * Callers that need "match every version" semantics (via
 * vmsfs_wildcard_match(), which already knows the version rules) must
 * feed it the version marker exactly as the user typed it, not a
 * synthesized one.
 *
 * Returns NULL for a raw Linux path passthrough (starts with '/', './'
 * or '../' — see dcl_resolve_path()), which carries no VMS version
 * marker to preserve; the caller should fall back to its own basename
 * logic in that case.
 */
static const char *dcl_filename_component(const char *spec)
{
    if (!spec) return NULL;
    if (spec[0] == '/' || (spec[0] == '.' && (spec[1] == '/' || spec[1] == '.')))
        return NULL;

    const char *p = spec;
    const char *bracket_end = strrchr(spec, ']');
    if (bracket_end) {
        p = bracket_end + 1;
    } else {
        const char *col = strrchr(spec, ':');
        if (col) p = col + 1;
    }
    return p;
}

/*
 * dcl_print_dir_total - Emit the DIRECTORY "Total of ..." trailer in the exact
 * form real OpenVMS uses.
 *
 * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY command
 * examples (OpenVMS DCL Dictionary, DIRECTORY —
 * www0.mi.infn.it/~calcolo/OpenVMS/ssb71/9996/9996p013.htm):
 *   - default listing (no size shown):  "Total of 4 files."   (NO block count)
 *   - /SIZE (blocks used):              "Total of 4 files, 15 blocks."
 *   - /FULL (used/allocated):           "Total of 1 file, 390/390 blocks."
 * i.e. the block count appears ONLY when file sizes are displayed. The prior
 * OVMX code always printed ", M blocks." even for a bare DIRECTORY, which a VMS
 * user spots on the first listing.
 */
static void dcl_print_dir_total(int file_count, long used_blocks,
                                long alloc_blocks, int show_size, int show_full,
                                enum dir_size_mode size_mode)
{
    const char *files_s = (file_count != 1) ? "s" : "";
    const char *blk_s   = (used_blocks != 1) ? "s" : "";
    if (show_full || (show_size && size_mode == DIR_SIZE_ALL)) {
        printf("\nTotal of %d file%s, %ld/%ld block%s.\n",
               file_count, files_s, used_blocks, alloc_blocks, blk_s);
    } else if (show_size && size_mode == DIR_SIZE_ALLOC) {
        printf("\nTotal of %d file%s, %ld block%s.\n",
               file_count, files_s, alloc_blocks,
               (alloc_blocks != 1) ? "s" : "");
    } else if (show_size) {
        printf("\nTotal of %d file%s, %ld block%s.\n",
               file_count, files_s, used_blocks, blk_s);
    } else {
        printf("\nTotal of %d file%s.\n", file_count, files_s);
    }
}

/* Compare directory entries: name ascending (case-insensitive), version descending */
static int dir_entry_cmp(const void *a, const void *b)
{
    const struct dir_entry *ea = (const struct dir_entry *)a;
    const struct dir_entry *eb = (const struct dir_entry *)b;

    /* Compare name without version */
    char na[256], nb[256];
    strncpy(na, ea->vms_name, sizeof(na) - 1); na[sizeof(na)-1]='\0';
    strncpy(nb, eb->vms_name, sizeof(nb) - 1); nb[sizeof(nb)-1]='\0';
    char *sa = strrchr(na, ';'); if (sa) *sa = '\0';
    char *sb = strrchr(nb, ';'); if (sb) *sb = '\0';

    int cmp = strcasecmp(na, nb);
    if (cmp != 0) return cmp;

    /* Same name: sort by version descending */
    return eb->version - ea->version;
}

/*
 * dir_opts - the display flags a single directory scan needs, packaged so
 * the per-directory collect/print helpers can be shared between the single-
 * directory and the multi-directory (ellipsis) paths.
 */
struct dir_opts {
    int show_size, show_date, show_full, show_brief, show_owner,
        show_protection, suppress_files, columns, versions_limit;
    enum dir_size_mode size_mode;
};

/*
 * dir_collect - Scan ONE Linux directory, applying the VMS filename
 * wildcard/exclude filter through the single matcher vmsfs_wildcard_match(),
 * and return the matching entries sorted (name asc, version desc).
 *
 * INV-6 / no facade: this is a real opendir()/readdir()/stat() walk of the
 * on-disk vmsfs tree — every listed file exists on disk.
 *
 * Returns SS$_NORMAL with *out_entries (malloc'd; caller frees) and
 * *out_count set; SS$_NOSUCHFILE if the directory cannot be opened;
 * SS$_INSFMEM on allocation failure.
 */
static int dir_collect(const char *linux_dir, const char *pattern,
                       char **excl_pats, int excl_count,
                       struct dir_entry **out_entries, int *out_count)
{
    *out_entries = NULL;
    *out_count = 0;

    DIR *dir = opendir(linux_dir);
    if (!dir) return SS$_NOSUCHFILE;

    int capacity = 256;
    struct dir_entry *entries = malloc((size_t)capacity * sizeof(*entries));
    if (!entries) { closedir(dir); return SS$_INSFMEM; }
    int entry_count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) continue;

        /* VMS filename wildcard filter — the ONE matcher (% single-char,
         * * any-chars, version-aware). */
        if (pattern) {
            if (!vmsfs_wildcard_match(pattern, de->d_name)) continue;
        }

        /* /EXCLUDE: skip entries matching any exclusion spec, same matcher. */
        if (excl_count > 0) {
            int excluded = 0;
            for (int xi = 0; xi < excl_count; xi++) {
                if (vmsfs_wildcard_match(excl_pats[xi], de->d_name)) {
                    excluded = 1;
                    break;
                }
            }
            if (excluded) continue;
        }

        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s%s", linux_dir, de->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (entry_count >= capacity) {
            capacity *= 2;
            struct dir_entry *tmp = realloc(entries,
                                            (size_t)capacity * sizeof(*entries));
            if (!tmp) { free(entries); closedir(dir); return SS$_INSFMEM; }
            entries = tmp;
        }

        struct dir_entry *e = &entries[entry_count];
        e->st = st;
        e->blocks = (st.st_size + 511) / 512;
        e->has_btime = ovmx_file_btime(full_path, &e->btime);
        strncpy(e->raw_name, de->d_name, sizeof(e->raw_name) - 1);
        e->raw_name[sizeof(e->raw_name) - 1] = '\0';

        size_t ni = 0;
        for (size_t i = 0; de->d_name[i] && ni < sizeof(e->vms_name) - 1; i++) {
            e->vms_name[ni++] = (char)toupper((unsigned char)de->d_name[i]);
        }
        e->vms_name[ni] = '\0';

        char *semi = strrchr(e->vms_name, ';');
        if (semi && semi[1] != '\0') {
            e->version = (int)strtol(semi + 1, NULL, 10);
        } else if (S_ISREG(st.st_mode)) {
            e->version = 1;
            strncat(e->vms_name, ";1",
                    sizeof(e->vms_name) - strlen(e->vms_name) - 1);
        } else {
            e->version = 0;
        }

        if (S_ISDIR(st.st_mode)) {
            strncat(e->vms_name, ".DIR;1",
                    sizeof(e->vms_name) - strlen(e->vms_name) - 1);
        }

        if (S_ISREG(st.st_mode) && !strchr(de->d_name, '.')) {
            char *s = strrchr(e->vms_name, ';');
            if (s) {
                memmove(s + 1, s, strlen(s) + 1);
                *s = '.';
            }
        }

        entry_count++;
    }
    closedir(dir);

    qsort(entries, (size_t)entry_count, sizeof(struct dir_entry), dir_entry_cmp);

    *out_entries = entries;
    *out_count = entry_count;
    return SS$_NORMAL;
}

/*
 * dir_collect_host - vms-5f0 executive-absent DIRECTORY defer. When /dev/vms /
 * the Files-11 ACP is unreachable (host ctest, plain-container self-host/link
 * gates -- rms_executive_absent()), DCL cannot list through the ACP, so it
 * defers to the SAME legacy resolver RMS ($OPEN/$SEARCH) and IMGACT already fall
 * back to: dcl_resolve_path() (the proven translator for logicals, the volume
 * root "[000000]" and on-disk case) + the opendir()-based dir_collect() above,
 * which synthesizes "NAME.DIR;1" for subdirectories and ";1" versions exactly as
 * the pre-flip DIRECTORY did. `dirspec` names the directory (VMS "DEV:[DIR]" or
 * default); `filepat` is the VMS file pattern to match, or NULL to list the
 * whole directory (files AND subdirectories). This runs ONLY when the executive
 * is absent; with /dev/vms present DIRECTORY stays on dir_collect_acp (INV-6).
 */
static int dir_collect_host(struct dcl_context *ctx, const char *dirspec,
                            const char *filepat, char **excl_pats,
                            int excl_count, struct dir_entry **out_entries,
                            int *out_count)
{
    char linux_dir[1024];
    if (dcl_resolve_path(ctx, (dirspec && dirspec[0]) ? dirspec : ctx->default_dir,
                         linux_dir, sizeof(linux_dir)) != 0)
        return SS$_NOSUCHFILE;

    size_t dl = strlen(linux_dir);
    if (dl > 0 && linux_dir[dl - 1] != '/' && dl < sizeof(linux_dir) - 1) {
        linux_dir[dl] = '/';
        linux_dir[dl + 1] = '\0';
    }

    /* An empty or version-only pattern lists the whole directory. */
    const char *pat = (filepat && filepat[0]) ? filepat : NULL;
    return dir_collect(linux_dir, pat, excl_pats, excl_count,
                       out_entries, out_count);
}

/*
 * dir_collect_acp - vms-481: collect one directory's matching entries through
 * the Files-11 ODS-2 ACP (sys$parse + sys$search wildcard directory context)
 * instead of opendir()/readdir()/stat() on a /vms passthrough. Each entry
 * carries the GENUINE File ID the search returned and the on-disk header
 * attributes (size/allocation/protection/owner/dates) read via rms_file_attr
 * -- so DIRECTORY /FULL emits a real File ID, verified against the codec, not a
 * synthesized one (INV-6). `vms_pattern` is a VMS wildcard filespec (e.g.
 * "DKA0:[DIR]*.*;*"). Fail-honest: no ACP-mounted SYS$DISK => 0 entries.
 *
 * Returns SS$_NORMAL with *out_entries (malloc'd; caller frees) and *out_count.
 */
static int dir_collect_acp(struct dcl_context *ctx, const char *vms_pattern,
                           char **excl_pats, int excl_count,
                           struct dir_entry **out_entries, int *out_count)
{
    *out_entries = NULL;
    *out_count = 0;

    struct dcl_rms_dir *d = dcl_rms_dir_open(ctx, vms_pattern);
    if (!d) return SS$_NOSUCHFILE;

    int capacity = 256;
    struct dir_entry *entries = malloc((size_t)capacity * sizeof(*entries));
    if (!entries) { dcl_rms_dir_close(d); return SS$_INSFMEM; }
    int entry_count = 0;

    char match[1024];
    uint16_t fnum, fseq; uint8_t frvn;
    while (dcl_rms_dir_next(d, match, sizeof(match), &fnum, &fseq, &frvn)) {
        /* Resultant is "DEV:[DIR]NAME.TYP;VER" -- take the NAME.TYP;VER tail. */
        const char *nt = match;
        const char *rb = strrchr(match, ']');
        if (!rb) rb = strrchr(match, '>');
        if (rb) nt = rb + 1;

        /* /EXCLUDE: skip entries matching any exclusion spec (same VMS matcher). */
        if (excl_count > 0) {
            int excluded = 0;
            for (int xi = 0; xi < excl_count; xi++) {
                if (vmsfs_wildcard_match(excl_pats[xi], nt)) { excluded = 1; break; }
            }
            if (excluded) continue;
        }

        if (entry_count >= capacity) {
            capacity *= 2;
            struct dir_entry *tmp = realloc(entries,
                                            (size_t)capacity * sizeof(*entries));
            if (!tmp) { free(entries); dcl_rms_dir_close(d); return SS$_INSFMEM; }
            entries = tmp;
        }

        struct dir_entry *e = &entries[entry_count];
        memset(e, 0, sizeof(*e));
        e->from_acp = 1;
        e->fid_num = fnum; e->fid_seq = fseq; e->fid_rvn = frvn;
        strncpy(e->vms_name, nt, sizeof(e->vms_name) - 1);
        e->vms_name[sizeof(e->vms_name) - 1] = '\0';
        strncpy(e->raw_name, nt, sizeof(e->raw_name) - 1);
        e->raw_name[sizeof(e->raw_name) - 1] = '\0';
        char *semi = strrchr(e->vms_name, ';');
        e->version = (semi && semi[1]) ? (int)strtol(semi + 1, NULL, 10) : 1;

        /* Genuine ODS-2 header attributes (real File ID we already have). */
        struct rms_fileattr at;
        if (rms_file_attr(match, &at) == RMS$_NORMAL) {
            long used  = at.efblk ? (at.ffbyte ? (long)at.efblk
                                                : (long)at.efblk - 1) : 0;
            if (used < 0) used = 0;
            e->blocks       = used;
            e->alloc_blocks = (long)at.hiblk;
            e->vms_prot     = at.fileprot;
            e->st.st_size   = (off_t)used * 512 + at.ffbyte;
            e->st.st_blocks = (blkcnt_t)at.hiblk;
            e->st.st_gid    = at.uic_group;
            e->st.st_uid    = at.uic_member;
            e->st.st_mode   = at.is_directory ? (S_IFDIR | 0755) : (S_IFREG | 0644);
            memcpy(e->credate, at.credate, 8);
            memcpy(e->revdate, at.revdate, 8);
            e->has_cre = (memcmp(at.credate, "\0\0\0\0\0\0\0\0", 8) != 0);
            e->has_rev = (memcmp(at.revdate, "\0\0\0\0\0\0\0\0", 8) != 0);
        } else {
            e->st.st_mode = S_IFREG | 0644;
        }

        entry_count++;
    }
    dcl_rms_dir_close(d);

    qsort(entries, (size_t)entry_count, sizeof(struct dir_entry), dir_entry_cmp);
    *out_entries = entries;
    *out_count = entry_count;
    return SS$_NORMAL;
}

/*
 * dir_print_entries - Print the file listing for one already-collected,
 * already-sorted directory, honoring the display qualifiers, and return the
 * listed file count and block totals. Shared by the single-directory and
 * multi-directory paths so their per-file output is byte-identical.
 */
static void dir_print_entries(const struct dir_entry *entries, int entry_count,
                              const struct dir_opts *o,
                              int *out_files, long *out_used, long *out_alloc)
{
    int file_count = 0;
    long total_blocks = 0;
    long total_alloc = 0;
    int col = 0;
    int col_width = (o->show_size || o->show_date) ? 0 : (80 / o->columns);

    char ver_prev_base[288] = "";
    int  ver_group_seen = 0;

    for (int idx = 0; idx < entry_count; idx++) {
        const struct dir_entry *e = &entries[idx];
        const char *vms_name = e->vms_name;
        long blocks = e->blocks;
        const struct stat *st = &e->st;

        if (o->versions_limit > 0) {
            char base[288];
            strncpy(base, vms_name, sizeof(base) - 1);
            base[sizeof(base) - 1] = '\0';
            char *semi = strrchr(base, ';');
            if (semi) *semi = '\0';
            if (strcasecmp(base, ver_prev_base) != 0) {
                strncpy(ver_prev_base, base, sizeof(ver_prev_base) - 1);
                ver_prev_base[sizeof(ver_prev_base) - 1] = '\0';
                ver_group_seen = 0;
            }
            ver_group_seen++;
            if (ver_group_seen > o->versions_limit) continue;
        }

        total_blocks += blocks;
        total_alloc += (long)st->st_blocks;
        file_count++;

        if (o->suppress_files) continue;

        if (o->show_full) {
            /* DIRECTORY/FULL: the authentic VMS multi-line per-file block.
             * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary,
             * DIRECTORY/FULL example output. Every field below is sourced from
             * REAL file metadata; fields VMS stores in an ODS-2 file header that
             * the live host-FS passthrough does NOT carry (File ID, file
             * organization, record format/attributes, longest-record length)
             * are OMITTED rather than fabricated (INV-6 / vms-5eb) -- see the
             * vms-5e2 PR's source-and-gap table. */
            long alloc = e->from_acp ? e->alloc_blocks
                                     : (long)st->st_blocks;   /* on-disk allocation */

            /* Line 1: file name, and -- when the entry came from the ACP -- the
             * GENUINE ODS-2 File ID (num,seq,rvn) from the directory search, the
             * fidelity gap the /vms passthrough could not fill (vms-481). */
            if (e->from_acp)
                printf("%-30s File ID:  (%u,%u,%u)\n",
                       vms_name, e->fid_num, e->fid_seq, e->fid_rvn);
            else
                printf("%s\n", vms_name);

            /* Size (used/allocated) + Owner UIC [group,member]. */
            char sizebuf[32];
            snprintf(sizebuf, sizeof(sizebuf), "%ld/%ld", blocks, alloc);
            printf("Size:            %-16sOwner:    [%03o,%03o]\n",
                   sizebuf,
                   (unsigned)(st->st_gid & 0377),
                   (unsigned)(st->st_uid & 0377));

            /* Created / Revised. From the ACP these are the file header's real
             * ODS-2 ATR$C_CREDATE / _REVDATE; from the passthrough they are the
             * birth time (or an honest gap) and the real mtime. */
            char datebuf[40];
            if (e->from_acp) {
                if (e->has_cre && dir_format_vmsbintime(e->credate, datebuf,
                                                        sizeof(datebuf)))
                    printf("Created:  %s\n", datebuf);
                else
                    printf("Created:  <not recorded>\n");
                if (e->has_rev && dir_format_vmsbintime(e->revdate, datebuf,
                                                        sizeof(datebuf)))
                    printf("Revised:  %s\n", datebuf);
                else
                    printf("Revised:  <not recorded>\n");
            } else {
                if (e->has_btime) {
                    dir_format_vmsdate(e->btime, datebuf, sizeof(datebuf));
                    printf("Created:  %s\n", datebuf);
                } else {
                    printf("Created:  <not recorded>\n");
                }
                dir_format_vmsdate(st->st_mtim, datebuf, sizeof(datebuf));
                printf("Revised:  %s\n", datebuf);
            }

            /* Expiration/backup dates are genuinely unset here -- VMS prints
             * exactly these strings for a file that has none. */
            printf("Expired:  <None specified>\n");
            printf("Backup:   <No backup recorded>\n");

            /* File protection (long form): the genuine ODS-2 protection word
             * from the ACP header, else derived from the passthrough st_mode. */
            uint16_t vprot = e->from_acp ? e->vms_prot
                                         : vmsfs_mode_to_protection(st->st_mode);
            char protbuf[80];
            dir_format_prot_full(vprot, protbuf, sizeof(protbuf));
            printf("File protection:    %s\n", protbuf);

            printf("\n");
        } else if (o->show_size || o->show_date || o->show_owner ||
                   o->show_protection) {
            printf("%-39s", vms_name);
            if (o->show_size) {
                /* /SIZE=USED (default) blocks used; /SIZE=ALLOCATION allocated;
                 * /SIZE=ALL "used/allocated". */
                if (o->size_mode == DIR_SIZE_ALLOC) {
                    printf(" %6ld", (long)st->st_blocks);
                } else if (o->size_mode == DIR_SIZE_ALL) {
                    char sb[32];
                    snprintf(sb, sizeof(sb), "%ld/%ld", blocks,
                             (long)st->st_blocks);
                    printf(" %11s", sb);
                } else {
                    printf(" %6ld", blocks);
                }
            }
            if (o->show_date) {
                char datebuf[40];
                if (e->from_acp) {
                    if (!(e->has_rev && dir_format_vmsbintime(e->revdate, datebuf,
                                                              sizeof(datebuf))))
                        snprintf(datebuf, sizeof(datebuf), "<not recorded>");
                } else {
                    dir_format_vmsdate(st->st_mtim, datebuf, sizeof(datebuf));
                }
                printf("  %s", datebuf);
            }
            if (o->show_owner) {
                printf(" [%03o,%03o]",
                       (unsigned)(st->st_gid & 0377),
                       (unsigned)(st->st_uid & 0377));
            }
            if (o->show_protection) {
                uint16_t vprot = e->from_acp ? e->vms_prot
                                             : vmsfs_mode_to_protection(st->st_mode);
                char prot_buf[64];
                vmsfs_format_protection(vprot, prot_buf, sizeof(prot_buf));
                printf(" %s", prot_buf);
            }
            printf("\n");
        } else if (o->show_brief) {
            printf("%s\n", vms_name);
        } else {
            if (col_width < 1) col_width = 20;
            printf("%-*s", col_width, vms_name);
            col++;
            if (col >= o->columns) {
                printf("\n");
                col = 0;
            }
        }
    }

    /* Finish last line of columnar output */
    if (col > 0 && !o->show_size && !o->show_date && !o->show_full &&
        !o->show_brief && !o->show_owner && !o->show_protection &&
        !o->suppress_files) {
        printf("\n");
    }

    *out_files = file_count;
    *out_used = total_blocks;
    *out_alloc = total_alloc;
}

/*
 * dir_gather_tree - Depth-first collect `base` plus every subdirectory
 * below it (the real on-disk vmsfs tree), each with a trailing slash, into a
 * malloc'd, case-insensitively sorted list. This is the concrete walk the
 * VMS "[...]" ellipsis directory wildcard names. Returns 0 on success.
 */
static void dir_gather_recurse(const char *d, char ***list, int *count, int *cap)
{
    /* Append d */
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char **tmp = realloc(*list, (size_t)nc * sizeof(char *));
        if (!tmp) return;
        *list = tmp;
        *cap = nc;
    }
    (*list)[(*count)++] = strdup(d);

    DIR *dir = opendir(d);
    if (!dir) return;

    /* Collect child subdir names first, then recurse in sorted order. */
    char **subs = NULL; int scount = 0, scap = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[2048];
        snprintf(child, sizeof(child), "%s%s", d, de->d_name);
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (scount >= scap) {
            int nc = scap ? scap * 2 : 8;
            char **tmp = realloc(subs, (size_t)nc * sizeof(char *));
            if (!tmp) break;
            subs = tmp; scap = nc;
        }
        char childslash[2049];
        snprintf(childslash, sizeof(childslash), "%s/", child);
        subs[scount++] = strdup(childslash);
    }
    closedir(dir);

    /* Sort children so the tree is emitted in a stable VMS-like order. */
    for (int i = 0; i < scount; i++) {
        for (int j = i + 1; j < scount; j++) {
            if (strcasecmp(subs[i], subs[j]) > 0) {
                char *t = subs[i]; subs[i] = subs[j]; subs[j] = t;
            }
        }
    }
    for (int i = 0; i < scount; i++) {
        dir_gather_recurse(subs[i], list, count, cap);
        free(subs[i]);
    }
    free(subs);
}

/*
 * dir_spec_child - vms-481: build the VMS directory spec of a subdirectory.
 * base is "DEV:[DIR]" (or "DEV:[000000]" for the MFD); sub is the subdirectory
 * NAME (from a "NAME.DIR" entry). Returns "DEV:[DIR.SUB]" (or "DEV:[SUB]" from
 * the MFD). Returns 0 on success, -1 if base has no bracketed directory.
 */
static int dir_spec_child(const char *base, const char *sub,
                          char *out, size_t outsz)
{
    const char *lb = strchr(base, '[');
    const char *rb = lb ? strchr(lb, ']') : NULL;
    if (!lb || !rb || rb <= lb) return -1;
    char prefix[256];
    size_t pl = (size_t)(lb - base);
    if (pl >= sizeof(prefix)) pl = sizeof(prefix) - 1;
    memcpy(prefix, base, pl); prefix[pl] = '\0';
    char dir[512];
    size_t dl = (size_t)(rb - lb - 1);
    if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
    memcpy(dir, lb + 1, dl); dir[dl] = '\0';
    if (dir[0] == '\0' || strcmp(dir, "000000") == 0)
        snprintf(out, outsz, "%s[%s]", prefix, sub);
    else
        snprintf(out, outsz, "%s[%s.%s]", prefix, dir, sub);
    return 0;
}

/*
 * dir_gather_acp - vms-481: depth-first collect a VMS directory spec plus every
 * subdirectory below it, through the ACP wildcard search for "*.DIR" (the
 * genuine ODS-2 directory tree, not opendir). This is the ellipsis "[...]" walk
 * done the VMS way. Appends each directory spec (malloc'd) to *list.
 */
static void dir_gather_acp(struct dcl_context *ctx, const char *dirspec,
                           char ***list, int *count, int *cap, int depth)
{
    if (depth > 32) return;   /* guard against a pathological / cyclic tree */

    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char **tmp = realloc(*list, (size_t)nc * sizeof(char *));
        if (!tmp) return;
        *list = tmp; *cap = nc;
    }
    (*list)[(*count)++] = strdup(dirspec);

    char subpat[1024];
    snprintf(subpat, sizeof(subpat), "%s*.DIR;*", dirspec);
    struct dcl_rms_dir *d = dcl_rms_dir_open(ctx, subpat);
    if (!d) return;

    char match[1024];
    while (dcl_rms_dir_next(d, match, sizeof(match), NULL, NULL, NULL)) {
        const char *nt = match;
        const char *rb = strrchr(match, ']');
        if (!rb) rb = strrchr(match, '>');
        if (rb) nt = rb + 1;
        /* nt is "NAME.DIR;VER" -- take NAME. */
        char sub[256];
        size_t i = 0;
        for (; nt[i] && nt[i] != '.' && i < sizeof(sub) - 1; i++) sub[i] = nt[i];
        sub[i] = '\0';
        if (sub[0] == '\0') continue;
        /* Skip the MFD self-reference 000000.DIR. */
        if (strcasecmp(sub, "000000") == 0) continue;
        char child[1024];
        if (dir_spec_child(dirspec, sub, child, sizeof(child)) == 0)
            dir_gather_acp(ctx, child, list, count, cap, depth + 1);
    }
    dcl_rms_dir_close(d);
}

/*
 * dir_gather_host - vms-5f0 executive-absent counterpart to dir_gather_acp: the
 * ellipsis "..." subtree walk when /dev/vms is unreachable. dir_gather_acp finds
 * subdirectories by searching "*.DIR;*" through the ACP, but the /vms passthrough
 * stores a subdirectory as a real POSIX directory (no "NAME.DIR" file), so that
 * search returns nothing on host. Walk the real directory with opendir()/stat()
 * instead -- the same on-disk depth-first traversal the pre-flip DIRECTORY used
 * -- composing each child's VMS spec via dir_spec_child(). Runs ONLY when the
 * executive is absent; with /dev/vms present the ACP walk is authoritative.
 */
static void dir_gather_host(struct dcl_context *ctx, const char *dirspec,
                            char ***list, int *count, int *cap, int depth)
{
    if (depth > 32) return;   /* guard against a pathological / cyclic tree */

    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char **tmp = realloc(*list, (size_t)nc * sizeof(char *));
        if (!tmp) return;
        *list = tmp; *cap = nc;
    }
    (*list)[(*count)++] = strdup(dirspec);

    char linux_dir[1024];
    if (dcl_resolve_path(ctx, dirspec, linux_dir, sizeof(linux_dir)) != 0)
        return;
    DIR *d = opendir(linux_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char full[2048];
        snprintf(full, sizeof(full), "%s/%s", linux_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char sub[256];
        size_t i = 0;
        for (; de->d_name[i] && i < sizeof(sub) - 1; i++)
            sub[i] = (char)toupper((unsigned char)de->d_name[i]);
        sub[i] = '\0';
        if (sub[0] == '\0' || strcasecmp(sub, "000000") == 0) continue;

        char child[1024];
        if (dir_spec_child(dirspec, sub, child, sizeof(child)) == 0)
            dir_gather_host(ctx, child, list, count, cap, depth + 1);
    }
    closedir(d);
}

/*
 * dir_deellipsize - Rewrite a VMS directory spec that contains the "..."
 * ellipsis wildcard into a plain, resolvable directory spec naming the START
 * of the tree (the ellipsis and everything from it is dropped), and report
 * whether an ellipsis was present.
 *
 *   "[...]*.TXT"     -> "*.TXT"       (start = current default dir)
 *   "[MYDIR...]*.C"  -> "[MYDIR]*.C"  (start = top-level [MYDIR])
 *   "[.SUB...]F.T"   -> "[.SUB]F.T"   (start = default's [.SUB])
 *
 * The DIRECTORY command then resolves the rewritten spec exactly as it does a
 * non-ellipsis spec, and walks the tree from the resolved start directory.
 *
 * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary — the ellipsis
 * ("...") in a directory specification means "this directory and all
 * subdirectories below it"; DIRECTORY over such a spec produces a
 * per-directory listing plus a "Grand total" line.
 */
static int dir_deellipsize(const char *spec, char *out, size_t out_sz)
{
    const char *lb = strchr(spec, '[');
    const char *rb = lb ? strchr(lb, ']') : NULL;
    if (!lb || !rb) {
        strncpy(out, spec, out_sz - 1);
        out[out_sz - 1] = '\0';
        return 0;
    }

    size_t dlen = (size_t)(rb - (lb + 1));
    char dtext[512];
    if (dlen >= sizeof(dtext)) dlen = sizeof(dtext) - 1;
    memcpy(dtext, lb + 1, dlen);
    dtext[dlen] = '\0';

    char *ell = strstr(dtext, "...");
    if (!ell) {
        strncpy(out, spec, out_sz - 1);
        out[out_sz - 1] = '\0';
        return 0;
    }
    *ell = '\0';                 /* drop "..." and everything after it */

    size_t before = (size_t)(lb - spec);
    char pre[600];
    if (before >= sizeof(pre)) before = sizeof(pre) - 1;
    memcpy(pre, spec, before);
    pre[before] = '\0';
    const char *post = rb + 1;

    if (dtext[0])
        snprintf(out, out_sz, "%s[%s]%s", pre, dtext, post);
    else
        snprintf(out, out_sz, "%s%s", pre, post);
    return 1;
}

int cmd_directory(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* vms-481: DIRECTORY lists through the Files-11 ODS-2 ACP (sys$parse +
     * sys$search wildcard directory context + rms_file_attr), emitting the
     * genuine File ID and on-disk attributes -- not opendir()/stat() on a /vms
     * passthrough. The listing is resolved as a VMS filespec throughout. */

    /* Detect and strip a "..." ellipsis directory wildcard. `use_spec` is the
     * spec with the ellipsis removed (naming the START of the tree); when
     * has_ellipsis is set the listing walks the resolved directory's whole
     * subtree via the ACP (dir_gather_acp). */
    char despec[1024];
    int has_ellipsis = 0;
    const char *use_spec = NULL;
    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        has_ellipsis = dir_deellipsize(cmd->params[0], despec, sizeof(despec));
        use_spec = despec;
    }

    /* The full VMS wildcard filespec the ACP search iterates. A spec ending in
     * a directory/device terminator (']' '>' ':') lists all files there; an
     * empty spec lists the process default directory. */
    char vms_pattern[1024];
    if (use_spec && use_spec[0] != '\0')
        dcl_rms_effective_spec(ctx, use_spec, vms_pattern, sizeof(vms_pattern));
    else
        dcl_rms_effective_spec(ctx, "*.*;*", vms_pattern, sizeof(vms_pattern));
    {
        size_t L = strlen(vms_pattern);
        char last = L ? vms_pattern[L - 1] : 0;
        if (last == ']' || last == '>' || last == ':')
            strncat(vms_pattern, "*.*;*", sizeof(vms_pattern) - L - 1);
    }

    /* Split the effective pattern into a directory-spec prefix and a file
     * pattern (used by the ellipsis subtree walk to re-anchor per directory). */
    char vms_base[1024], vms_filepat[256];
    {
        const char *rb = strrchr(vms_pattern, ']');
        if (!rb) rb = strrchr(vms_pattern, '>');
        if (!rb) { const char *cn = strrchr(vms_pattern, ':'); if (cn) rb = cn; }
        if (rb) {
            size_t pl = (size_t)(rb - vms_pattern) + 1;
            if (pl >= sizeof(vms_base)) pl = sizeof(vms_base) - 1;
            memcpy(vms_base, vms_pattern, pl); vms_base[pl] = '\0';
            strncpy(vms_filepat, rb + 1, sizeof(vms_filepat) - 1);
            vms_filepat[sizeof(vms_filepat) - 1] = '\0';
        } else {
            vms_base[0] = '\0';
            strncpy(vms_filepat, vms_pattern, sizeof(vms_filepat) - 1);
            vms_filepat[sizeof(vms_filepat) - 1] = '\0';
        }
        if (vms_filepat[0] == '\0')
            strncpy(vms_filepat, "*.*;*", sizeof(vms_filepat) - 1);
    }

    /* Check qualifiers */
    int show_size = dcl_has_qualifier(cmd, "SIZE");
    /* /SIZE=option → block-count display mode. */
    enum dir_size_mode size_mode = DIR_SIZE_USED;
    const char *size_val = dcl_qualifier_value(cmd, "SIZE");
    if (size_val && strcasecmp(size_val, "ALLOCATION") == 0)
        size_mode = DIR_SIZE_ALLOC;
    else if (size_val && strcasecmp(size_val, "ALL") == 0)
        size_mode = DIR_SIZE_ALL;
    int show_date = dcl_has_qualifier(cmd, "DATE");
    int show_full = dcl_has_qualifier(cmd, "FULL");
    int show_brief = dcl_has_qualifier(cmd, "BRIEF");
    int show_owner = dcl_has_qualifier(cmd, "OWNER");
    int show_total = dcl_has_qualifier(cmd, "TOTAL");
    int show_grand_total = dcl_has_qualifier(cmd, "GRAND_TOTAL");
    /* /HEADING is default on; /NOHEADING suppresses it.
     * The parser stores /NOHEADING as name="HEADING" negated=1,
     * and dcl_has_qualifier returns 0 for negated qualifiers.
     * So: if HEADING qualifier is absent → show (default).
     *     if HEADING qualifier is present and not negated → show.
     *     if HEADING qualifier is present and negated → hide.
     * We detect negation by scanning the qualifiers directly. */
    int show_heading = 1;
    for (int qi = 0; qi < cmd->qualifier_count; qi++) {
        if (strcasecmp(cmd->qualifiers[qi].name, "HEADING") == 0) {
            show_heading = !cmd->qualifiers[qi].negated;
            break;
        }
    }
    int show_trailing = dcl_has_qualifier(cmd, "TRAILING");
    int show_protection = dcl_has_qualifier(cmd, "PROTECTION");
    int columns = 4;
    const char *col_val = dcl_qualifier_value(cmd, "COLUMNS");
    if (col_val && col_val[0]) {
        char *endp;
        int c = (int)strtol(col_val, &endp, 10);
        if (endp != col_val && *endp == '\0') columns = c;
    }
    if (columns < 1) columns = 1;
    if (columns > 8) columns = 8;

    /* /VERSIONS=n: list at most n versions of each file (0/absent = all).
     * Grounded: DCL Dictionary DIRECTORY /VERSIONS=n. */
    int versions_limit = 0;
    const char *ver_val = dcl_qualifier_value(cmd, "VERSIONS");
    if (ver_val && ver_val[0]) {
        char *endp;
        long v = strtol(ver_val, &endp, 10);
        if (endp != ver_val && *endp == '\0' && v >= 1) versions_limit = (int)v;
    }

    /* /EXCLUDE=(spec[,...]): omit files matching any spec, using the same VMS
     * wildcard engine as the positional pattern. The parser stores a list as
     * "(a,b,c)"; strip the parens and split on commas. Grounded: DCL
     * Dictionary DIRECTORY /EXCLUDE=(file-spec[,...]). */
    #define DIR_MAX_EXCLUDE 16
    char *excl_pats[DIR_MAX_EXCLUDE];
    int excl_count = 0;
    char excl_buf[512];
    const char *excl_val = dcl_qualifier_value(cmd, "EXCLUDE");
    if (excl_val && excl_val[0]) {
        const char *s = excl_val;
        size_t bl = strlen(s);
        /* Strip a single surrounding (...) if present. */
        if (s[0] == '(' && bl >= 2 && s[bl - 1] == ')') {
            strncpy(excl_buf, s + 1, sizeof(excl_buf) - 1);
            excl_buf[sizeof(excl_buf) - 1] = '\0';
            size_t el = strlen(excl_buf);
            if (el > 0 && excl_buf[el - 1] == ')') excl_buf[el - 1] = '\0';
        } else {
            strncpy(excl_buf, s, sizeof(excl_buf) - 1);
            excl_buf[sizeof(excl_buf) - 1] = '\0';
        }
        char *tok = strtok(excl_buf, ",");
        while (tok && excl_count < DIR_MAX_EXCLUDE) {
            while (*tok == ' ') tok++;
            if (*tok) excl_pats[excl_count++] = tok;
            tok = strtok(NULL, ",");
        }
    }

    if (show_full) {
        show_size = 1;
        show_date = 1;
        show_owner = 1;
        show_protection = 1;
    }

    /* If /TOTAL or /GRAND_TOTAL, suppress individual file listing */
    int suppress_files = show_total || show_grand_total;

    /* Package the display flags once for the per-directory helpers so the
     * single-directory and multi-directory (ellipsis) paths render files
     * identically. */
    struct dir_opts opts;
    opts.show_size = show_size;
    opts.show_date = show_date;
    opts.show_full = show_full;
    opts.show_brief = show_brief;
    opts.show_owner = show_owner;
    opts.show_protection = show_protection;
    opts.suppress_files = suppress_files;
    opts.columns = columns;
    opts.versions_limit = versions_limit;
    opts.size_mode = size_mode;

    char vms_dir[512];   /* VMS display spec of the (last) directory listed */
    vms_dir[0] = '\0';

    if (!has_ellipsis) {
        /* ---------------- Single-directory listing ---------------- */
        struct dir_entry *entries = NULL;
        int entry_count = 0;
        int cst;
        if (rms_executive_absent()) {
            /* Executive absent (host ctest / self-host container): defer to the
             * legacy opendir resolver (vms-5f0). A spec that names only a
             * directory ("DEV:[DIR]", "[SUB]", empty=default) lists the WHOLE
             * directory -- files AND "NAME.DIR;1" subdirectories -- so pass a
             * NULL file pattern; an explicit file pattern is matched as given. */
            int dir_target = (!use_spec || !use_spec[0]);
            if (use_spec && use_spec[0]) {
                size_t L = strlen(use_spec);
                char lc = use_spec[L - 1];
                dir_target = (lc == ']' || lc == '>' || lc == ':');
            }
            cst = dir_collect_host(ctx,
                                   vms_base[0] ? vms_base : NULL,
                                   dir_target ? NULL : vms_filepat,
                                   excl_pats, excl_count,
                                   &entries, &entry_count);
        } else {
            cst = dir_collect_acp(ctx, vms_pattern, excl_pats, excl_count,
                                  &entries, &entry_count);
        }
        if (cst == SS$_NOSUCHFILE) {
            dcl_error("RMS", 2, "DNF", "directory not found - %s", vms_pattern);
            return SS$_NOSUCHFILE;
        }
        if (cst != SS$_NORMAL) {
            return cst;
        }

        /* Zero matches: VMS prints %DIRECT-W-NOFILES with NO header — not an
         * empty "Total of 0 files." trailer.
         * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY
         * — a search that finds nothing yields
         * "%DIRECT-W-NOFILES, no files found". */
        if (entry_count == 0) {
            free(entries);
            dcl_error("DIRECT", STS$K_WARNING, "NOFILES", "no files found");
            return SS$_NOSUCHFILE;
        }

        /* Header (only once we know there is at least one file to list).
         * Derive the "DEV:[DIR]" from the VMS-side inputs (the DIRECTORY
         * argument, else the process default) rather than reverse-deriving it
         * from the host path — the host round-trip cannot recover the volume
         * root [000000] from the device-root mount and dropped the real device
         * (vms-272). */
        dcl_directory_header_spec(ctx->default_dir, use_spec,
                                  vms_dir, sizeof(vms_dir));
        if (show_heading) printf("\nDirectory %s\n\n", vms_dir);

        int file_count = 0;
        long total_blocks = 0, total_alloc = 0;
        dir_print_entries(entries, entry_count, &opts,
                          &file_count, &total_blocks, &total_alloc);
        free(entries);

        /* Footer. Block counts appear only when file sizes are displayed
         * (/SIZE or /FULL) — see dcl_print_dir_total()'s citation. */
        if (show_grand_total) {
            printf("\nGrand total of 1 directory, %d file%s",
                   file_count, file_count != 1 ? "s" : "");
            if (show_full || (show_size && size_mode == DIR_SIZE_ALL))
                printf(", %ld/%ld block%s", total_blocks, total_alloc,
                       total_blocks != 1 ? "s" : "");
            else if (show_size && size_mode == DIR_SIZE_ALLOC)
                printf(", %ld block%s", total_alloc,
                       total_alloc != 1 ? "s" : "");
            else if (show_size)
                printf(", %ld block%s", total_blocks,
                       total_blocks != 1 ? "s" : "");
            printf(".\n");
        } else {
            dcl_print_dir_total(file_count, total_blocks, total_alloc,
                                show_size, show_full, size_mode);
        }

        /* /TRAILING: repeat the totals with the directory spec appended. */
        if (show_trailing) {
            dcl_print_dir_total(file_count, total_blocks, total_alloc,
                                show_size, show_full, size_mode);
            printf("%s\n", vms_dir);
        }

        return SS$_NORMAL;
    }

    /* ---------------- Multi-directory ellipsis listing ----------------
     * The "..." wildcard names the start directory plus every subdirectory
     * below it. Walk the genuine ODS-2 directory tree through the ACP
     * (dir_gather_acp: "*.DIR" search per level), list each directory that has
     * at least one match with its own header + "Total of N files" subtotal,
     * then emit a single "Grand total of D directories, F files[, M blocks]."
     * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY —
     * ellipsis directory wildcard + the per-directory / Grand total layout. */
    char ell_base[1024];
    if (vms_base[0])
        strncpy(ell_base, vms_base, sizeof(ell_base) - 1);
    else
        dcl_rms_effective_spec(ctx, "", ell_base, sizeof(ell_base));  /* default dir */
    ell_base[sizeof(ell_base) - 1] = '\0';

    char **dirs = NULL;
    int ndirs = 0, dcap = 0;
    int exec_absent = rms_executive_absent();   /* vms-5f0 host defer */
    if (exec_absent)
        dir_gather_host(ctx, ell_base, &dirs, &ndirs, &dcap, 0);
    else
        dir_gather_acp(ctx, ell_base, &dirs, &ndirs, &dcap, 0);

    long grand_used = 0, grand_alloc = 0, grand_files = 0;
    int  grand_dirs = 0;

    for (int di = 0; di < ndirs; di++) {
        struct dir_entry *entries = NULL;
        int entry_count = 0;
        char dpat[1200];
        snprintf(dpat, sizeof(dpat), "%s%s", dirs[di], vms_filepat);
        int cst;
        if (exec_absent)
            cst = dir_collect_host(ctx, dirs[di], vms_filepat, excl_pats,
                                   excl_count, &entries, &entry_count);
        else
            cst = dir_collect_acp(ctx, dpat, excl_pats, excl_count,
                                  &entries, &entry_count);
        if (cst != SS$_NORMAL) { free(entries); continue; }
        if (entry_count == 0) { free(entries); continue; }

        strncpy(vms_dir, dirs[di], sizeof(vms_dir) - 1);
        vms_dir[sizeof(vms_dir) - 1] = '\0';

        int file_count = 0;
        long used = 0, alloc = 0;
        if (show_grand_total) {
            /* /GRAND_TOTAL: suppress every directory's header, files and
             * subtotal — only the final grand total is printed. */
            struct dir_opts sopts = opts;
            sopts.suppress_files = 1;
            dir_print_entries(entries, entry_count, &sopts,
                              &file_count, &used, &alloc);
        } else {
            if (show_heading) printf("\nDirectory %s\n\n", vms_dir);
            dir_print_entries(entries, entry_count, &opts,
                              &file_count, &used, &alloc);
            dcl_print_dir_total(file_count, used, alloc, show_size, show_full,
                                size_mode);
        }
        free(entries);

        grand_files += file_count;
        grand_used  += used;
        grand_alloc += alloc;
        grand_dirs++;
    }

    for (int di = 0; di < ndirs; di++) free(dirs[di]);
    free(dirs);

    if (grand_files == 0) {
        dcl_error("DIRECT", STS$K_WARNING, "NOFILES", "no files found");
        return SS$_NOSUCHFILE;
    }

    printf("\nGrand total of %d director%s, %ld file%s",
           grand_dirs, grand_dirs == 1 ? "y" : "ies",
           grand_files, grand_files != 1 ? "s" : "");
    if (show_full || (show_size && size_mode == DIR_SIZE_ALL))
        printf(", %ld/%ld block%s", grand_used, grand_alloc,
               grand_used != 1 ? "s" : "");
    else if (show_size && size_mode == DIR_SIZE_ALLOC)
        printf(", %ld block%s", grand_alloc, grand_alloc != 1 ? "s" : "");
    else if (show_size)
        printf(", %ld block%s", grand_used, grand_used != 1 ? "s" : "");
    printf(".\n");

    return SS$_NORMAL;
}

/*
 * TYPE - Display file contents.
 */
int cmd_type(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* vms-481: TYPE reads the file through RMS ($OPEN + sequential $GET), which
     * on the product runtime reaches the Files-11 ODS-2 ACP over /dev/vms -- not
     * fopen() on a vmsfs_to_linux_path("/vms/...") path. Fail-honest: no
     * ACP-mounted SYS$DISK => RMS$_FNF/ACC, never a silent POSIX read (INV-6). */
    uint32_t rst = 0;
    struct dcl_rms_reader *r = dcl_rms_read_open(ctx, cmd->params[0], &rst);
    if (!r) {
        /* -------- Legacy TYPE defer (vms-5f0, extended vms-b3e) --------
         * RMS could not open it through the Files-11 ODS-2 ACP. Two honest
         * fall-backs, BOTH reading only kernel mount truth, NEVER the /vms
         * SYS$DISK passthrough the atomic flip forbids (INV-6):
         *
         *  (a) rms_executive_absent(): /dev/vms is unreachable (host ctest,
         *      plain-container self-host/link gates), which also covers a
         *      DEFINEd logical resolving OUTSIDE the SYSDISK root (the mkdtemp-
         *      volume test convention) that RMS's $OPEN boundary refuses even
         *      though the file is right there. Read it the pre-flip way.
         *
         *  (b) vmsfs_device_spec_kernel_mounted(): the executive IS present but
         *      the named unit is a GENUINE cross-process vmsfs volume in
         *      /proc/mounts (a raw `mount -t vmsfs`, e.g. the install target
         *      vms-718 MOUNTs then RUNs AUTHORIZE against, or a loop-mounted
         *      DKA100:) that the ACP does not own. A mounted unit is node-wide
         *      executive state on VMS: every process must resolve it (vms-8b6 /
         *      vms-b3e). The ACP only manages ODS-2 volumes IT mounted, so read
         *      this one through its /mnt/<dev> mount. The gate matches ONLY a
         *      real /proc/mounts entry -- the system disk (DKA0: -> ACP, or the
         *      /vms passthrough) is never a /mnt/<dev> mount, so this can never
         *      reopen the masquerade; an unmounted unit yields no fall-back and
         *      the honest RMS$_FNF stands.
         *
         * In both cases dcl_resolve_path() honours the logical and an explicit
         * ";N"; RMS stays the primary path so a genuine record-formatted /vms
         * file is still decoded by $GET. */
        if (rms_executive_absent() ||
            vmsfs_device_spec_kernel_mounted(cmd->params[0])) {
            char linux_path[1024];
            dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));
            FILE *fp = fopen(linux_path, "r");
            if (fp) {
                int paged = dcl_has_qualifier(cmd, "PAGE");
                int line_count = 0, page_size = 24;
                char line[4096];
                while (fgets(line, sizeof(line), fp)) {
                    fputs(line, stdout);
                    line_count++;
                    if (paged && line_count >= page_size) {
                        printf("Press RETURN to continue...");
                        fflush(stdout);
                        char buf[64];
                        if (!fgets(buf, sizeof(buf), stdin)) break;
                        line_count = 0;
                    }
                }
                fclose(fp);
                return SS$_NORMAL;
            }
        }
        dcl_error("RMS", 2, "FNF",
                  "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Check for /PAGE qualifier */
    int paged = dcl_has_qualifier(cmd, "PAGE");
    int line_count = 0;
    int page_size = 24;

    char line[4096];
    int eof = 0, len;
    while ((len = dcl_rms_read_record(r, line, sizeof(line), &eof)) >= 0) {
        fwrite(line, 1, (size_t)len, stdout);
        fputc('\n', stdout);           /* RMS records carry no terminator */
        line_count++;

        if (paged && line_count >= page_size) {
            printf("Press RETURN to continue...");
            fflush(stdout);
            char buf[64];
            if (!fgets(buf, sizeof(buf), stdin)) break;
            line_count = 0;
        }
    }

    dcl_rms_read_close(r);
    return SS$_NORMAL;
}

/* ===================================================================
 * COPY / DELETE / RENAME — VMS wildcard + version file operations.
 *
 * All three route filename matching through the single VMS wildcard
 * matcher vmsfs_wildcard_match() (%, *, version-aware, shared with
 * DIRECTORY/PURGE) and resolve versions against the real on-disk vmsfs
 * version store (name.type;N files / vmsfs_get_highest_version()).
 * INV-6: every file copied, renamed or deleted exists on disk; the
 * per-command counts are the count of real filesystem operations, never
 * a fabricated total.
 *
 * Grounded (clean-room, Rule 8) in the public VSI OpenVMS DCL Dictionary
 * COPY, DELETE and RENAME entries — the specific rule is cited at each
 * command.
 * =================================================================== */

/* One on-disk file matched by a wildcard/version file operation. */
struct file_match {
    char dname[256];   /* exact on-disk name, e.g. "FOO.TXT;3"       */
    char base[256];    /* name.type portion, UPPERCASED for grouping */
    int  version;      /* version number; an unversioned file => 1   */
};

/* Split "name.type;ver" (as it sits on disk) into an UPPERCASED base plus a
 * numeric version. An unversioned entry is version 1 — VMS treats a plain file
 * as ;1 (matches dir_collect() and vmsfs_get_highest_version()). */
static void fm_split(const char *dname, char *base, size_t base_sz, int *ver)
{
    const char *semi = strrchr(dname, ';');
    size_t blen = semi ? (size_t)(semi - dname) : strlen(dname);
    if (blen >= base_sz) blen = base_sz - 1;
    size_t i;
    for (i = 0; i < blen; i++)
        base[i] = (char)toupper((unsigned char)dname[i]);
    base[i] = '\0';
    if (semi && semi[1]) {
        int alld = 1;
        for (const char *q = semi + 1; *q; q++)
            if (!isdigit((unsigned char)*q)) { alld = 0; break; }
        *ver = alld ? (int)strtol(semi + 1, NULL, 10) : 1;
    } else {
        *ver = 1;
    }
}

/* Split "NAME.TYPE" into name + type (type empty when there is no dot). */
static void split_name_type(const char *base, char *name, size_t nsz,
                            char *type, size_t tsz)
{
    const char *dot = strrchr(base, '.');
    if (dot) {
        size_t nl = (size_t)(dot - base);
        if (nl >= nsz) nl = nsz - 1;
        memcpy(name, base, nl);
        name[nl] = '\0';
        strncpy(type, dot + 1, tsz - 1);
        type[tsz - 1] = '\0';
    } else {
        strncpy(name, base, nsz - 1);
        name[nsz - 1] = '\0';
        type[0] = '\0';
    }
}

/*
 * split_file_spec - Resolve a VMS file parameter to its Linux directory (with
 * trailing slash), its name.type portion (VMS wildcards preserved), and its
 * version-spec text (the characters after ';'; *has_version is set if a ';'
 * was present at all).
 *
 * The name+version text is taken verbatim from what the user typed
 * (dcl_filename_component()), NOT from a version-collapsed resolved path — see
 * dcl_filename_component()'s doc comment: a bare name must retain its "all
 * versions" meaning, and the explicit-version guard must see the ';' the user
 * actually typed.
 */
static void split_file_spec(struct dcl_context *ctx, const char *param,
                            char *linux_dir, size_t dir_sz,
                            char *name_pat, size_t name_sz,
                            char *vspec, size_t vspec_sz, int *has_version)
{
    /* Resolve the DIRECTORY from a version-stripped copy of the spec. A version
     * marker (;n, ;*, ;) defeats dcl_resolve_path()'s case-insensitive file
     * lookup, which is what fixes the on-disk directory case when a VMS
     * directory maps to a differently-cased Linux path; the bare-name form
     * resolves the same directory the way COPY/RENAME already do successfully.
     * The name.type + version fields are still taken verbatim from `param`
     * below. */
    char probe[1024];
    strncpy(probe, param, sizeof(probe) - 1);
    probe[sizeof(probe) - 1] = '\0';
    {
        const char *pc = dcl_filename_component(param);
        if (pc && pc[0]) {
            const char *psemi = strrchr(pc, ';');
            if (psemi) {
                size_t off = (size_t)(psemi - param);
                if (off < sizeof(probe)) probe[off] = '\0';
            }
        }
    }

    char resolved[1024];
    dcl_resolve_path(ctx, probe, resolved, sizeof(resolved));

    char *slash = strrchr(resolved, '/');
    if (slash) {
        size_t dl = (size_t)(slash - resolved) + 1;
        if (dl >= dir_sz) dl = dir_sz - 1;
        memcpy(linux_dir, resolved, dl);
        linux_dir[dl] = '\0';
    } else {
        vmsfs_to_linux_path(ctx->default_dir, linux_dir, dir_sz);
        size_t dl = strlen(linux_dir);
        if (dl && linux_dir[dl - 1] != '/' && dl < dir_sz - 1) {
            linux_dir[dl] = '/';
            linux_dir[dl + 1] = '\0';
        }
    }

    const char *comp = dcl_filename_component(param);
    if (!comp || !comp[0]) comp = slash ? slash + 1 : resolved;

    const char *semi = strrchr(comp, ';');
    if (semi) {
        *has_version = 1;
        size_t nl = (size_t)(semi - comp);
        if (nl >= name_sz) nl = name_sz - 1;
        memcpy(name_pat, comp, nl);
        name_pat[nl] = '\0';
        strncpy(vspec, semi + 1, vspec_sz - 1);
        vspec[vspec_sz - 1] = '\0';
    } else {
        *has_version = 0;
        strncpy(name_pat, comp, name_sz - 1);
        name_pat[name_sz - 1] = '\0';
        vspec[0] = '\0';
    }
}

/*
 * fm_collect - Enumerate the regular files in linux_dir whose name.type matches
 * name_pat through the single VMS wildcard matcher (vmsfs_wildcard_match; %, *,
 * version-agnostic when the pattern carries no ';'). Real opendir()/stat() walk
 * (INV-6). Returns a malloc'd array (caller frees) + count.
 */
static int fm_collect(const char *linux_dir, const char *name_pat,
                      struct file_match **out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    DIR *d = opendir(linux_dir);
    if (!d) return SS$_NOSUCHFILE;

    int cap = 32, n = 0;
    struct file_match *arr = malloc((size_t)cap * sizeof(*arr));
    if (!arr) { closedir(d); return SS$_INSFMEM; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        char full[2048];
        snprintf(full, sizeof(full), "%s%s", linux_dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        if (!vmsfs_wildcard_match(name_pat, de->d_name)) continue;

        if (n >= cap) {
            cap *= 2;
            struct file_match *t = realloc(arr, (size_t)cap * sizeof(*arr));
            if (!t) { free(arr); closedir(d); return SS$_INSFMEM; }
            arr = t;
        }
        strncpy(arr[n].dname, de->d_name, sizeof(arr[n].dname) - 1);
        arr[n].dname[sizeof(arr[n].dname) - 1] = '\0';
        fm_split(de->d_name, arr[n].base, sizeof(arr[n].base), &arr[n].version);
        n++;
    }
    closedir(d);

    *out = arr;
    *out_count = n;
    return SS$_NORMAL;
}

/* Highest version among the matches that share `base` (case-insensitive). */
static int fm_highest_for(const struct file_match *m, int count, const char *base)
{
    int hi = 0;
    for (int i = 0; i < count; i++)
        if (strcasecmp(m[i].base, base) == 0 && m[i].version > hi)
            hi = m[i].version;
    return hi;
}

/*
 * version_selected - Is match `idx` in the version set the user named?
 *
 * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary version-number
 * conventions in a file specification:
 *   no version marker  -> the highest existing version (a bare name references
 *                         the latest version);
 *   ";*"               -> every version;
 *   ";" or ";0"        -> the highest existing version;
 *   ";-n"              -> the version n below the highest;
 *   ";n"  (n>0)        -> exactly version n.
 */
static int version_selected(const struct file_match *m, int idx, int count,
                            int has_version, const char *vspec)
{
    if (!has_version)
        return m[idx].version == fm_highest_for(m, count, m[idx].base);
    if (vspec[0] == '*') return 1;
    if (vspec[0] == '\0' || strcmp(vspec, "0") == 0)
        return m[idx].version == fm_highest_for(m, count, m[idx].base);
    if (vspec[0] == '-') {
        int rel = (int)strtol(vspec, NULL, 10);      /* negative */
        return m[idx].version == fm_highest_for(m, count, m[idx].base) + rel;
    }
    return m[idx].version == (int)strtol(vspec, NULL, 10);
}

/* Map an output name/type field: a lone "*" (or empty) copies the input field;
 * anything else is taken literally. This is the common VMS field-substitution
 * case (COPY *.TXT *.LIS keeps each name, swaps the type). */
static void map_out_field(const char *out_field, const char *in_field,
                          char *dst, size_t sz)
{
    if (out_field[0] == '\0' || strcmp(out_field, "*") == 0)
        strncpy(dst, in_field, sz - 1);
    else
        strncpy(dst, out_field, sz - 1);
    dst[sz - 1] = '\0';
}

/*
 * copy_one_rms - vms-481: copy src_spec to dst_spec through RMS, record by
 * record. Source is read via $OPEN + sequential $GET; the destination is minted
 * via $CREATE (which, on the product runtime, allocates a real FID from
 * INDEXF.SYS and enters a new highest version through the Files-11 ODS-2 ACP)
 * and written via sequential $PUT. The destination inherits the source's record
 * format/attributes (rfm/rat/mrs) from its ODS-2 header (rms_file_attr) so a
 * copied file keeps its record structure -- not a raw byte stream. No fopen on a
 * /vms passthrough; fail-honest on an absent ACP (INV-6).
 *
 * ctx is used only for device/directory defaulting; src_spec and dst_spec are
 * VMS filespecs. Returns SS$_NORMAL, or a VMS error.
 */
static int copy_one_rms(struct dcl_context *ctx,
                        const char *src_spec, const char *dst_spec)
{
    struct rms_fileattr sattr;
    uint8_t rfm = FAB$C_VAR, rat = 0;
    uint16_t mrs = 0;

    /* Inherit the source record format where we can read it (genuine ODS-2
     * header via the ACP); default to variable-length text otherwise. */
    if (dcl_rms_attr(ctx, src_spec, &sattr) == RMS$_NORMAL && sattr.rfm) {
        rfm = sattr.rfm;
        rat = (uint8_t)sattr.rat;
        mrs = sattr.mrs;
    }

    uint32_t rst = 0;
    struct dcl_rms_reader *r = dcl_rms_read_open(ctx, src_spec, &rst);
    if (!r) return SS$_NOSUCHFILE;

    struct dcl_rms_writer *w = dcl_rms_write_create(ctx, dst_spec, rfm, rat, mrs, &rst);
    if (!w) { dcl_rms_read_close(r); return SS$_FILACCERR; }

    static char rec[65536];
    int eof = 0, len, rc = SS$_NORMAL;
    while ((len = dcl_rms_read_record(r, rec, sizeof(rec), &eof)) >= 0) {
        if (dcl_rms_write_record(w, rec, (size_t)len) != 0) { rc = SS$_ABORT; break; }
    }
    if (!eof && rc == SS$_NORMAL && len < 0) rc = SS$_ABORT;   /* hard read error */

    dcl_rms_read_close(r);
    dcl_rms_write_close(w);
    return rc;
}

/*
 * copy_one_host - vms-5f0 executive-absent byte copy between two RESOLVED Linux
 * paths. The COPY host defer (see cmd_copy) resolves source and destination via
 * dcl_resolve_path() -- the same legacy resolver DELETE/RENAME use, which honours
 * a DEFINEd logical to any directory (the test convention of a mkdtemp volume) --
 * and copies the bytes directly, never re-parsing a VMS spec through RMS (whose
 * SYSDISK-root boundary would reject an out-of-/vms logical target). Runs ONLY
 * when the executive is absent; with /dev/vms present COPY goes through the ACP.
 */
static int copy_one_host(const char *src_full, const char *dst_full)
{
    FILE *in = fopen(src_full, "rb");
    if (!in) return SS$_NOSUCHFILE;
    FILE *out = fopen(dst_full, "wb");
    if (!out) { fclose(in); return SS$_FILACCERR; }

    char buf[65536];
    size_t n;
    int rc = SS$_NORMAL;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = SS$_ABORT; break; }
    }
    if (ferror(in)) rc = SS$_ABORT;
    fclose(in);
    if (fclose(out) != 0 && rc == SS$_NORMAL) rc = SS$_ABORT;
    return rc;
}

/*
 * resolve_out_version - Decide the destination version number for a COPY/RENAME
 * output file, and whether the write must be refused as a collision.
 *
 * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, COPY / RENAME
 * output version-number defaulting and /NEW_VERSION:
 *   - No explicit output version: if a file of the same name and type already
 *     exists in the target directory, the output version is one greater than
 *     the highest existing version; otherwise the output keeps the (user-typed)
 *     name with no synthesized version (OVMX writes it unversioned — the same
 *     never-overwrite result VMS gives, without perturbing files that had no
 *     version before). This is the "never silently overwrite" rule.
 *   - Explicit output version ;n that does NOT already exist: use ;n.
 *   - Explicit output version ;n that DOES exist: an error unless /NEW_VERSION,
 *     which bumps to highest+1.
 *
 * *out_ver is set to the version to write (0 == write unversioned name.type).
 * Returns 1 to proceed, 0 to refuse (collision without /NEW_VERSION).
 */
static int resolve_out_version(const char *dst_dir, const char *out_name,
                               const char *out_type, int dst_has_version,
                               const char *dst_vspec, int new_version,
                               int *out_ver)
{
    const char *type = out_type[0] ? out_type : NULL;
    int highest = vmsfs_get_highest_version(dst_dir, out_name, type);

    /* Explicit numeric output version (;n, n>0). */
    if (dst_has_version && dst_vspec[0] && isdigit((unsigned char)dst_vspec[0])) {
        int want = (int)strtol(dst_vspec, NULL, 10);
        if (want > 0) {
            int exists =
                (vmsfs_resolve_version(dst_dir, out_name, type, want) == want);
            if (exists && !new_version) { *out_ver = want; return 0; }
            *out_ver = (exists && new_version) ? highest + 1 : want;
            return 1;
        }
    }

    /* Default / ;0 / ; / ;* : one above the highest, else unversioned. */
    *out_ver = (highest > 0) ? highest + 1 : 0;
    return 1;
}

/*
 * COPY - Copy file(s), with VMS wildcard source expansion and version
 * defaulting on the output.
 *
 * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, COPY —
 *   - a wildcard input spec copies each matching file;
 *   - the output version defaults to one greater than the highest existing
 *     version of the same name.type (never a silent overwrite); /NEW_VERSION
 *     forces a higher version when an explicit output version collides;
 *   - /LOG lists each file with "%COPY-S-COPIED, in copied to out", and a
 *     multi-file copy reports "%COPY-S-NEWFILES, n files created".
 */
int cmd_copy(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE", "missing source and/or destination");
        return SS$_BADPARAM;
    }

    int do_log      = dcl_has_qualifier(cmd, "LOG");
    int do_confirm  = dcl_has_qualifier(cmd, "CONFIRM");
    int new_version = dcl_has_qualifier(cmd, "NEW_VERSION");

    (void)new_version;   /* RMS $CREATE mints a new highest version by default */

    /* -------- Executive-absent COPY defer (vms-5f0) --------
     * With no /dev/vms, COPY cannot route through the ACP; and the RMS $OPEN/
     * $CREATE path enforces the SYSDISK-root boundary, which rejects a DEFINEd
     * logical that resolves outside /vms (the test convention of a mkdtemp
     * volume). DELETE and RENAME already avoid this by resolving through
     * dcl_resolve_path() + fm_collect() and operating on the on-disk files
     * directly; COPY mirrors them here -- same source enumeration, the shared
     * resolve_out_version() version defaulting, and a direct byte copy. With
     * /dev/vms present the ACP path below runs unchanged (INV-6). */
    if (rms_executive_absent()) {
        char src_dir[1024], src_pat[512], src_vspec[64];
        int  src_hasver;
        split_file_spec(ctx, cmd->params[0], src_dir, sizeof(src_dir),
                        src_pat, sizeof(src_pat),
                        src_vspec, sizeof(src_vspec), &src_hasver);

        /* Destination: an existing directory keeps each source name/type
         * (fields "*"); otherwise parse the destination name.type;ver fields. */
        char hdst_dir[1024];
        char hdname_pat[256] = "*", hdtype_pat[256] = "*", hdst_vspec[64] = "";
        int  hdst_hasver = 0;

        char hdst_resolved[1024];
        dcl_resolve_path(ctx, cmd->params[1], hdst_resolved, sizeof(hdst_resolved));
        struct stat hdst_st;
        int hdst_is_dir = (stat(hdst_resolved, &hdst_st) == 0 &&
                           S_ISDIR(hdst_st.st_mode));
        if (hdst_is_dir) {
            size_t dl = strlen(hdst_resolved);
            strncpy(hdst_dir, hdst_resolved, sizeof(hdst_dir) - 1);
            hdst_dir[sizeof(hdst_dir) - 1] = '\0';
            if (dl && hdst_dir[dl - 1] != '/' && dl < sizeof(hdst_dir) - 1) {
                hdst_dir[dl] = '/';
                hdst_dir[dl + 1] = '\0';
            }
        } else {
            char dnamepat[256];
            split_file_spec(ctx, cmd->params[1], hdst_dir, sizeof(hdst_dir),
                            dnamepat, sizeof(dnamepat),
                            hdst_vspec, sizeof(hdst_vspec), &hdst_hasver);
            split_name_type(dnamepat, hdname_pat, sizeof(hdname_pat),
                            hdtype_pat, sizeof(hdtype_pat));
        }

        struct file_match *m = NULL;
        int count = 0;
        int st = fm_collect(src_dir, src_pat, &m, &count);
        if (st != SS$_NORMAL) {
            free(m);
            dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }

        int copied = 0, matched = 0;
        for (int i = 0; i < count; i++) {
            if (!version_selected(m, i, count, src_hasver, src_vspec)) continue;
            matched++;

            char in_name[256], in_type[256];
            split_name_type(m[i].base, in_name, sizeof(in_name),
                            in_type, sizeof(in_type));
            char out_name[256], out_type[256];
            map_out_field(hdname_pat, in_name, out_name, sizeof(out_name));
            map_out_field(hdtype_pat, in_type, out_type, sizeof(out_type));

            int out_ver = 0;
            if (!resolve_out_version(hdst_dir, out_name, out_type,
                                     hdst_hasver, hdst_vspec, new_version,
                                     &out_ver)) {
                dcl_error("RMS", 2, "FEX",
                          "file already exists, not superseded - %s.%s;%d",
                          out_name, out_type, out_ver);
                continue;
            }

            char dfile[600];
            if (out_ver > 0) {
                if (out_type[0])
                    snprintf(dfile, sizeof(dfile), "%s.%s;%d",
                             out_name, out_type, out_ver);
                else
                    snprintf(dfile, sizeof(dfile), "%s;%d", out_name, out_ver);
            } else if (out_type[0]) {
                snprintf(dfile, sizeof(dfile), "%s.%s", out_name, out_type);
            } else {
                snprintf(dfile, sizeof(dfile), "%s", out_name);
            }

            char src_full[2600], dst_full[2600];
            snprintf(src_full, sizeof(src_full), "%s%s", src_dir, m[i].dname);
            snprintf(dst_full, sizeof(dst_full), "%s%s", hdst_dir, dfile);

            if (do_confirm) {
                char vsrc[256], vdst[256];
                dcl_format_filespec(src_full, vsrc, sizeof(vsrc));
                dcl_format_filespec(dst_full, vdst, sizeof(vdst));
                printf("COPY %s to %s ? [N]: ", vsrc, vdst);
                fflush(stdout);
                char resp[64];
                if (!fgets(resp, sizeof(resp), stdin)) break;
                if (toupper((unsigned char)resp[0]) != 'Y') continue;
            }

            int cst = copy_one_host(src_full, dst_full);
            if (cst == SS$_NOSUCHFILE) {
                dcl_error("RMS", 2, "FNF", "file not found - %s", src_full);
                continue;
            }
            if (cst != SS$_NORMAL) {
                dcl_error("RMS", 2, "WER", "write error - %s", dst_full);
                continue;
            }

            copied++;
            if (do_log) {
                char vsrc[256], vdst[256];
                dcl_format_filespec(src_full, vsrc, sizeof(vsrc));
                dcl_format_filespec(dst_full, vdst, sizeof(vdst));
                dcl_error("COPY", 1, "COPIED", "%s copied to %s", vsrc, vdst);
            }
        }
        free(m);

        if (matched == 0) {
            dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
            return SS$_NOSUCHFILE;
        }
        if (copied == 0)
            return SS$_ABORT;
        if (copied > 1)
            dcl_error("COPY", 1, "NEWFILES", "%d files created", copied);
        return SS$_NORMAL;
    }

    /*
     * vms-481: COPY reaches files through RMS/$QIO-ACP. The source is enumerated
     * by the executive wildcard search (sys$parse + sys$search over the ODS-2
     * ACP -- genuine directory order, real FIDs), each file is copied record by
     * record via RMS $GET/$PUT, and the destination is minted by $CREATE, which
     * allocates a real FID and enters a NEW HIGHEST VERSION through the ACP (the
     * VMS never-overwrite guarantee, now the executive's, not an opendir scan).
     * No opendir/fopen on a /vms passthrough; fail-honest on an absent ACP.
     */

    /* Effective destination VMS spec. A destination that ends with ']' / '>' /
     * ':' (a directory or device, no filename) receives each source's own
     * name.type; otherwise it is an explicit output filespec (wildcards mapped
     * from the source name/type). */
    char dst_eff[1024];
    dcl_rms_effective_spec(ctx, cmd->params[1], dst_eff, sizeof(dst_eff));
    size_t del = strlen(dst_eff);
    int dst_is_dir = (del > 0 && (dst_eff[del - 1] == ']' ||
                                  dst_eff[del - 1] == '>' ||
                                  dst_eff[del - 1] == ':'));

    /* For an explicit output filespec, split it into a directory prefix and the
     * output name/type pattern (which may carry "*"/"%" mapped from the source). */
    char dst_prefix[1024] = "", dname_pat[256] = "*", dtype_pat[256] = "*";
    if (!dst_is_dir) {
        const char *nt = dst_eff;
        const char *rb = strrchr(dst_eff, ']');
        if (!rb) rb = strrchr(dst_eff, '>');
        if (!rb) { const char *cn = strrchr(dst_eff, ':'); if (cn) rb = cn; }
        if (rb) {
            size_t pl = (size_t)(rb - dst_eff) + 1;
            if (pl >= sizeof(dst_prefix)) pl = sizeof(dst_prefix) - 1;
            memcpy(dst_prefix, dst_eff, pl); dst_prefix[pl] = '\0';
            nt = rb + 1;
        }
        char ntbuf[300];
        strncpy(ntbuf, nt, sizeof(ntbuf) - 1); ntbuf[sizeof(ntbuf) - 1] = '\0';
        char *semi = strchr(ntbuf, ';'); if (semi) *semi = '\0';
        split_name_type(ntbuf, dname_pat, sizeof(dname_pat),
                        dtype_pat, sizeof(dtype_pat));
    }

    struct dcl_rms_dir *d = dcl_rms_dir_open(ctx, cmd->params[0]);
    if (!d) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    int copied = 0, matched = 0;
    char src_spec[1024];
    while (dcl_rms_dir_next(d, src_spec, sizeof(src_spec), NULL, NULL, NULL)) {
        matched++;

        /* Source name.type (strip device/dir and version). */
        const char *snt = src_spec;
        const char *srb = strrchr(src_spec, ']');
        if (!srb) srb = strrchr(src_spec, '>');
        if (srb) snt = srb + 1;
        char in_nt[300];
        strncpy(in_nt, snt, sizeof(in_nt) - 1); in_nt[sizeof(in_nt) - 1] = '\0';
        char *isemi = strchr(in_nt, ';'); if (isemi) *isemi = '\0';

        char in_name[256], in_type[256];
        split_name_type(in_nt, in_name, sizeof(in_name),
                        in_type, sizeof(in_type));

        /* Destination filespec for this source. */
        char dst_spec[1200];
        if (dst_is_dir) {
            snprintf(dst_spec, sizeof(dst_spec), "%s%s", dst_eff, in_nt);
        } else {
            char out_name[256], out_type[256];
            map_out_field(dname_pat, in_name, out_name, sizeof(out_name));
            map_out_field(dtype_pat, in_type, out_type, sizeof(out_type));
            if (out_type[0])
                snprintf(dst_spec, sizeof(dst_spec), "%s%s.%s",
                         dst_prefix, out_name, out_type);
            else
                snprintf(dst_spec, sizeof(dst_spec), "%s%s", dst_prefix, out_name);
        }

        if (do_confirm) {
            printf("COPY %s to %s ? [N]: ", src_spec, dst_spec);
            fflush(stdout);
            char resp[64];
            if (!fgets(resp, sizeof(resp), stdin)) break;
            if (toupper((unsigned char)resp[0]) != 'Y') continue;
        }

        int cst = copy_one_rms(ctx, src_spec, dst_spec);
        if (cst == SS$_NOSUCHFILE) {
            dcl_error("RMS", 2, "FNF", "file not found - %s", src_spec);
            continue;
        }
        if (cst == SS$_FILACCERR) {
            dcl_error("RMS", 2, "CRE", "cannot create - %s", dst_spec);
            continue;
        }
        if (cst == SS$_ABORT) {
            dcl_error("RMS", 2, "WER", "write error - %s", dst_spec);
            continue;
        }

        copied++;
        if (do_log)
            dcl_error("COPY", 1, "COPIED", "%s copied to %s", src_spec, dst_spec);
    }
    dcl_rms_dir_close(d);

    if (matched == 0) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }
    if (copied == 0)
        return SS$_ABORT;

    /* A multi-file copy reports the aggregate; a single copy is silent unless
     * /LOG asked for the per-file COPIED line above. */
    if (copied > 1)
        dcl_error("COPY", 1, "NEWFILES", "%d files created", copied);

    return SS$_NORMAL;
}

/*
 * DELETE - Delete a file, or (with /SYMBOL) delete a symbol.
 */
/*
 * delete_files_acp - vms-0a5: the DELETE file path over the Files-11 ODS-2 ACP
 * (the product runtime, Rule 9/INV-6). It enumerates every version of the named
 * file through the executive directory search ($PARSE/$SEARCH), applies the SAME
 * version selection the POSIX path uses (version_selected), and $ERASEs each
 * selected file over the ACP (IO$_DELETE, dcl_rms_erase) -- no unlink() on a
 * /vms passthrough. This is what makes DELETE SYS$SYSTEM:PROVISION.EXE;*
 * actually remove the image off the mounted ODS-2 volume -- the volume PID 1
 * stages the boot images from (vms-5f0) -- rather than the passthrough copy the
 * booted system never reads. `param` is the user's VMS filespec; the caller has
 * already enforced the explicit-version guard.
 */
static int delete_files_acp(struct dcl_context *ctx, const char *param,
                            int do_confirm, int do_log)
{
    /* Version spec the user typed (guaranteed present by the DELVER guard). */
    char vspec[64];
    int  has_version = 0;
    {
        const char *comp = dcl_filename_component(param);
        const char *fcheck = (comp && comp[0]) ? comp : param;
        const char *semi = strrchr(fcheck, ';');
        if (semi) {
            has_version = 1;
            strncpy(vspec, semi + 1, sizeof(vspec) - 1);
            vspec[sizeof(vspec) - 1] = '\0';
        } else {
            vspec[0] = '\0';
        }
    }

    /* Search pattern: the user's spec with the version replaced by ";*" so the
     * executive returns ALL versions and version_selected can pick the set. */
    char searchpat[1024];
    strncpy(searchpat, param, sizeof(searchpat) - 1);
    searchpat[sizeof(searchpat) - 1] = '\0';
    {
        const char *comp = dcl_filename_component(param);
        if (comp && comp[0]) {
            const char *semi = strrchr(comp, ';');
            if (semi) {
                size_t off = (size_t)(semi - param);
                if (off < sizeof(searchpat)) searchpat[off] = '\0';
            }
        }
        size_t L = strlen(searchpat);
        snprintf(searchpat + L, sizeof(searchpat) - L, ";*");
    }

    struct dcl_rms_dir *d = dcl_rms_dir_open(ctx, searchpat);
    if (!d) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", param);
        return SS$_NOSUCHFILE;
    }

    struct acp_del { char spec[1024]; struct file_match fm; };
    int cap = 32, n = 0;
    struct acp_del *arr = malloc((size_t)cap * sizeof(*arr));
    if (!arr) { dcl_rms_dir_close(d); return SS$_INSFMEM; }

    char rspec[1024];
    while (dcl_rms_dir_next(d, rspec, sizeof(rspec), NULL, NULL, NULL)) {
        if (n >= cap) {
            cap *= 2;
            struct acp_del *t = realloc(arr, (size_t)cap * sizeof(*arr));
            if (!t) { free(arr); dcl_rms_dir_close(d); return SS$_INSFMEM; }
            arr = t;
        }
        /* the full resultant "DEV:[DIR]NAME.TYP;VER" is the $ERASE target. */
        strncpy(arr[n].spec, rspec, sizeof(arr[n].spec) - 1);
        arr[n].spec[sizeof(arr[n].spec) - 1] = '\0';
        /* name.type;ver tail for grouping + version selection. */
        const char *nt = rspec;
        const char *rb = strrchr(rspec, ']');
        if (!rb) rb = strrchr(rspec, '>');
        if (rb) nt = rb + 1;
        strncpy(arr[n].fm.dname, nt, sizeof(arr[n].fm.dname) - 1);
        arr[n].fm.dname[sizeof(arr[n].fm.dname) - 1] = '\0';
        fm_split(nt, arr[n].fm.base, sizeof(arr[n].fm.base), &arr[n].fm.version);
        n++;
    }
    dcl_rms_dir_close(d);

    /* version_selected wants a contiguous file_match array. */
    struct file_match *fms = malloc((size_t)(n > 0 ? n : 1) * sizeof(*fms));
    if (!fms) { free(arr); return SS$_INSFMEM; }
    for (int i = 0; i < n; i++) fms[i] = arr[i].fm;

    int deleted = 0;
    for (int i = 0; i < n; i++) {
        if (!version_selected(fms, i, n, has_version, vspec)) continue;

        if (do_confirm) {
            printf("DELETE %s ? [N]: ", arr[i].spec);
            fflush(stdout);
            char resp[64];
            if (!fgets(resp, sizeof(resp), stdin)) break;
            if (toupper((unsigned char)resp[0]) != 'Y') continue;
        }

        uint32_t est = dcl_rms_erase(ctx, arr[i].spec);
        if (est & 1) {
            deleted++;
            if (do_log)
                dcl_error("DELETE", 3, "FILDEL", "%s deleted", arr[i].spec);
        } else if (est == RMS$_PRV) {
            /* Protection/privilege refusal: report per file, keep going -- the
             * same shape a POSIX EACCES took (RMS$_PRV) before the flip. */
            dcl_error("DELETE", 2, "PRV",
                      "insufficient privilege or file protection violation for %s",
                      arr[i].spec);
        }
    }

    free(fms);
    free(arr);

    if (deleted == 0) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", param);
        return SS$_NOSUCHFILE;
    }
    return SS$_NORMAL;
}

int cmd_delete(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /ENTRY=n qualifier: delete a queue entry */
    if (dcl_has_qualifier(cmd, "ENTRY")) {
        const char *entry_str = dcl_qualifier_value(cmd, "ENTRY");
        if (!entry_str || !entry_str[0]) {
            dcl_error("DCL", 0, "INSFPRM",
                      "missing command parameters - supply all required parameters");
            return SS$_BADPARAM;
        }
        char *endptr;
        long entry_val = strtol(entry_str, &endptr, 10);
        if (endptr == entry_str || *endptr != '\0' || entry_val <= 0) {
            dcl_error("OVMX", 2, "IVENTNUM", "invalid entry number - %s", entry_str);
            return SS$_BADPARAM;
        }
        uint32_t entry_id = (uint32_t)entry_val;
        int qsts = ensure_queue_init();
        if (!(qsts & 1)) {
            dcl_error("JBC", 2, "JOBQUEDIS", "system job queue manager is not running");
            return qsts;
        }
        qsts = vmsq_delete_entry(entry_id);
        if (!(qsts & 1)) {
            /* Faithful two-line VMS rendering: a DELETE/ENTRY of a nonexistent
             * entry prints the command-facility primary chained with the JBC
             * secondary, exactly as the VSI OpenVMS DCL Dictionary DELETE/ENTRY
             * example shows (see docs/audit-message-idents-vms-916.md):
             *   %DELETE-W-SEARCHFAIL, error searching for <n>
             *   -JBC-E-NOSUCHENT, no such entry
             * The '-' continuation prefix is VMS's, not '%', so it is emitted
             * directly rather than through dcl_error() (which always writes a
             * primary '%' line). */
            dcl_error("DELETE", 0, "SEARCHFAIL", "error searching for %u", entry_id);
            fprintf(stderr, "-JBC-E-NOSUCHENT, no such entry\n");
            return qsts;
        }
        printf("%%DELETE-S-DELETED, entry %u deleted\n", entry_id);
        return SS$_NORMAL;
    }

    /* /SYMBOL qualifier: delete a symbol from the symbol table */
    if (dcl_has_qualifier(cmd, "SYMBOL")) {
        if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
            dcl_error("DCL", 2, "NOSYM", "missing symbol name");
            return SS$_BADPARAM;
        }

        int scope = DCL_SYM_LOCAL;
        if (dcl_has_qualifier(cmd, "GLOBAL")) {
            scope = DCL_SYM_GLOBAL;
        }

        int ret = dcl_sym_delete(cmd->params[0], scope);
        if (ret != 0) {
            dcl_error("DCL", 0, "NOSUCHSYM",
                      "no symbol \"%s\" found", cmd->params[0]);
        }
        return SS$_NORMAL;
    }

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* /CONFIRM prompts before each delete; /NOCONFIRM suppresses.
     * The parser stores /NOCONFIRM as name="CONFIRM" negated=1,
     * so dcl_has_qualifier(cmd, "CONFIRM") returns 0 for /NOCONFIRM. */
    int do_confirm = dcl_has_qualifier(cmd, "CONFIRM");
    int do_log = dcl_has_qualifier(cmd, "LOG");

    /*
     * VMS requires an explicit version field on DELETE — a bare name is
     * refused, so that a user cannot delete the wrong version by omission.
     * Accepted forms: ";n" (a version), ";" or ";0" (highest), ";-n"
     * (relative), ";*" (all versions / wildcard).
     *
     * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DELETE — "you
     * must include the version number ... in each file specification"; omitting
     * it yields %DELETE-E-DELVER, "explicit version number or wild card
     * required". (The wild-card allowance is exactly why the guard passes on a
     * ";" of any of the forms above, not only a literal ";n".)
     */
    const char *fcomp = dcl_filename_component(cmd->params[0]);
    const char *fcheck = (fcomp && fcomp[0]) ? fcomp : cmd->params[0];
    if (!strchr(fcheck, ';')) {
        dcl_error("DELETE", 2, "DELVER",
                  "explicit version number or wild card required in %s",
                  cmd->params[0]);
        return SS$_BADPARAM;
    }

    /*
     * Product runtime (vms-0a5): DELETE removes files over the Files-11 ODS-2
     * ACP -- $SEARCH to enumerate, $ERASE (IO$_DELETE) to remove. Only the
     * executive-absent defer (host ctest / self-host container, where the ACP
     * is unreachable) uses the legacy opendir()+unlink() path below. Mirrors
     * exactly how DIRECTORY branches (dir_collect_acp vs dir_collect_host) and
     * how RMS $ERASE itself branches (rms_impl_erase vs rms_posix_erase) --
     * fail-honest on an absent executive, no silent passthrough (INV-6/Rule 9).
     */
    if (!rms_executive_absent())
        return delete_files_acp(ctx, cmd->params[0], do_confirm, do_log);

    /* Directory + name.type pattern + version spec (the text after ';'). */
    char linux_dir[1024], name_pat[512], vspec[64];
    int  has_version;
    split_file_spec(ctx, cmd->params[0], linux_dir, sizeof(linux_dir),
                    name_pat, sizeof(name_pat), vspec, sizeof(vspec),
                    &has_version);

    struct file_match *m = NULL;
    int count = 0;
    int st = fm_collect(linux_dir, name_pat, &m, &count);
    if (st == SS$_NOSUCHFILE) {
        /* Directory could not be opened: the named file cannot exist -> FNF
         * (the same result the old single-file path gave when unlink() failed),
         * rather than surfacing a resolution-artifact directory path. */
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }
    if (st != SS$_NORMAL) {
        free(m);
        return st;
    }

    int deleted = 0;
    for (int i = 0; i < count; i++) {
        if (!version_selected(m, i, count, has_version, vspec)) continue;

        char full[2600];
        snprintf(full, sizeof(full), "%s%s", linux_dir, m[i].dname);

        if (do_confirm) {
            char vms_spec[256];
            dcl_format_filespec(full, vms_spec, sizeof(vms_spec));
            printf("DELETE %s ? [N]: ", vms_spec);
            fflush(stdout);
            char resp[64];
            if (!fgets(resp, sizeof(resp), stdin)) break;
            if (toupper((unsigned char)resp[0]) != 'Y') continue;
        }

        if (unlink(full) == 0) {
            deleted++;
            if (do_log) {
                char vms_spec[256];
                dcl_format_filespec(full, vms_spec, sizeof(vms_spec));
                /* Authentic identifier: %DELETE-I-FILDEL, <spec> deleted. */
                dcl_error("DELETE", 3, "FILDEL", "%s deleted", vms_spec);
            }
        }
    }
    free(m);

    if (deleted == 0) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    return SS$_NORMAL;
}

/*
 * RENAME - Change the name and/or directory of file(s), with VMS wildcard
 * source expansion, field substitution, and output version defaulting.
 *
 * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, RENAME —
 *   - a wildcard input spec renames each matching file, substituting the
 *     wildcard output fields from the corresponding input fields;
 *   - a directory in the output spec moves the file to that directory;
 *   - the output version defaults like COPY (one greater than the highest
 *     existing version of the same name.type, else the name is kept);
 *   - /LOG reports each with "%RENAME-I-RENAMED, in renamed to out".
 */
int cmd_rename(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE", "missing source and/or destination");
        return SS$_BADPARAM;
    }

    int do_log      = dcl_has_qualifier(cmd, "LOG");
    int new_version = dcl_has_qualifier(cmd, "NEW_VERSION");

    /* Source: directory + name.type pattern + version spec. */
    char src_dir[1024], src_pat[512], src_vspec[64];
    int  src_hasver;
    split_file_spec(ctx, cmd->params[0], src_dir, sizeof(src_dir),
                    src_pat, sizeof(src_pat),
                    src_vspec, sizeof(src_vspec), &src_hasver);

    /* Destination: an existing directory keeps each source name/type (fields
     * "*"); otherwise parse the destination name.type;ver fields. */
    char dst_dir[1024];
    char dname_pat[256] = "*", dtype_pat[256] = "*", dst_vspec[64] = "";
    int  dst_hasver = 0;

    char dst_resolved[1024];
    dcl_resolve_path(ctx, cmd->params[1], dst_resolved, sizeof(dst_resolved));
    struct stat dst_st;
    int dst_is_dir = (stat(dst_resolved, &dst_st) == 0 && S_ISDIR(dst_st.st_mode));

    if (dst_is_dir) {
        size_t dl = strlen(dst_resolved);
        strncpy(dst_dir, dst_resolved, sizeof(dst_dir) - 1);
        dst_dir[sizeof(dst_dir) - 1] = '\0';
        if (dl && dst_dir[dl - 1] != '/' && dl < sizeof(dst_dir) - 1) {
            dst_dir[dl] = '/';
            dst_dir[dl + 1] = '\0';
        }
    } else {
        char dnamepat[256];
        split_file_spec(ctx, cmd->params[1], dst_dir, sizeof(dst_dir),
                        dnamepat, sizeof(dnamepat),
                        dst_vspec, sizeof(dst_vspec), &dst_hasver);
        split_name_type(dnamepat, dname_pat, sizeof(dname_pat),
                        dtype_pat, sizeof(dtype_pat));
    }

    struct file_match *m = NULL;
    int count = 0;
    int st = fm_collect(src_dir, src_pat, &m, &count);
    if (st != SS$_NORMAL) {
        free(m);
        dcl_error("RMS", 2, "RNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    int renamed = 0, matched = 0;
    for (int i = 0; i < count; i++) {
        if (!version_selected(m, i, count, src_hasver, src_vspec)) continue;
        matched++;

        char in_name[256], in_type[256];
        split_name_type(m[i].base, in_name, sizeof(in_name),
                        in_type, sizeof(in_type));

        char out_name[256], out_type[256];
        map_out_field(dname_pat, in_name, out_name, sizeof(out_name));
        map_out_field(dtype_pat, in_type, out_type, sizeof(out_type));

        int out_ver = 0;
        if (!resolve_out_version(dst_dir, out_name, out_type,
                                 dst_hasver, dst_vspec, new_version, &out_ver)) {
            dcl_error("RMS", 2, "FEX",
                      "file already exists, not superseded - %s.%s;%d",
                      out_name, out_type, out_ver);
            continue;
        }

        char dfile[600];
        if (out_ver > 0) {
            if (out_type[0])
                snprintf(dfile, sizeof(dfile), "%s.%s;%d",
                         out_name, out_type, out_ver);
            else
                snprintf(dfile, sizeof(dfile), "%s;%d", out_name, out_ver);
        } else {
            if (out_type[0])
                snprintf(dfile, sizeof(dfile), "%s.%s", out_name, out_type);
            else
                snprintf(dfile, sizeof(dfile), "%s", out_name);
        }

        char src_full[2600], dst_full[2600];
        snprintf(src_full, sizeof(src_full), "%s%s", src_dir, m[i].dname);
        snprintf(dst_full, sizeof(dst_full), "%s%s", dst_dir, dfile);

        if (rename(src_full, dst_full) != 0) {
            dcl_error("RMS", 2, "RNF",
                      "rename failed - %s", vms_strerror(errno));
            continue;
        }

        renamed++;
        if (do_log) {
            char vsrc[256], vdst[256];
            dcl_format_filespec(src_full, vsrc, sizeof(vsrc));
            dcl_format_filespec(dst_full, vdst, sizeof(vdst));
            dcl_error("RENAME", 3, "RENAMED",
                      "%s renamed to %s", vsrc, vdst);
        }
    }
    free(m);

    if (matched == 0) {
        dcl_error("RMS", 2, "RNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }
    if (renamed == 0)
        return SS$_FILACCERR;

    return SS$_NORMAL;
}

/*
 * CREATE - Create a new file, or (with /DIRECTORY) create a directory.
 */
int cmd_create(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    /* /DIRECTORY qualifier: create a directory instead of a file */
    if (dcl_has_qualifier(cmd, "DIRECTORY")) {
        if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
            dcl_error("DCL", 2, "NODIR", "missing directory specification");
            return SS$_BADPARAM;
        }

        char linux_path[1024];
        dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

        /* Remove trailing slash before mkdir */
        size_t plen = strlen(linux_path);
        if (plen > 1 && linux_path[plen - 1] == '/') {
            linux_path[plen - 1] = '\0';
        }

        if (mkdir(linux_path, 0755) != 0) {
            if (errno == EEXIST) {
                dcl_error("DCL", 0, "CREATED",
                          "directory already exists - %s", cmd->params[0]);
                return SS$_NORMAL;
            }
            dcl_error("RMS", 2, "CRE",
                      "cannot create directory - %s: %s",
                      cmd->params[0], vms_strerror(errno));
            return SS$_FILACCERR;
        }

        return SS$_NORMAL;
    }

    if (cmd->param_count < 1 || cmd->params[0][0] == '\0') {
        dcl_error("DCL", 2, "NOFILE", "missing file specification");
        return SS$_BADPARAM;
    }

    /* Validate against ODS-2 naming rules before resolving the path.
     * Strip any directory spec to get just the filename portion. */
    const char *fname_for_check = cmd->params[0];
    /* Skip past device: and [dir] if present */
    const char *bracket_end = strrchr(fname_for_check, ']');
    if (bracket_end) fname_for_check = bracket_end + 1;
    else {
        const char *col = strrchr(fname_for_check, ':');
        if (col) fname_for_check = col + 1;
    }
    /* Strip version ;N for ODS-2 check */
    char name_check[256];
    strncpy(name_check, fname_for_check, sizeof(name_check) - 1);
    name_check[sizeof(name_check) - 1] = '\0';
    char *semi_check = strrchr(name_check, ';');
    if (semi_check) *semi_check = '\0';

    if (name_check[0] != '\0' && !vmsfs_is_valid_ods2_name(name_check)) {
        dcl_error("RMS", 2, "SYN",
                  "invalid ODS-2 filename - %s", cmd->params[0]);
        return SS$_BADPARAM;
    }

    /*
     * VMS CREATE mints a NEW version of the file — it never truncates an
     * existing one. vms-481: CREATE now writes through RMS ($CREATE + sequential
     * $PUT), which on the product runtime reaches the Files-11 ODS-2 ACP over
     * /dev/vms. RMS/IO$_CREATE itself allocates a real FID from INDEXF.SYS and
     * enters the file in its directory at a new HIGHEST version -- so the
     * next-higher-version rule is now the executive's, not an opendir scan. An
     * explicit output ;n that already exists is refused by the ACP as RMS$_FEX.
     * Fail-honest: no ACP-mounted SYS$DISK => RMS$_CRE/ACC, never a silent
     * fopen("w") truncate on a /vms passthrough (INV-6).
     *
     * Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, CREATE —
     * "creates a sequential disk file" from SYS$INPUT and, like every RMS
     * file creation, assigns the next-higher version.
     */
    uint32_t cst = 0;
    /* Text file: variable-length records, implied carriage return (VMS default
     * for CREATE from SYS$INPUT). MRS 0 = no max. */
    struct dcl_rms_writer *w =
        dcl_rms_write_create(ctx, cmd->params[0], FAB$C_VAR, FAB$M_CR, 0, &cst);
    if (!w) {
        if (cst == RMS$_FEX) {
            dcl_error("RMS", 2, "FEX",
                      "file already exists, not superseded - %s", cmd->params[0]);
            return RMS$_FEX;
        }
        dcl_error("RMS", 2, "CRE", "cannot create - %s", cmd->params[0]);
        return SS$_FILACCERR;
    }

    /* CREATE reads records from SYS$INPUT until end-of-file. Interactively that
     * is the terminal (Ctrl-Z ends it); inside a command procedure it is the
     * procedure's following in-stream data lines -- the classic
     *   $ CREATE FILE.TXT
     *   ...data...
     *   $ NEXTCOMMAND
     * idiom, and the consumer of a DECK/EOD block. dcl_sysinput_setup() points
     * fd 0 at exactly that block (and is a no-op interactively), so the same
     * read loop serves both. Reference: DCL Dictionary, "CREATE" (input from
     * SYS$INPUT); "DECK"/"EOD". (vms-3983) Each input line becomes one RMS
     * record ($PUT), with the trailing newline stripped (RMS records carry no
     * terminator; the RAT=CR attribute supplies it on read-back). */
    struct dcl_sysinput si;
    dcl_sysinput_setup(ctx, &si);
    if (ctx->interactive || ctx->proc_depth >= 0) {
        char line[4096];
        while (1) {
            if (!fgets(line, sizeof(line), stdin)) break;
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r'))
                ll--;
            if (dcl_rms_write_record(w, line, ll) != 0) break;
        }
    }
    dcl_sysinput_restore(&si);

    dcl_rms_write_close(w);
    return SS$_NORMAL;
}

/*
 * SEARCH - Search file for a string (like grep).
 */
int cmd_search(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing file specification and/or search string");
        return SS$_BADPARAM;
    }

    char linux_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], linux_path, sizeof(linux_path));

    const char *search_str = cmd->params[1];
    int exact = dcl_has_qualifier(cmd, "EXACT");
    int show_numbers = dcl_has_qualifier(cmd, "NUMBERS");
    int show_stats = dcl_has_qualifier(cmd, "STATISTICS");

    FILE *fp = fopen(linux_path, "r");
    if (!fp) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    /* Display header */
    char vms_name[256];
    const char *basename = strrchr(linux_path, '/');
    if (basename) basename++; else basename = linux_path;
    size_t i;
    for (i = 0; i < sizeof(vms_name) - 1 && basename[i]; i++)
        vms_name[i] = (char)toupper((unsigned char)basename[i]);
    vms_name[i] = '\0';

    char line[4096];
    int found = 0;
    int line_num = 0;
    int match_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        int match;

        if (exact) {
            match = (strstr(line, search_str) != NULL);
        } else {
            /* Case-insensitive search */
            char lower_line[4096], lower_search[1024];
            for (i = 0; line[i] && i < sizeof(lower_line) - 1; i++)
                lower_line[i] = (char)tolower((unsigned char)line[i]);
            lower_line[i] = '\0';
            for (i = 0; search_str[i] && i < sizeof(lower_search) - 1; i++)
                lower_search[i] = (char)tolower((unsigned char)search_str[i]);
            lower_search[i] = '\0';
            match = (strstr(lower_line, lower_search) != NULL);
        }

        if (match) {
            if (!found) {
                printf("\n******************************\n%s\n", vms_name);
                found = 1;
            }
            /* Remove trailing newline for cleaner output */
            size_t llen = strlen(line);
            if (llen > 0 && line[llen - 1] == '\n') line[llen - 1] = '\0';
            if (show_numbers) {
                printf("%d: %s\n", line_num, line);
            } else {
                printf("%s\n", line);
            }
            match_count++;
        }
    }

    fclose(fp);

    if (show_stats) {
        printf("\n%d lines matched in 1 file\n", match_count);
    }

    if (!found) {
        dcl_error("SEARCH", 0, "NOMATCHES",
                  "no strings matched");
        return SS$_NORMAL; /* Warning, not error */
    }

    return SS$_NORMAL;
}

/* vmsfs version management API (from vmsfs/version.h) */
extern int vmsfs_purge_versions(const char *linux_dir, const char *basename,
                                const char *ext, int keep_count);
extern int vmsfs_list_versions(const char *linux_dir, const char *basename,
                               const char *ext, int *versions, int max_versions,
                               int *count);

/*
 * PURGE - Delete all but the highest N versions of files.
 *
 * Syntax: PURGE [filespec] [/KEEP=n]
 *   filespec  - VMS file specification (wildcards allowed); defaults to *.*
 *   /KEEP=n   - Number of versions to keep; default is 1
 *
 * Calls vmsfs_purge_versions() for each matching file base name found
 * in the target directory.
 */
int cmd_purge(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    int do_log = dcl_has_qualifier(cmd, "LOG");
    int do_confirm = dcl_has_qualifier(cmd, "CONFIRM");

    /* Determine keep count from /KEEP=n qualifier */
    int keep_count = 1;
    const char *keep_val = dcl_qualifier_value(cmd, "KEEP");
    if (keep_val && keep_val[0]) {
        char *endp;
        keep_count = (int)strtol(keep_val, &endp, 10);
        if (endp == keep_val || *endp != '\0' || keep_count < 1) keep_count = 1;
    }

    /* Determine the target directory and pattern */
    char linux_dir[1024];
    const char *pattern = NULL;

    if (cmd->param_count >= 1 && cmd->params[0][0] != '\0') {
        char resolved[1024];
        dcl_resolve_path(ctx, cmd->params[0], resolved, sizeof(resolved));

        struct stat st;
        if (stat(resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(linux_dir, resolved, sizeof(linux_dir) - 1);
            linux_dir[sizeof(linux_dir) - 1] = '\0';
        } else {
            /* Split path into directory + filename pattern. Use the
             * ORIGINAL filename+version text, not resolved's own
             * basename — see dcl_filename_component()'s doc comment. */
            const char *orig = dcl_filename_component(cmd->params[0]);
            char *last_slash = strrchr(resolved, '/');
            if (last_slash) {
                pattern = strdup((orig && orig[0]) ? orig : last_slash + 1);
                *(last_slash + 1) = '\0';
                strncpy(linux_dir, resolved, sizeof(linux_dir) - 1);
                linux_dir[sizeof(linux_dir) - 1] = '\0';
            } else {
                pattern = strdup((orig && orig[0]) ? orig : resolved);
                vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
            }
        }
    } else {
        /* Default: current directory, all files (*.*) */
        vmsfs_to_linux_path(ctx->default_dir, linux_dir, sizeof(linux_dir));
    }

    /* Ensure trailing slash */
    size_t dlen = strlen(linux_dir);
    if (dlen > 0 && linux_dir[dlen - 1] != '/') {
        if (dlen < sizeof(linux_dir) - 1) {
            linux_dir[dlen] = '/';
            linux_dir[dlen + 1] = '\0';
        }
    }

    /* Scan the directory and collect unique base names (name without version).
     * For each unique base+ext, call vmsfs_purge_versions(). */
    DIR *dir = opendir(linux_dir);
    if (!dir) {
        dcl_error("RMS", 2, "DNF", "directory not found - %s", linux_dir);
        if (pattern) free((void *)pattern);
        return SS$_NOSUCHFILE;
    }

    /* Track processed bases to avoid duplicate purge calls */
    struct purge_base {
        char name[64];
        char ext[64];
    };
    int cap = 64;
    struct purge_base *bases = malloc((size_t)cap * sizeof(*bases));
    if (!bases) {
        closedir(dir);
        if (pattern) free((void *)pattern);
        return SS$_INSFMEM;
    }
    int base_count = 0;
    int total_deleted = 0;

    struct dirent *de;
    /* Remove trailing slash for opendir (already done) — iterate entries */
    /* Strip the trailing slash from linux_dir to use as dir arg to vmsfs */
    char dir_notrail[1024];
    strncpy(dir_notrail, linux_dir, sizeof(dir_notrail) - 1);
    dir_notrail[sizeof(dir_notrail) - 1] = '\0';
    size_t dtlen = strlen(dir_notrail);
    if (dtlen > 1 && dir_notrail[dtlen - 1] == '/') {
        dir_notrail[dtlen - 1] = '\0';
    }

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        /* Only process versioned files (must contain ;N) */
        const char *semi = strrchr(de->d_name, ';');
        if (!semi) continue;

        /* Apply wildcard pattern filter if one was given */
        if (pattern) {
            if (!vmsfs_wildcard_match(pattern, de->d_name)) continue;
        }

        /* Extract base (name) and ext from the portion before ';' */
        char base_ext[256];
        size_t belen = (size_t)(semi - de->d_name);
        if (belen >= sizeof(base_ext)) belen = sizeof(base_ext) - 1;
        memcpy(base_ext, de->d_name, belen);
        base_ext[belen] = '\0';

        /* Split base_ext into name and extension */
        char fname[64] = {0};
        char fext[64]  = {0};
        const char *dot = strrchr(base_ext, '.');
        if (dot) {
            size_t nlen = (size_t)(dot - base_ext);
            if (nlen >= sizeof(fname)) nlen = sizeof(fname) - 1;
            memcpy(fname, base_ext, nlen);
            fname[nlen] = '\0';
            strncpy(fext, dot + 1, sizeof(fext) - 1);
            fext[sizeof(fext) - 1] = '\0';
        } else {
            strncpy(fname, base_ext, sizeof(fname) - 1);
            fname[sizeof(fname) - 1] = '\0';
        }

        /* Check if we already processed this base */
        int already = 0;
        for (int k = 0; k < base_count; k++) {
            if (strcasecmp(bases[k].name, fname) == 0 &&
                strcasecmp(bases[k].ext,  fext)  == 0) {
                already = 1;
                break;
            }
        }
        if (already) continue;

        /* Record this base */
        if (base_count >= cap) {
            cap *= 2;
            struct purge_base *tmp = realloc(bases, (size_t)cap * sizeof(*bases));
            if (!tmp) { free(bases); closedir(dir);
                        if (pattern) free((void *)pattern);
                        return SS$_INSFMEM; }
            bases = tmp;
        }
        strncpy(bases[base_count].name, fname, sizeof(bases[0].name) - 1);
        bases[base_count].name[sizeof(bases[0].name) - 1] = '\0';
        strncpy(bases[base_count].ext,  fext,  sizeof(bases[0].ext)  - 1);
        bases[base_count].ext[sizeof(bases[0].ext) - 1] = '\0';
        base_count++;

        /* /CONFIRM: prompt before purging this file */
        if (do_confirm) {
            char upper_name[128];
            size_t ui;
            for (ui = 0; ui < sizeof(upper_name) - 1 && fname[ui]; ui++)
                upper_name[ui] = (char)toupper((unsigned char)fname[ui]);
            upper_name[ui] = '\0';
            char upper_ext[64];
            for (ui = 0; ui < sizeof(upper_ext) - 1 && fext[ui]; ui++)
                upper_ext[ui] = (char)toupper((unsigned char)fext[ui]);
            upper_ext[ui] = '\0';
            printf("PURGE %s%s%s ? [N]: ", upper_name,
                   upper_ext[0] ? "." : "", upper_ext);
            fflush(stdout);
            char resp[64];
            if (!fgets(resp, sizeof(resp), stdin) ||
                toupper((unsigned char)resp[0]) != 'Y')
                continue;
        }

        /* Purge old versions for this file */
        int deleted = vmsfs_purge_versions(dir_notrail, fname,
                                            fext[0] ? fext : NULL, keep_count);
        if (deleted > 0) {
            total_deleted += deleted;
            if (do_log) {
                char upper_name[128];
                size_t ui;
                for (ui = 0; ui < sizeof(upper_name) - 1 && fname[ui]; ui++)
                    upper_name[ui] = (char)toupper((unsigned char)fname[ui]);
                upper_name[ui] = '\0';
                char upper_ext[64];
                for (ui = 0; ui < sizeof(upper_ext) - 1 && fext[ui]; ui++)
                    upper_ext[ui] = (char)toupper((unsigned char)fext[ui]);
                upper_ext[ui] = '\0';
                dcl_error("PURGE", 1, "PURGED",
                          "%s%s%s purged (%d version%s removed)",
                          upper_name, upper_ext[0] ? "." : "", upper_ext,
                          deleted, deleted != 1 ? "s" : "");
            }
        }
    }
    closedir(dir);
    free(bases);
    if (pattern) free((void *)pattern);

    if (total_deleted == 0) {
        printf("%%PURGE-I-NOPURGE, no file versions to purge\n");
    } else {
        printf("%%PURGE-I-PURGED, %d old version%s deleted\n",
               total_deleted, total_deleted != 1 ? "s" : "");
    }
    return SS$_NORMAL;
}

/*
 * APPEND - Append one file to another.
 * Format: APPEND source-filespec destination-filespec
 */
int cmd_append(struct dcl_command *cmd)
{
    struct dcl_context *ctx = dcl_get_context();

    if (cmd->param_count < 2) {
        dcl_error("DCL", 2, "NOFILE",
                  "missing source and/or destination file specification");
        return SS$_BADPARAM;
    }

    char src_path[1024], dst_path[1024];
    dcl_resolve_path(ctx, cmd->params[0], src_path, sizeof(src_path));
    dcl_resolve_path(ctx, cmd->params[1], dst_path, sizeof(dst_path));

    FILE *src = fopen(src_path, "rb");
    if (!src) {
        dcl_error("RMS", 2, "FNF", "file not found - %s", cmd->params[0]);
        return SS$_NOSUCHFILE;
    }

    FILE *dst = fopen(dst_path, "ab");
    if (!dst) {
        fclose(src);
        dcl_error("RMS", 2, "CRE", "cannot open for append - %s", cmd->params[1]);
        return SS$_FILACCERR;
    }

    char buf[8192];
    size_t n;
    int write_err = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            write_err = 1;
            break;
        }
    }

    fclose(src);
    fclose(dst);

    if (write_err) {
        dcl_error("RMS", 2, "WER", "write error - %s", cmd->params[1]);
        return SS$_ABORT;
    }

    return SS$_NORMAL;
}
