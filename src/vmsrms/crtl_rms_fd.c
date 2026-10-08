/*
 * crtl_rms_fd.c - the C RTL's file descriptors over RMS (vms-b90, the file
 * half of vms-254).
 *
 * On OpenVMS the C RTL is a client of RMS: open() is $OPEN/$CREATE, read() and
 * write() on a stream file are RMS block I/O, close() is $CLOSE. OVMX's
 * DECC$SHR is musl, whose every system call funnels through one function,
 * __vms_alpha_syscall (tools/cross-alpha-vms/musl-arch). This file installs a
 * hook there, so the whole musl file API -- open/read/write/lseek/close/dup,
 * fstat/stat, access, unlink, rename, and all of stdio on top of them -- runs
 * against the Files-11 ODS-2 volume through the public RMS services, with no
 * change to musl's stdio and no per-function veneer.
 *
 * WHICH PATHS ARE RMS FILES. The C RTL's file system is RMS. Every path is an
 * RMS file specification -- OpenVMS syntax as written, UNIX syntax translated
 * by the DEC C rules (crtl_filespec.c, the decc$to_vms translator) -- EXCEPT the
 * substrate kernel's own namespaces, which stay with the kernel: /dev, /proc,
 * /sys and /run (the device nodes, /dev/vms among them, and the boot
 * launcher). A path relative to a directory descriptor other than the current
 * directory is not an RMS form and goes to the kernel too.
 *
 * DESCRIPTOR NUMBERS. An RMS file's descriptor is a real kernel descriptor
 * number: a placeholder (O_PATH on "/", close-on-exec) reserves it, so RMS and
 * kernel descriptors (pipes, sockets, terminals) share one number space, dup2()
 * onto 0/1/2 works, and a number is never handed out twice. The placeholder is
 * close-on-exec so a subprocess never inherits a descriptor that would read
 * as an empty file: an RMS file is not passed across exec (EBADF there, never a
 * silent wrong file).
 *
 * I/O. Stream-format files (STMLF, UDF, FIX) are byte streams: read/write/lseek
 * are $READ/$WRITE block I/O at the byte position, with a read-modify-write of
 * a partial block, and the end of file kept byte-exact (sys$write moves it to
 * the end of the transfer). Record-format files (VAR, VFC, STM, STMCR) are read
 * the way the DEC C RTL reads them: $GET record by record, each record followed
 * by a newline when the file has carriage-control attributes. Writing a
 * record-format file, positioning one other than to its start or current
 * position, and directories as descriptors are not provided yet: those calls
 * fail with an honest errno (EOPNOTSUPP / ESPIPE), never a silent success.
 * A file created here is a new version, Stream_LF with carriage-return
 * attributes -- what the DEC C RTL creates.
 *
 * STAT. statx/fstat/fstatat on an RMS file report the RMS attributes:
 * st_size the end of file (the byte count of a stream file; for a record file
 * the stored byte count, which the C RTL reports the same way), st_mode a
 * regular file with the UNIX bits the owner/group/world protection grants,
 * st_uid/st_gid the owner UIC, times from the revision and creation dates,
 * st_ino the File ID.
 *
 * Single-threaded, as the rest of DECC$SHR (non-TLS producer).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include "rms/rms.h"
#include "rms/xab.h"
#include "rms/crtl_filespec.h"
#include "rms_io.h"                 /* the open file's File ID (vms-692 adds NAM$W_FID) */
#include "kstat.h"                  /* musl-arch: the fstat system-call buffer */

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef F_DUPFD_CLOEXEC
#define F_DUPFD_CLOEXEC 1030
#endif

/* The raw trap and the hook cell (musl-arch src/internal/vms_alpha_syscall.c). */
typedef long long (*ovmx_sys_hook_fn)(long long, long long, long long, long long,
                                      long long, long long, long long, int *);
extern ovmx_sys_hook_fn __ovmx_sys_hook;
extern long long __vms_alpha_syscall_raw(long long, long long, long long,
                                         long long, long long, long long,
                                         long long);

static long long rawsys(long long n, long long a1, long long a2, long long a3,
                        long long a4, long long a5)
{
    return __vms_alpha_syscall_raw(n, a1, a2, a3, a4, a5, 0);
}

/* OVMX_CRTLFD_TRACE: a debugging build writes each step to fd 2 with the raw
 * trap (never through the hook). Off in every shipped build. */
#ifdef OVMX_CRTLFD_TRACE
static void tr(const char *m, long long v)
{
    char b[96];
    int n = 0;
    while (*m && n < 60)
        b[n++] = *m++;
    b[n++] = ' ';
    unsigned long long u = (unsigned long long)v;
    char h[17];
    int i = 16;
    h[16] = 0;
    do { h[--i] = "0123456789abcdef"[u & 15]; u >>= 4; } while (u && i);
    while (h[i]) b[n++] = h[i++];
    b[n++] = '\n';
    __vms_alpha_syscall_raw(SYS_write, 2, (long long)(uintptr_t)b, n, 0, 0, 0);
}
#define TR(m, v) tr(m, (long long)(v))
#else
#define TR(m, v) ((void)0)
#endif

#define BLK        512u
#define IOBUF      (126u * BLK)     /* largest block-multiple a RAB word holds */
#define RFD_MAX    1024

enum { RF_STREAM = 1, RF_RECORD = 2 };

/* An open file description: shared by every descriptor dup()ed from it. */
struct rfile {
    int       refs;
    int       kind;                 /* RF_STREAM / RF_RECORD                   */
    int       oflags;               /* O_ACCMODE | O_APPEND | O_NONBLOCK        */
    struct FAB fab;
    struct RAB rab;
    struct NAM nam;
    char      spec[256];
    char      rsa[256];
    char      esa[256];
    uint64_t  pos;                  /* byte position                           */
    uint64_t  eof;                  /* end of file (stream files)               */
    /* record-mode read: the current record + its newline, served bytewise */
    char     *rec;
    size_t    reclen, recoff;
    int       rec_eof;
    char     *io;                   /* IOBUF bytes, block-I/O staging          */
};

static struct rfile *rfd[RFD_MAX];
static unsigned char rfd_cloexec[RFD_MAX];   /* the user's FD_CLOEXEC view   */
static int in_rms;                           /* RMS's own syscalls -> kernel */

static struct rfile *rget(long long fd)
{
    return (fd >= 0 && fd < RFD_MAX) ? rfd[fd] : NULL;
}

/* -------------------------------------------------------- status -> errno -- */

static int rms_errno(uint32_t st)
{
    switch (st) {
    case RMS$_FNF: case RMS$_DNF: case RMS$_DEV:
        return ENOENT;
    case RMS$_PRV:
        return EACCES;
    case RMS$_FLK:
        return EBUSY;
    case RMS$_FEX:
        return EEXIST;
    case RMS$_SYN: case RMS$_FNM: case RMS$_DIR: case RMS$_TYP: case RMS$_VER:
        return EINVAL;
    case RMS$_EOF:
        return 0;
    default:
        return EIO;
    }
}

/* ------------------------------------------------------------- paths -------- */

static int kernel_path(const char *p)
{
    static const char *const ns[] = { "/dev", "/proc", "/sys", "/run" };
    if (!p || !*p)
        return 1;
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        size_t n = strlen(ns[i]);
        if (strncmp(p, ns[i], n) == 0 && (p[n] == '\0' || p[n] == '/'))
            return 1;
    }
    return 0;
}

/* The RMS spec for (dirfd, path), or -1 if the path is the kernel's; sets *dir
 * when the spec names a directory. */
static int rms_spec(long long dirfd, const char *path, char *out, size_t outsz,
                    int *dir)
{
    if (kernel_path(path))
        return -1;
    if (path[0] != '/' && !ovmx_crtl_is_vms_syntax(path) && dirfd != AT_FDCWD)
        return -1;                  /* relative to another directory fd */
    int k = ovmx_crtl_unix_to_vms(path, out, outsz, OVMX_FS_AUTO);
    if (k < 0)
        return -2;                  /* errno set: an invalid RMS spec */
    if (dir) {
        size_t n = strlen(out);
        *dir = k == OVMX_FS_DIR ||
               (k == OVMX_FS_PASSTHRU && n && (out[n - 1] == ']' || out[n - 1] == '>'));
    }
    return 0;
}

/* -------------------------------------------------------- attributes ------- */

struct rattr {
    uint64_t size, blocks, ino, cdt, rdt;
    uint32_t uid, gid, dev;
    uint16_t mode;
};

/* VMS binary time (100 ns since 17-NOV-1858) -> UNIX seconds. */
static int64_t vms_to_unix_time(uint64_t q)
{
    const uint64_t epoch = 35067168000000000ULL;
    if (q < epoch)
        return 0;
    return (int64_t)((q - epoch) / 10000000ULL);
}

/* SOGW protection (a set bit DENIES R,W,E,D per class) -> UNIX permission bits:
 * owner, group, world. */
static uint16_t vms_prot_mode(uint16_t pro)
{
    uint16_t m = 0;
    const int cls[3] = { 4, 8, 12 };             /* owner, group, world nibbles */
    for (int i = 0; i < 3; i++) {
        unsigned nib = (pro >> cls[i]) & 0xFu;
        int sh = 6 - 3 * i;
        if (!(nib & 1u)) m |= (uint16_t)(4u << sh);   /* R */
        if (!(nib & 2u)) m |= (uint16_t)(2u << sh);   /* W */
        if (!(nib & 4u)) m |= (uint16_t)(1u << sh);   /* E */
    }
    return m;
}

static uint32_t devhash(const char *rsa)
{
    uint32_t h = 2166136261u;
    for (const char *p = rsa; *p && *p != ':'; p++)
        h = (h ^ (uint8_t)(*p & ~0x20)) * 16777619u;
    return h & 0xFFFFu;
}

/* $DISPLAY the open FAB's XABFHC/XABDAT/XABPRO and fill *a. */
static int fill_attr(struct FAB *fab, struct NAM *nam, const char *rsa, struct rattr *a)
{
    struct XABFHC fhc = cc$rms_xabfhc;
    struct XABDAT dat = cc$rms_xabdat;
    struct XABPRO pro = cc$rms_xabpro;
    fhc.xab$l_nxt = &dat;
    dat.xab$l_nxt = &pro;
    void *save = fab->fab$l_xab;
    fab->fab$l_xab = (struct XABKEY *)&fhc;
    TR("crtlfd: $display ...", 0);
    uint32_t st = sys$display(fab, 0, 0);
    TR("crtlfd: $display", st);
    fab->fab$l_xab = save;
    (void)nam;
    if (!(st & 1))
        return -rms_errno(st);
    memset(a, 0, sizeof *a);
    if (fhc.xab$l_ebk)
        a->size = (uint64_t)(fhc.xab$l_ebk - 1u) * BLK + fhc.xab$w_ffb;
    a->blocks = fhc.xab$l_hbk;
    a->cdt = dat.xab$q_cdt;
    a->rdt = dat.xab$q_rdt;
    a->mode = (uint16_t)(S_IFREG | vms_prot_mode(pro.xab$w_pro));
    a->uid = pro.xab$l_uic & 0xFFFFu;           /* member */
    a->gid = pro.xab$l_uic >> 16;               /* group  */
    rms_file_t *f = fab->_rms_file;
    if (f)
        a->ino = (uint64_t)f->fid_num | ((uint64_t)f->fid_seq << 16) |
                 ((uint64_t)f->fid_rvn << 32) | ((uint64_t)f->fid_nmx << 40);
    a->dev = devhash(rsa);
    return 0;
}

struct ovmx_statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode, pad1;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct { int64_t tv_sec; uint32_t tv_nsec; int32_t pad; }
        stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t spare[14];
};

static void attr_to_statx(const struct rattr *a, struct ovmx_statx *x)
{
    memset(x, 0, sizeof *x);
    x->stx_mask = 0x7ff;
    x->stx_blksize = BLK;
    x->stx_nlink = 1;
    x->stx_uid = a->uid;
    x->stx_gid = a->gid;
    x->stx_mode = a->mode;
    x->stx_ino = a->ino;
    x->stx_size = a->size;
    x->stx_blocks = a->blocks;
    x->stx_atime.tv_sec = vms_to_unix_time(a->rdt);
    x->stx_mtime.tv_sec = vms_to_unix_time(a->rdt);
    x->stx_ctime.tv_sec = vms_to_unix_time(a->rdt);
    x->stx_btime.tv_sec = vms_to_unix_time(a->cdt);
    x->stx_dev_major = a->dev >> 8;
    x->stx_dev_minor = a->dev & 0xFFu;
}

/* The SYS_fstat / SYS_newfstatat buffer is musl's struct kstat for this arch
 * (musl-arch kstat.h). musl's own fstat/stat take the statx path above; this
 * answers a direct fstat/fstatat system call in the same layout musl reads. */
static void attr_to_kstat(const struct rattr *a, struct kstat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = a->dev;
    st->st_ino = a->ino;
    st->st_mode = a->mode;
    st->st_nlink = 1;
    st->st_uid = a->uid;
    st->st_gid = a->gid;
    st->st_size = (off_t)a->size;
    st->st_blksize = BLK;
    st->st_blocks = (blkcnt_t)a->blocks;
    st->st_atime_sec = vms_to_unix_time(a->rdt);
    st->st_mtime_sec = vms_to_unix_time(a->rdt);
    st->st_ctime_sec = vms_to_unix_time(a->rdt);
}

/* Stat a path that is not open: $OPEN for attributes (read, shared), or for a
 * directory spec "DEV:[A.B]" look up its directory file "DEV:[A]B.DIR;1". */
static int stat_path(const char *spec, int dir, struct rattr *a)
{
    char dirfile[300];
    const char *open_spec = spec;
    if (dir) {
        /* "[A.B]" -> "[A]B.DIR;1"; "[A]" -> "[000000]A.DIR;1"; "[]"/"[-]" and
         * the MFD are directories by definition (the current/parent/root). */
        const char *lb = strpbrk(spec, "[<");
        const char *rb = lb ? strpbrk(lb, "]>") : NULL;
        if (!lb || !rb)
            return -ENOENT;
        size_t dl = (size_t)(rb - lb - 1);
        const char *inner = lb + 1;
        const char *last = NULL;
        for (const char *p = inner; p < rb; p++)
            if (*p == '.')
                last = p;
        if (dl == 0 || inner[0] == '-' || (dl == 6 && !memcmp(inner, "000000", 6)) ||
            (inner[0] == '.' && last == inner && dl == 1)) {
            memset(a, 0, sizeof *a);
            a->mode = S_IFDIR | 0755;
            a->dev = devhash(spec);
            return 0;
        }
        size_t pre = (size_t)(lb - spec);
        size_t n;
        if (last && last > inner) {
            n = (size_t)snprintf(dirfile, sizeof dirfile, "%.*s%.*s%c%.*s.DIR;1",
                                 (int)pre, spec, (int)(last - lb), lb,
                                 *lb == '[' ? ']' : '>', (int)(rb - last - 1), last + 1);
        } else if (inner[0] == '.') {
            n = (size_t)snprintf(dirfile, sizeof dirfile, "%.*s[]%.*s.DIR;1",
                                 (int)pre, spec, (int)(dl - 1), inner + 1);
        } else {
            n = (size_t)snprintf(dirfile, sizeof dirfile, "%.*s[000000]%.*s.DIR;1",
                                 (int)pre, spec, (int)dl, inner);
        }
        if (n >= sizeof dirfile)
            return -ENAMETOOLONG;
        open_spec = dirfile;
    }

    struct FAB fab = cc$rms_fab;
    struct NAM nam = cc$rms_nam;
    char rsa[256], esa[256];
    fab.fab$l_fna = (char *)open_spec;
    fab.fab$b_fns = (uint8_t)strlen(open_spec);
    fab.fab$b_fac = FAB$M_GET;
    fab.fab$b_shr = FAB$M_SHRGET | FAB$M_SHRPUT | FAB$M_SHRUPD | FAB$M_SHRDEL;
    fab.fab$l_nam = &nam;
    nam.nam$l_rsa = rsa;
    nam.nam$b_rss = sizeof rsa - 1;
    nam.nam$l_esa = esa;
    nam.nam$b_ess = sizeof esa - 1;
    uint32_t st = sys$open(&fab, 0, 0);
    if (!(st & 1))
        return -(rms_errno(st) ? rms_errno(st) : EIO);
    rsa[nam.nam$b_rsl] = '\0';
    int r = fill_attr(&fab, &nam, rsa, a);
    sys$close(&fab, 0, 0);
    if (r == 0 && dir)
        a->mode = (uint16_t)(S_IFDIR | (a->mode & 07777));
    return r;
}

/* ------------------------------------------------------------ open/close --- */

static int is_record_rfm(uint8_t rfm)
{
    return rfm == FAB$C_VAR || rfm == FAB$C_VFC || rfm == FAB$C_STM || rfm == FAB$C_STMCR;
}

static void rfile_free(struct rfile *rf)
{
    if (!rf)
        return;
    free(rf->rec);
    free(rf->io);
    free(rf);
}

/* Bind a new kernel descriptor number to rf. */
static long long rfd_bind(struct rfile *rf, int cloexec)
{
    long long k = rawsys(SYS_openat, AT_FDCWD, (long long)(uintptr_t)"/",
                         O_PATH | O_DIRECTORY | O_CLOEXEC, 0, 0);
    if (k < 0)
        return k;
    if (k >= RFD_MAX) {
        rawsys(SYS_close, k, 0, 0, 0, 0);
        return -EMFILE;
    }
    rfd[k] = rf;
    rfd_cloexec[k] = (unsigned char)(cloexec != 0);
    rf->refs++;
    return k;
}

static long long do_openat(long long dirfd, const char *path, long long flags,
                           int *handled)
{
    char spec[256];
    int dir = 0;
    int r = rms_spec(dirfd, path, spec, sizeof spec, &dir);
    if (r == -1)
        return 0;                               /* kernel namespace */
    *handled = 1;
    TR("crtlfd: openat rms flags", flags);
    if (r == -2)
        return -errno;
    if (dir || (flags & O_DIRECTORY))
        return -EOPNOTSUPP;                     /* directory streams: not yet */

    int acc = (int)(flags & O_ACCMODE);
    int writing = acc != O_RDONLY;

    struct rfile *rf = calloc(1, sizeof *rf);
    if (!rf)
        return -ENOMEM;
    rf->io = malloc(IOBUF);
    if (!rf->io) {
        rfile_free(rf);
        return -ENOMEM;
    }
    strncpy(rf->spec, spec, sizeof rf->spec - 1);
    rf->oflags = (int)(flags & (O_ACCMODE | O_APPEND | O_NONBLOCK));
    rf->fab = cc$rms_fab;
    rf->nam = cc$rms_nam;
    rf->fab.fab$l_fna = rf->spec;
    rf->fab.fab$b_fns = (uint8_t)strlen(rf->spec);
    rf->fab.fab$l_nam = &rf->nam;
    rf->nam.nam$l_rsa = rf->rsa;
    rf->nam.nam$b_rss = sizeof rf->rsa - 1;
    rf->nam.nam$l_esa = rf->esa;
    rf->nam.nam$b_ess = sizeof rf->esa - 1;

    uint32_t st = RMS$_FNF;
    int create = 0;
    if (!(flags & O_TRUNC) || !(flags & O_CREAT)) {
        /* Open an existing file (block + record access, so either mode works). */
        rf->fab.fab$b_fac = (uint8_t)(FAB$M_GET | FAB$M_BRO |
                                      (writing ? FAB$M_PUT | FAB$M_UPD : 0));
        rf->fab.fab$b_shr = writing ? 0 : FAB$M_SHRGET;
        st = sys$open(&rf->fab, 0, 0);
        TR("crtlfd: $open", st);
        if ((st & 1) && (flags & O_CREAT) && (flags & O_EXCL)) {
            sys$close(&rf->fab, 0, 0);
            rfile_free(rf);
            return -EEXIST;
        }
        if ((st & 1) && (flags & O_TRUNC)) {
            sys$close(&rf->fab, 0, 0);          /* truncate == a new version */
            st = RMS$_FNF;
            create = 1;
        }
    }
    if (!(st & 1)) {
        if (!(flags & O_CREAT) && !create) {
            int e = rms_errno(st);
            rfile_free(rf);
            return -(e ? e : EIO);
        }
        if (st != RMS$_FNF && !create && rms_errno(st) != ENOENT) {
            int e = rms_errno(st);
            rfile_free(rf);
            return -(e ? e : EIO);
        }
        /* $CREATE a new version: Stream_LF, carriage-return attributes. */
        rf->fab = cc$rms_fab;
        rf->fab.fab$l_fna = rf->spec;
        rf->fab.fab$b_fns = (uint8_t)strlen(rf->spec);
        rf->fab.fab$l_nam = &rf->nam;
        rf->fab.fab$b_org = FAB$C_SEQ;
        rf->fab.fab$b_rfm = FAB$C_STMLF;
        rf->fab.fab$b_rat = FAB$M_CR;
        rf->fab.fab$b_fac = FAB$M_GET | FAB$M_PUT | FAB$M_BIO;
        st = sys$create(&rf->fab, 0, 0);
        TR("crtlfd: $create", st);
        if (!(st & 1)) {
            int e = rms_errno(st);
            rfile_free(rf);
            return -(e ? e : EIO);
        }
    }
    rf->rsa[rf->nam.nam$b_rsl] = '\0';            /* rsa[256], rsl <= 255 */

    rf->kind = is_record_rfm(rf->fab.fab$b_rfm) ? RF_RECORD : RF_STREAM;
    if (rf->kind == RF_RECORD && writing) {
        sys$close(&rf->fab, 0, 0);
        rfile_free(rf);
        return -EOPNOTSUPP;                     /* writing a record file: not yet */
    }
    if (rf->kind == RF_STREAM) {
        struct rattr a;
        int ar = fill_attr(&rf->fab, &rf->nam, rf->rsa, &a);
        if (ar < 0) {                           /* no end of file: no stream */
            sys$close(&rf->fab, 0, 0);
            rfile_free(rf);
            return ar;
        }
        rf->eof = a.size;
    }
    rf->rab = cc$rms_rab;
    rf->rab.rab$l_fab = &rf->fab;
    st = sys$connect(&rf->rab, 0, 0);
    TR("crtlfd: $connect", st);
    if (!(st & 1)) {
        sys$close(&rf->fab, 0, 0);
        rfile_free(rf);
        return -EIO;
    }
    if (flags & O_APPEND)
        rf->pos = rf->eof;

    TR("crtlfd: opened kind", rf->kind);
    TR("crtlfd: eof", rf->eof);
    long long fd = rfd_bind(rf, (flags & O_CLOEXEC) != 0);
    TR("crtlfd: fd", fd);
    if (fd < 0) {
        sys$close(&rf->fab, 0, 0);
        rfile_free(rf);
    }
    return fd;
}

static void rfile_release(struct rfile *rf)
{
    if (--rf->refs > 0)
        return;
    sys$disconnect(&rf->rab, 0, 0);
    sys$close(&rf->fab, 0, 0);
    rfile_free(rf);
}

/* Drop descriptor fd's binding (the kernel placeholder is the caller's). */
static void rfd_unbind(long long fd)
{
    struct rfile *rf = rfd[fd];
    rfd[fd] = NULL;
    rfd_cloexec[fd] = 0;
    if (rf)
        rfile_release(rf);
}

/* --------------------------------------------------------------- I/O ------ */

static long long stream_read(struct rfile *rf, char *buf, uint64_t n)
{
    if (rf->pos >= rf->eof || n == 0)
        return 0;
    if (n > rf->eof - rf->pos)
        n = rf->eof - rf->pos;
    uint64_t done = 0;
    while (done < n) {
        uint64_t off = rf->pos % BLK;
        uint64_t want = off + (n - done);
        if (want > IOBUF)
            want = IOBUF;
        want = (want + BLK - 1) / BLK * BLK;
        rf->rab.rab$l_bkt = (uint32_t)(rf->pos / BLK) + 1u;
        rf->rab.rab$l_ubf = rf->io;
        rf->rab.rab$w_usz = (uint16_t)want;
        uint32_t st = sys$read(&rf->rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (!(st & 1))
            return done ? (long long)done : -EIO;
        uint64_t got = rf->rab.rab$w_rsz;
        if (got <= off)
            break;
        uint64_t take = got - off;
        if (take > n - done)
            take = n - done;
        memcpy(buf + done, rf->io + off, take);
        done += take;
        rf->pos += take;
        if (got < want)
            break;
    }
    return (long long)done;
}

/* One $WRITE of `len` bytes at block `vbn` from `src`. */
static int bio_write(struct rfile *rf, uint32_t vbn, const char *src, uint32_t len)
{
    rf->rab.rab$l_bkt = vbn;
    rf->rab.rab$l_rbf = (char *)src;
    rf->rab.rab$w_rsz = (uint16_t)len;
    return (sys$write(&rf->rab, 0, 0) & 1) ? 0 : -EIO;
}

static long long stream_write(struct rfile *rf, const char *buf, uint64_t n)
{
    if (rf->oflags & O_APPEND)
        rf->pos = rf->eof;
    uint64_t done = 0;
    while (done < n) {
        uint64_t pos = rf->pos;
        uint64_t off = pos % BLK;
        uint32_t vbn = (uint32_t)(pos / BLK) + 1u;
        uint64_t left = n - done;
        if (off == 0 && left >= BLK) {
            /* Whole blocks straight from the caller's buffer. */
            uint64_t take = left / BLK * BLK;
            if (take > IOBUF)
                take = IOBUF;
            if (bio_write(rf, vbn, buf + done, (uint32_t)take) < 0)
                return done ? (long long)done : -EIO;
            done += take;
            rf->pos += take;
        } else {
            /* A partial block: merge into what the block already holds. */
            uint64_t bstart = (uint64_t)(vbn - 1u) * BLK;
            uint64_t valid = rf->eof > bstart ? rf->eof - bstart : 0;
            if (valid > BLK)
                valid = BLK;
            memset(rf->io, 0, BLK);
            if (valid) {
                rf->rab.rab$l_bkt = vbn;
                rf->rab.rab$l_ubf = rf->io;
                rf->rab.rab$w_usz = BLK;
                uint32_t st = sys$read(&rf->rab, 0, 0);
                if (!(st & 1) && st != RMS$_EOF)
                    return done ? (long long)done : -EIO;
            }
            uint64_t take = BLK - off;
            if (take > left)
                take = left;
            memcpy(rf->io + off, buf + done, take);
            uint64_t end = off + take > valid ? off + take : valid;
            if (bio_write(rf, vbn, rf->io, (uint32_t)end) < 0)
                return done ? (long long)done : -EIO;
            done += take;
            rf->pos += take;
        }
        if (rf->pos > rf->eof)
            rf->eof = rf->pos;
    }
    return (long long)done;
}

/* Record files: the next record plus its newline, served a byte at a time. */
static long long record_read(struct rfile *rf, char *buf, uint64_t n)
{
    uint64_t done = 0;
    while (done < n) {
        if (rf->recoff >= rf->reclen) {
            if (rf->rec_eof)
                break;
            if (!rf->rec) {
                rf->rec = malloc(65536 + 1);
                if (!rf->rec)
                    return done ? (long long)done : -ENOMEM;
            }
            rf->rab.rab$l_ubf = rf->rec;
            rf->rab.rab$w_usz = 65535;
            uint32_t st = sys$get(&rf->rab, 0, 0);
            if (st == RMS$_EOF) {
                rf->rec_eof = 1;
                break;
            }
            if (!(st & 1))
                return done ? (long long)done : -EIO;
            rf->reclen = rf->rab.rab$w_rsz;
            if (rf->fab.fab$b_rat & (FAB$M_CR | FAB$M_PRN | FAB$M_FTN))
                rf->rec[rf->reclen++] = '\n';
            rf->recoff = 0;
            continue;
        }
        uint64_t take = rf->reclen - rf->recoff;
        if (take > n - done)
            take = n - done;
        memcpy(buf + done, rf->rec + rf->recoff, take);
        rf->recoff += take;
        rf->pos += take;
        done += take;
    }
    return (long long)done;
}

static long long r_read(struct rfile *rf, void *buf, uint64_t n)
{
    if ((rf->oflags & O_ACCMODE) == O_WRONLY)
        return -EBADF;
    return rf->kind == RF_STREAM ? stream_read(rf, buf, n) : record_read(rf, buf, n);
}

static long long r_write(struct rfile *rf, const void *buf, uint64_t n)
{
    if ((rf->oflags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (rf->kind != RF_STREAM)
        return -EOPNOTSUPP;
    return stream_write(rf, buf, n);
}

static long long r_lseek(struct rfile *rf, long long off, int whence)
{
    if (rf->kind == RF_RECORD) {
        if (whence == SEEK_CUR && off == 0)
            return (long long)rf->pos;
        if (whence == SEEK_SET && off == 0) {
            if (!(sys$rewind(&rf->rab, 0, 0) & 1))
                return -EIO;
            rf->pos = 0;
            rf->reclen = rf->recoff = 0;
            rf->rec_eof = 0;
            return 0;
        }
        return -ESPIPE;                          /* record files: not yet */
    }
    long long base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long long)rf->pos; break;
    case SEEK_END: base = (long long)rf->eof; break;
    default: return -EINVAL;
    }
    if (off < 0 && -off > base)
        return -EINVAL;
    rf->pos = (uint64_t)(base + off);
    return (long long)rf->pos;
}

static long long r_iov(struct rfile *rf, const struct iovec *iov, long long cnt,
                       int writing)
{
    long long total = 0;
    for (long long i = 0; i < cnt; i++) {
        if (!iov[i].iov_len)
            continue;
        long long r = writing ? r_write(rf, iov[i].iov_base, iov[i].iov_len)
                              : r_read(rf, iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len)
            break;
    }
    return total;
}

static long long r_pio(struct rfile *rf, void *buf, uint64_t n, long long off,
                       int writing)
{
    if (rf->kind != RF_STREAM)
        return -ESPIPE;
    uint64_t save = rf->pos;
    int app = rf->oflags & O_APPEND;
    rf->oflags &= ~O_APPEND;
    rf->pos = (uint64_t)off;
    long long r = writing ? r_write(rf, buf, n) : r_read(rf, buf, n);
    rf->pos = save;
    rf->oflags |= app;
    return r;
}

/* --------------------------------------------------------- descriptors ---- */

static long long r_dup_to(long long oldfd, long long newfd, long long flags)
{
    struct rfile *rf = rfd[oldfd];
    long long k = rawsys(SYS_dup3, oldfd, newfd, O_CLOEXEC, 0, 0);
    if (k < 0)
        return k;
    rfd[k] = rf;
    rfd_cloexec[k] = (unsigned char)((flags & O_CLOEXEC) != 0);
    rf->refs++;
    return k;
}

/* ------------------------------------------------------------- hook ------- */

static long long rms_hook_body(long long n, long long a1, long long a2,
                               long long a3, long long a4, long long a5,
                               int *handled)
{
    struct rfile *rf;
    if (rget(a1) || n == SYS_openat)
        TR("crtlfd: sys", n);
    switch (n) {
    case SYS_openat:
        return do_openat(a1, (const char *)(uintptr_t)a2, a3, handled);

    case SYS_read:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_read(rf, (void *)(uintptr_t)a2, (uint64_t)a3);
    case SYS_write:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_write(rf, (const void *)(uintptr_t)a2, (uint64_t)a3);
    case SYS_readv:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_iov(rf, (const struct iovec *)(uintptr_t)a2, a3, 0);
    case SYS_writev:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_iov(rf, (const struct iovec *)(uintptr_t)a2, a3, 1);
    case SYS_pread64:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_pio(rf, (void *)(uintptr_t)a2, (uint64_t)a3, a4, 0);
    case SYS_pwrite64:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_pio(rf, (void *)(uintptr_t)a2, (uint64_t)a3, a4, 1);
    case SYS_preadv:
    case SYS_pwritev:
        if (!rget(a1)) return 0;
        *handled = 1;
        return -EOPNOTSUPP;
    case SYS_lseek:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return r_lseek(rf, a2, (int)a3);

    case SYS_close:
        if (!rget(a1)) return 0;
        *handled = 1;
        rfd_unbind(a1);
        return rawsys(SYS_close, a1, 0, 0, 0, 0);
    case SYS_close_range: {
        if (a3 & 4)                             /* CLOSE_RANGE_CLOEXEC: no close */
            return 0;
        unsigned long long lo = (unsigned long long)a1, hi = (unsigned long long)a2;
        for (unsigned long long fd = lo; fd <= hi && fd < RFD_MAX; fd++)
            if (rfd[fd])
                rfd_unbind((long long)fd);
        return 0;                               /* the kernel closes the range */
    }
    case SYS_dup:
        if (!rget(a1)) return 0;
        *handled = 1;
        {
            long long k = rawsys(SYS_fcntl, a1, F_DUPFD_CLOEXEC, 0, 0, 0);
            if (k < 0) return k;
            if (k >= RFD_MAX) { rawsys(SYS_close, k, 0, 0, 0, 0); return -EMFILE; }
            rfd[k] = rfd[a1];
            rfd_cloexec[k] = 0;
            rfd[k]->refs++;
            return k;
        }
    case SYS_dup3:
        if (a1 == a2)
            return 0;                           /* the kernel answers EINVAL */
        if (a2 >= 0 && a2 < RFD_MAX && rfd[a2])
            rfd_unbind(a2);                     /* newfd's old file goes away */
        if (!rget(a1)) return 0;
        *handled = 1;
        return r_dup_to(a1, a2, a3);
    case SYS_fcntl:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        switch (a2) {
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            long long k = rawsys(SYS_fcntl, a1, F_DUPFD_CLOEXEC, a3, 0, 0);
            if (k < 0) return k;
            if (k >= RFD_MAX) { rawsys(SYS_close, k, 0, 0, 0, 0); return -EMFILE; }
            rfd[k] = rf;
            rfd_cloexec[k] = a2 == F_DUPFD_CLOEXEC;
            rf->refs++;
            return k;
        }
        case F_GETFD:
            return rfd_cloexec[a1] ? FD_CLOEXEC : 0;
        case F_SETFD:
            rfd_cloexec[a1] = (a3 & FD_CLOEXEC) != 0;
            return 0;
        case F_GETFL:
            return rf->oflags;
        case F_SETFL:
            rf->oflags = (rf->oflags & O_ACCMODE) | (int)(a3 & (O_APPEND | O_NONBLOCK));
            return 0;
        default:
            return -EINVAL;
        }
    case SYS_ioctl:
        if (!rget(a1)) return 0;
        *handled = 1;
        return -ENOTTY;
    case SYS_fsync:
    case SYS_fdatasync:
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        return (sys$flush(&rf->rab, 0, 0) & 1) ? 0 : -EIO;
    case SYS_ftruncate:
    case SYS_getdents64:
    case SYS_fchmod:
    case SYS_flock:
    case SYS_fallocate:
        if (!rget(a1)) return 0;
        *handled = 1;
        return n == SYS_getdents64 ? -ENOTDIR : -EOPNOTSUPP;

    case SYS_statx: {
        struct rattr a;
        const char *path = (const char *)(uintptr_t)a2;
        int r;
        if ((a3 & AT_EMPTY_PATH) && path && !*path) {
            if (!(rf = rget(a1))) return 0;
            *handled = 1;
            r = fill_attr(&rf->fab, &rf->nam, rf->rsa, &a);
            if (r == 0 && rf->kind == RF_STREAM)
                a.size = rf->eof;
        } else {
            char spec[256];
            int dir = 0;
            int s = rms_spec(a1, path, spec, sizeof spec, &dir);
            if (s == -1) return 0;
            *handled = 1;
            if (s == -2) return -errno;
            r = stat_path(spec, dir, &a);
        }
        if (r < 0)
            return r;
        attr_to_statx(&a, (struct ovmx_statx *)(uintptr_t)a5);
        return 0;
    }
    case SYS_fstat: {
        if (!(rf = rget(a1))) return 0;
        *handled = 1;
        struct rattr a;
        int r = fill_attr(&rf->fab, &rf->nam, rf->rsa, &a);
        if (r < 0) return r;
        if (rf->kind == RF_STREAM)
            a.size = rf->eof;
        attr_to_kstat(&a, (struct kstat *)(uintptr_t)a2);
        return 0;
    }
    case SYS_newfstatat: {
        const char *path = (const char *)(uintptr_t)a2;
        struct rattr a;
        int r;
        if ((a4 & AT_EMPTY_PATH) && path && !*path) {
            if (!(rf = rget(a1))) return 0;
            *handled = 1;
            r = fill_attr(&rf->fab, &rf->nam, rf->rsa, &a);
            if (r == 0 && rf->kind == RF_STREAM)
                a.size = rf->eof;
        } else {
            char spec[256];
            int dir = 0;
            int s = rms_spec(a1, path, spec, sizeof spec, &dir);
            if (s == -1) return 0;
            *handled = 1;
            if (s == -2) return -errno;
            r = stat_path(spec, dir, &a);
        }
        if (r < 0) return r;
        attr_to_kstat(&a, (struct kstat *)(uintptr_t)a3);
        return 0;
    }
    case SYS_faccessat:
    case SYS_faccessat2: {
        char spec[256];
        int dir = 0;
        int s = rms_spec(a1, (const char *)(uintptr_t)a2, spec, sizeof spec, &dir);
        if (s == -1) return 0;
        *handled = 1;
        if (s == -2) return -errno;
        struct rattr a;
        int r = stat_path(spec, dir, &a);
        if (r < 0) return r;
        /* The protection decides: the bits the owner/group/world class grant. */
        int want = (int)a3 & 7;
        if (want && (a.mode & (uint16_t)(want << 6)) != (uint16_t)(want << 6) &&
            (a.mode & (uint16_t)(want << 3)) != (uint16_t)(want << 3) &&
            (a.mode & (uint16_t)want) != (uint16_t)want)
            return -EACCES;
        return 0;
    }
    case SYS_unlinkat: {
        char spec[256];
        int dir = 0;
        int s = rms_spec(a1, (const char *)(uintptr_t)a2, spec, sizeof spec, &dir);
        if (s == -1) return 0;
        *handled = 1;
        if (s == -2) return -errno;
        if ((a3 & AT_REMOVEDIR) || dir)
            return -EOPNOTSUPP;                 /* rmdir: not yet */
        struct FAB fab = cc$rms_fab;
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        uint32_t st = sys$erase(&fab, 0, 0);
        return (st & 1) ? 0 : -(rms_errno(st) ? rms_errno(st) : EIO);
    }
    case SYS_renameat:
    case SYS_renameat2: {
        if (n == SYS_renameat2 && a5)
            return 0;                           /* flags: the kernel's EINVAL */
        char os[256], ns[256];
        int od = 0, nd = 0;
        int so = rms_spec(a1, (const char *)(uintptr_t)a2, os, sizeof os, &od);
        int sn = rms_spec(a3, (const char *)(uintptr_t)a4, ns, sizeof ns, &nd);
        if (so == -1 && sn == -1) return 0;
        *handled = 1;
        if (so == -1 || sn == -1) return -EXDEV;
        if (so == -2 || sn == -2) return -errno;
        struct FAB ofab = cc$rms_fab, nfab = cc$rms_fab;
        ofab.fab$l_fna = os;
        ofab.fab$b_fns = (uint8_t)strlen(os);
        nfab.fab$l_fna = ns;
        nfab.fab$b_fns = (uint8_t)strlen(ns);
        uint32_t st = sys$rename(&ofab, 0, 0, &nfab);
        return (st & 1) ? 0 : -(rms_errno(st) ? rms_errno(st) : EIO);
    }
    case SYS_mkdirat: {
        char spec[256];
        int s = rms_spec(a1, (const char *)(uintptr_t)a2, spec, sizeof spec, NULL);
        if (s == -1) return 0;
        *handled = 1;
        return s == -2 ? -errno : -EOPNOTSUPP;  /* mkdir: not yet */
    }
    case SYS_readlinkat: {
        char spec[256];
        int s = rms_spec(a1, (const char *)(uintptr_t)a2, spec, sizeof spec, NULL);
        if (s == -1) return 0;
        *handled = 1;
        return s == -2 ? -errno : -EINVAL;      /* RMS has no symbolic links */
    }
    default:
        return 0;
    }
}

static long long rms_hook(long long n, long long a1, long long a2, long long a3,
                          long long a4, long long a5, long long a6, int *handled)
{
    (void)a6;
    if (in_rms)
        return 0;                               /* RMS's own system calls */
    in_rms = 1;
    int saved = errno;
    long long r = rms_hook_body(n, a1, a2, a3, a4, a5, handled);
    errno = saved;
    in_rms = 0;
    return r;
}

/* ------------------------------------------------------------ install ----- */

/* decc$main is bound to this in the RMS-backed DECC$SHR (mk_decc_shr.sh): the
 * C RTL's per-image entry turns the RMS file layer on, then runs the C RTL's
 * own argument/environment setup. */
extern void ovmx_decc_main_crtl(void *, void *, void *, void *, unsigned int,
                                unsigned int, int *, int *, int *)
    __asm__("decc$main");

void ovmx_crtl_fd_main(void *progxfer, void *cli_util, void *imghdr,
                       void *image_file_desc, unsigned int linkflag,
                       unsigned int cliflag, int *argc, int *argv, int *envp)
{
    TR("crtlfd: main enter", 0);
    __ovmx_sys_hook = rms_hook;
    TR("crtlfd: hook installed", (uintptr_t)rms_hook);
    ovmx_decc_main_crtl(progxfer, cli_util, imghdr, image_file_desc, linkflag,
                        cliflag, argc, argv, envp);
    TR("crtlfd: decc$main returned", 0);
}
