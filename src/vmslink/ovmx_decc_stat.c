/*
 * ovmx_decc_stat.c - the DEC C RTL's struct stat (vms-28d part 2).
 *
 * A DEC C client's default struct stat (musl-arch bits/stat.h, unless it
 * defines _USE_STD_STAT) is the DEC C RTL layout: st_ino is the three-word File
 * ID and st_fab_rfm/rat/fsz/mrs carry the file's RMS record attributes. Its
 * stat/fstat/lstat/fstatat are bound to the decc$$ entries below, which take
 * the C RTL's own (X/Open) answer and translate it:
 *
 *   st_ino[0..2]  the File ID: number (with its NMX extension in the high
 *                 word), sequence, relative volume -- from the RMS file
 *                 layer's st_ino packing (src/vmsrms/crtl_rms_fd.c); a file of
 *                 the substrate kernel's own namespaces (/dev, /proc ...) has
 *                 its inode number in word 0 and zeros after it.
 *   st_fab_*      the record format/attributes/fixed-control size/maximum
 *                 record size from the file's header, supplied by the RMS file
 *                 layer through __ovmx_crtl_fab_query when the RMS-backed
 *                 DECC$SHR runs it; a kernel file is a byte stream with no
 *                 record structure and reports FAB$C_UDF with no attributes.
 *
 * Everything else is the X/Open value unchanged.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

/* The DEC C layout, from the same field list the client header uses. */
struct ovmx_decc_stat {
    __OVMX_DECC_STAT_FIELDS
};

/* RMS record attributes of a file, as the RMS file layer answers them. */
struct ovmx_fab_attrs {
    unsigned char rfm, rat, fsz;
    unsigned      mrs;
};

/* Installed by the RMS file layer (crtl_rms_fd.c) in the RMS-backed DECC$SHR:
 * answer the RMS attributes of descriptor `fd` (path NULL) or of `path`
 * relative to `dirfd`. 0 and *out filled for an RMS file; -1 otherwise. NULL
 * in the bootstrap DECC$SHR, where every file is the kernel's. */
int (*__ovmx_crtl_fab_query)(int fd, int dirfd, const char *path,
                             struct ovmx_fab_attrs *out) = 0;

static void to_decc(struct ovmx_decc_stat *d, const struct stat *s,
                    const struct ovmx_fab_attrs *fa, int rms)
{
    memset(d, 0, sizeof *d);
    d->st_dev = s->st_dev;
    if (rms) {
        uint64_t ino = (uint64_t)s->st_ino;
        d->st_ino[0] = (ino_t)((ino & 0xFFFFu) | (((ino >> 40) & 0xFFu) << 16));
        d->st_ino[1] = (ino_t)((ino >> 16) & 0xFFFFu);
        d->st_ino[2] = (ino_t)((ino >> 32) & 0xFFu);
        d->st_fab_rfm = (char)fa->rfm;
        d->st_fab_rat = (char)fa->rat;
        d->st_fab_fsz = (char)fa->fsz;
        d->st_fab_mrs = fa->mrs;
    } else {
        d->st_ino[0] = s->st_ino;
    }
    d->st_mode = s->st_mode;
    d->st_nlink = s->st_nlink;
    d->st_uid = s->st_uid;
    d->st_gid = s->st_gid;
    d->st_rdev = s->st_rdev;
    d->st_size = s->st_size;
    d->st_atim = s->st_atim;
    d->st_mtim = s->st_mtim;
    d->st_ctim = s->st_ctim;
    d->st_blksize = s->st_blksize;
    d->st_blocks = s->st_blocks;
}

static int query(int fd, int dirfd, const char *path, struct ovmx_fab_attrs *fa)
{
    memset(fa, 0, sizeof *fa);
    if (!__ovmx_crtl_fab_query)
        return 0;
    int saved = errno;
    int r = __ovmx_crtl_fab_query(fd, dirfd, path, fa);
    errno = saved;
    return r == 0;
}

int ovmx_decc_stat(const char *path, struct ovmx_decc_stat *st) __asm__("decc$$stat");
int ovmx_decc_lstat(const char *path, struct ovmx_decc_stat *st) __asm__("decc$$lstat");
int ovmx_decc_fstat(int fd, struct ovmx_decc_stat *st) __asm__("decc$$fstat");
int ovmx_decc_fstatat(int dirfd, const char *path, struct ovmx_decc_stat *st, int flag)
    __asm__("decc$$fstatat");

int ovmx_decc_fstatat(int dirfd, const char *path, struct ovmx_decc_stat *st, int flag)
{
    struct stat s;
    struct ovmx_fab_attrs fa;
    if (!st) {
        errno = EFAULT;
        return -1;
    }
    if (fstatat(dirfd, path, &s, flag) != 0)
        return -1;
    int rms = (flag & AT_EMPTY_PATH) && path && !*path
                  ? query(dirfd, AT_FDCWD, NULL, &fa)
                  : query(-1, dirfd, path, &fa);
    to_decc(st, &s, &fa, rms);
    return 0;
}

int ovmx_decc_stat(const char *path, struct ovmx_decc_stat *st)
{
    return ovmx_decc_fstatat(AT_FDCWD, path, st, 0);
}

int ovmx_decc_lstat(const char *path, struct ovmx_decc_stat *st)
{
    return ovmx_decc_fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int ovmx_decc_fstat(int fd, struct ovmx_decc_stat *st)
{
    return ovmx_decc_fstatat(fd, "", st, AT_EMPTY_PATH);
}
