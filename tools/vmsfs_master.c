/*
 * vmsfs_master.c - Master a populated VMSFS volume image from a source tree.
 *
 * This is the "mkfs -d" / mastering step of the OVMX build: it takes a Linux
 * source directory tree (e.g. distro/rootfs/vms/...) and writes it into a
 * VMSFS/ODS-2-inspired block image with the files and directories in place,
 * ready to be mounted by vmsfs.ko at boot.  It extends the empty-volume writer
 * in tools/vms_initialize.c (INITIALIZE.EXE) to populate content.
 *
 * It is factory BUILD tooling: never shipped on the media, never given a VMS
 * name, never run at boot (per docs/design-vms-faithful-install.md §3.3).
 * vms-8ab (the bootable distribution image) invokes the `master` mode to
 * produce dist/ovmx-distrib.img.
 *
 * Modes:
 *   vmsfs_master master  <image> <volume-label> <source-dir> [size-in-MB]
 *       Create <image> and populate it with the tree rooted at <source-dir>.
 *       The direct children of <source-dir> become the volume's root (MFD)
 *       entries.  Size is auto-computed from the tree when omitted.
 *
 *   vmsfs_master extract <image> <output-dir>
 *       Round-trip reader: walk the on-disk format and write every file and
 *       directory back out to <output-dir>, byte-for-byte.  Used to prove a
 *       mastered image is readable (master tree -> extract -> diff).
 *
 *   vmsfs_master list    <image>
 *       Print the directory tree stored in <image> (diagnostic).
 *
 * ----------------------------------------------------------------------------
 * Grounding (clean-room Rule 8):
 *   The on-disk byte layout is taken entirely from vmsfs_ondisk.h — the SAME
 *   header shared by vmsfs.ko and INITIALIZE.EXE, so the format has exactly one
 *   description.  The block-level semantics this tool writes to (home block at
 *   LBN 1, storage bitmap, one 512-byte file header per FID in the index area,
 *   88-byte directory entries with de_version==0 marking a directory) are what
 *   the kernel reader in src/kernel/vmsfs/vmsfs_blkdev.c consumes.
 *
 *   VMSFS is explicitly "not byte-compatible with real ODS-2" (vmsfs_ondisk.h
 *   header comment) — it is an OVMX design choice, not presented as
 *   VMS-authentic.  The mastering policy decisions this tool makes on top of
 *   that format — assigning FIDs in a deterministic DFS, laying each file/dir
 *   out contiguously, owning all mastered files as SYSTEM UIC [1,4] with the
 *   standard VMS default protection, and stamping every file version 1 — are
 *   OVMX build-tool choices, labeled as such here.
 * ----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "vmsfs_ondisk.h"

/* Mastering policy constants (OVMX build-tool choices, not VMS-authentic). */
#define MASTER_UIC_GROUP   1      /* SYSTEM group */
#define MASTER_UIC_MEMBER  4      /* SYSTEM member -> UIC [1,4] */
#define MASTER_FILE_VER    1      /* every mastered file is version ;1 */
#define MIN_BLOCKS         64     /* matches INITIALIZE.EXE floor */

/* ================================================================
 * In-memory tree
 * ================================================================ */

struct node {
    int          is_dir;
    char         name[VMSFS_NAME_MAX + 1];   /* component name (upcased) */
    char         type[VMSFS_TYPE_MAX + 1];   /* file type; "" for dirs */

    uint32_t     fid;
    uint16_t     version;                    /* 1 for files, 0 for dirs */

    /* file only */
    char        *host_path;
    uint64_t     size;

    /* directory only */
    struct node **children;
    size_t       nchild;
    size_t       cap;

    /* assigned during layout */
    uint32_t     data_lbn;                   /* start of contiguous run */
    uint32_t     data_blocks;                /* number of blocks in run */
};

static struct node *node_new(int is_dir)
{
    struct node *n = calloc(1, sizeof(*n));
    if (!n) { fprintf(stderr, "%%MASTER-F-NOMEM, out of memory\n"); exit(1); }
    n->is_dir = is_dir;
    return n;
}

static void node_add_child(struct node *dir, struct node *child)
{
    if (dir->nchild == dir->cap) {
        size_t nc = dir->cap ? dir->cap * 2 : 8;
        struct node **p = realloc(dir->children, nc * sizeof(*p));
        if (!p) { fprintf(stderr, "%%MASTER-F-NOMEM, out of memory\n"); exit(1); }
        dir->children = p;
        dir->cap = nc;
    }
    dir->children[dir->nchild++] = child;
}

/*
 * Split a Linux basename "STARTUP.COM" into VMS name/type components, upcased.
 * The last '.' separates name from type; no dot means an empty type.
 * Directories carry the whole basename as the name and an empty type.
 */
static int split_name_type(const char *base, int is_dir,
                           char *name, char *type)
{
    const char *dot = is_dir ? NULL : strrchr(base, '.');
    size_t nlen, tlen;
    size_t i;

    if (dot && dot != base) {
        nlen = (size_t)(dot - base);
        tlen = strlen(dot + 1);
    } else {
        nlen = strlen(base);
        tlen = 0;
    }

    if (nlen > VMSFS_NAME_MAX || tlen > VMSFS_TYPE_MAX) {
        fprintf(stderr, "%%MASTER-E-NAMLONG, name component too long: %s\n", base);
        return -1;
    }

    for (i = 0; i < nlen; i++) {
        char c = base[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        name[i] = c;
    }
    name[nlen] = '\0';

    for (i = 0; i < tlen; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') c -= 32;
        type[i] = c;
    }
    type[tlen] = '\0';
    return 0;
}

static int name_cmp(const void *a, const void *b)
{
    const struct node *na = *(const struct node * const *)a;
    const struct node *nb = *(const struct node * const *)b;
    int c = strcmp(na->name, nb->name);
    if (c) return c;
    return strcmp(na->type, nb->type);
}

/*
 * Recursively build the in-memory tree from a Linux directory.
 * Only regular files and directories are taken; anything else is skipped
 * with a warning (a system tree should contain neither symlinks nor devices).
 */
static struct node *build_tree(const char *host_dir)
{
    DIR *d = opendir(host_dir);
    if (!d) {
        fprintf(stderr, "%%MASTER-F-OPENDIR, cannot open %s: %s\n",
                host_dir, strerror(errno));
        exit(1);
    }

    struct node *dir = node_new(1);
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;   /* skip . and .. */

        char path[4096];
        int r = snprintf(path, sizeof(path), "%s/%s", host_dir, ent->d_name);
        if (r < 0 || r >= (int)sizeof(path)) {
            fprintf(stderr, "%%MASTER-E-PATHLONG, path too long under %s\n",
                    host_dir);
            continue;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "%%MASTER-W-STAT, cannot stat %s: %s\n",
                    path, strerror(errno));
            continue;
        }

        struct node *child;
        if (S_ISDIR(st.st_mode)) {
            child = build_tree(path);
            if (split_name_type(ent->d_name, 1, child->name, child->type) != 0)
                exit(1);
            child->version = 0;
        } else if (S_ISREG(st.st_mode)) {
            child = node_new(0);
            if (split_name_type(ent->d_name, 0, child->name, child->type) != 0)
                exit(1);
            child->version = MASTER_FILE_VER;
            child->host_path = strdup(path);
            child->size = (uint64_t)st.st_size;
        } else {
            fprintf(stderr, "%%MASTER-W-SKIP, skipping non-regular %s\n", path);
            continue;
        }
        node_add_child(dir, child);
    }
    closedir(d);

    /* Deterministic order so mastered images are reproducible. */
    if (dir->nchild > 1)
        qsort(dir->children, dir->nchild, sizeof(dir->children[0]), name_cmp);

    return dir;
}

/* ================================================================
 * Sizing and layout
 * ================================================================ */

static uint32_t blocks_for_bytes(uint64_t bytes)
{
    return (uint32_t)((bytes + VMSFS_BLOCK_SIZE - 1) / VMSFS_BLOCK_SIZE);
}

static uint32_t bitmap_blocks_needed(uint32_t total_blocks)
{
    uint32_t bits_per_block = VMSFS_BLOCK_SIZE * 8;
    return (total_blocks + bits_per_block - 1) / bits_per_block;
}

/* Count file headers the tree needs (dirs + files), excluding root MFD. */
static void count_headers(const struct node *dir, uint32_t *n)
{
    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        (*n)++;
        if (c->is_dir)
            count_headers(c, n);
    }
}

/* Data blocks the tree needs (dir entry blocks + file data blocks). */
static void count_data_blocks(const struct node *dir, uint32_t *n)
{
    /* This directory's own entry blocks: 5 entries per 512-byte block, min 1. */
    uint32_t entries = (uint32_t)dir->nchild;
    uint32_t dblocks = (entries + VMSFS_DIR_PER_BLOCK - 1) / VMSFS_DIR_PER_BLOCK;
    if (dblocks == 0) dblocks = 1;
    *n += dblocks;

    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        if (c->is_dir)
            count_data_blocks(c, n);
        else
            *n += blocks_for_bytes(c->size);   /* 0 blocks for empty files */
    }
}

/* Assign FIDs in DFS order; the caller seeds *next_fid at VMSFS_FID_FIRST_USER. */
static void assign_fids(struct node *dir, uint32_t *next_fid)
{
    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        c->fid = (*next_fid)++;
        if (c->is_dir)
            assign_fids(c, next_fid);
    }
}

/*
 * Assign contiguous data-block runs from a running allocator.  Directory
 * blocks and file blocks are laid out contiguously per node — the simple
 * "one retrieval pointer per file" case the reader handles best.
 */
static void assign_data(struct node *dir, uint32_t *next_lbn)
{
    uint32_t entries = (uint32_t)dir->nchild;
    uint32_t dblocks = (entries + VMSFS_DIR_PER_BLOCK - 1) / VMSFS_DIR_PER_BLOCK;
    if (dblocks == 0) dblocks = 1;
    dir->data_lbn = *next_lbn;
    dir->data_blocks = dblocks;
    *next_lbn += dblocks;

    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        if (c->is_dir) {
            assign_data(c, next_lbn);
        } else {
            uint32_t fb = blocks_for_bytes(c->size);
            c->data_lbn = fb ? *next_lbn : 0;
            c->data_blocks = fb;
            *next_lbn += fb;
        }
    }
}

/* ================================================================
 * Block writer
 * ================================================================ */

/*
 * master_build_time - the timestamp stamped into every mastered file's
 * fh_created/fh_modified/fh_accessed (write_header() applies one value to
 * every entry, not per-file real mtimes -- see build_tree() above, which
 * only carries st_size out of lstat(), never st_mtime).
 *
 * Reproducibility (vms-d73, tools/cut-release.sh): time(NULL) would make this
 * the one wall-clock difference between two otherwise byte-identical masters
 * of the same staged tree. SOURCE_DATE_EPOCH (Dockerfile.bootable exports it
 * from its own build-arg of the same name, default 0) is authoritative when
 * set; a real release build passes the release commit's timestamp.
 */
static time_t master_build_time(void)
{
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    if (sde && sde[0] != '\0') {
        char *end = NULL;
        long v = strtol(sde, &end, 10);
        if (end != sde && *end == '\0' && v >= 0)
            return (time_t)v;
    }
    return time(NULL);
}

static int wr_block(int fd, uint32_t lbn, const void *buf)
{
    off_t off = (off_t)lbn * VMSFS_BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, VMSFS_BLOCK_SIZE, off);
    if (n != VMSFS_BLOCK_SIZE) {
        fprintf(stderr, "%%MASTER-F-WRITE, write at LBN %u failed: %s\n",
                lbn, strerror(errno));
        return -1;
    }
    return 0;
}

static void bitmap_set(uint8_t *bm, uint32_t bit)
{
    bm[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

/* Write one file/dir header at its index slot. */
static int write_header(int fd, uint32_t index_lbn, const struct node *n,
                        uint32_t parent_fid, time_t now)
{
    struct vmsfs_file_header fh;
    memset(&fh, 0, sizeof(fh));

    fh.fh_magic      = VMSFS_FH_MAGIC;
    fh.fh_fid        = n->fid;
    fh.fh_created    = (uint64_t)now;
    fh.fh_modified   = (uint64_t)now;
    fh.fh_accessed   = (uint64_t)now;
    fh.fh_parent_fid = parent_fid;
    fh.fh_protection = VMSFS_PROT_DEFAULT;
    fh.fh_uic_group  = MASTER_UIC_GROUP;
    fh.fh_uic_member = MASTER_UIC_MEMBER;

    if (n->is_dir) {
        fh.fh_flags      = VMSFS_FH_INUSE | VMSFS_FH_DIRECTORY;
        fh.fh_version    = 0;
        fh.fh_size       = (uint64_t)n->data_blocks * VMSFS_BLOCK_SIZE;
        fh.fh_blocks     = n->data_blocks;
        fh.fh_link_count = 2;
        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = n->data_lbn;
        fh.fh_map[0].rp_count = n->data_blocks;
        strncpy(fh.fh_name, n->name, sizeof(fh.fh_name) - 1);
        strncpy(fh.fh_type, "DIR", sizeof(fh.fh_type) - 1);
    } else {
        fh.fh_flags      = VMSFS_FH_INUSE |
                           (n->data_blocks ? VMSFS_FH_CONTIGUOUS : 0);
        fh.fh_version    = n->version;
        fh.fh_size       = n->size;
        fh.fh_blocks     = n->data_blocks;
        fh.fh_link_count = 1;
        if (n->data_blocks) {
            fh.fh_map_count       = 1;
            fh.fh_map[0].rp_lbn   = n->data_lbn;
            fh.fh_map[0].rp_count = n->data_blocks;
        }
        strncpy(fh.fh_name, n->name, sizeof(fh.fh_name) - 1);
        strncpy(fh.fh_type, n->type, sizeof(fh.fh_type) - 1);
    }

    fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));
    return wr_block(fd, index_lbn + n->fid - 1, &fh);
}

/* Write a directory's data blocks: one 88-byte entry per child. */
static int write_dir_data(int fd, const struct node *dir)
{
    uint8_t *buf = calloc(dir->data_blocks, VMSFS_BLOCK_SIZE);
    if (!buf) { fprintf(stderr, "%%MASTER-F-NOMEM, out of memory\n"); return -1; }

    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        size_t blk = i / VMSFS_DIR_PER_BLOCK;
        size_t slot = i % VMSFS_DIR_PER_BLOCK;
        struct vmsfs_dir_entry *de =
            (struct vmsfs_dir_entry *)(buf + blk * VMSFS_BLOCK_SIZE
                                       + slot * VMSFS_DIR_ENTRY_SIZE);

        char full[sizeof(de->de_name)];
        if (c->is_dir || c->type[0] == '\0')
            snprintf(full, sizeof(full), "%s", c->name);
        else
            snprintf(full, sizeof(full), "%s.%s", c->name, c->type);

        de->de_fid      = c->fid;
        de->de_version  = c->version;   /* 0 for dirs, marks DT_DIR to reader */
        de->de_name_len = (uint8_t)strlen(full);
        de->de_flags    = 0;
        memcpy(de->de_name, full, strlen(full));
    }

    int rc = 0;
    for (uint32_t b = 0; b < dir->data_blocks; b++) {
        if (wr_block(fd, dir->data_lbn + b,
                     buf + (size_t)b * VMSFS_BLOCK_SIZE) < 0) {
            rc = -1;
            break;
        }
    }
    free(buf);
    return rc;
}

/* Copy a file's content into its data blocks, zero-padding the last block. */
static int write_file_data(int fd, const struct node *n)
{
    if (n->data_blocks == 0)
        return 0;

    int in = open(n->host_path, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "%%MASTER-F-OPENIN, cannot read %s: %s\n",
                n->host_path, strerror(errno));
        return -1;
    }

    uint8_t block[VMSFS_BLOCK_SIZE];
    uint64_t remaining = n->size;
    int rc = 0;
    for (uint32_t b = 0; b < n->data_blocks; b++) {
        memset(block, 0, sizeof(block));
        size_t want = remaining > VMSFS_BLOCK_SIZE
                          ? VMSFS_BLOCK_SIZE : (size_t)remaining;
        size_t got = 0;
        while (got < want) {
            ssize_t k = read(in, block + got, want - got);
            if (k < 0) {
                fprintf(stderr, "%%MASTER-F-READ, error reading %s: %s\n",
                        n->host_path, strerror(errno));
                rc = -1;
                goto done;
            }
            if (k == 0) break;
            got += (size_t)k;
        }
        remaining -= want;
        if (wr_block(fd, n->data_lbn + b, block) < 0) { rc = -1; goto done; }
    }
done:
    close(in);
    return rc;
}

/* Recursively write all headers and data for a subtree. */
static int emit_tree(int fd, uint32_t index_lbn, const struct node *dir,
                     time_t now)
{
    if (write_dir_data(fd, dir) < 0)
        return -1;

    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        if (write_header(fd, index_lbn, c, dir->fid, now) < 0)
            return -1;
        if (c->is_dir) {
            if (emit_tree(fd, index_lbn, c, now) < 0)
                return -1;
        } else {
            if (write_file_data(fd, c) < 0)
                return -1;
        }
    }
    return 0;
}

/* Mark every allocated block for a subtree in the in-memory bitmap. */
static void mark_tree_bitmap(uint8_t *bm, const struct node *dir)
{
    for (uint32_t b = 0; b < dir->data_blocks; b++)
        bitmap_set(bm, dir->data_lbn + b);

    for (size_t i = 0; i < dir->nchild; i++) {
        struct node *c = dir->children[i];
        if (c->is_dir) {
            mark_tree_bitmap(bm, c);
        } else {
            for (uint32_t b = 0; b < c->data_blocks; b++)
                bitmap_set(bm, c->data_lbn + b);
        }
    }
}

/* ================================================================
 * master mode
 * ================================================================ */

static int do_master(const char *image, const char *label,
                     const char *srcdir, long size_mb)
{
    size_t llen = strlen(label);
    if (llen == 0 || llen > 12) {
        fprintf(stderr, "%%MASTER-F-BADLABEL, volume label must be 1-12 chars\n");
        return 1;
    }

    /* ---- Build the tree; the MFD (FID 3) IS the source directory root. ---- */
    struct node *root = build_tree(srcdir);
    root->is_dir = 1;
    root->fid = VMSFS_FID_MFD;
    root->version = 0;
    strncpy(root->name, "000000", sizeof(root->name) - 1);

    /* ---- Sizing. ---- */
    uint32_t user_headers = 0;
    count_headers(root, &user_headers);
    uint32_t data_needed = 0;
    count_data_blocks(root, &data_needed);

    /* Index area: reserved 3 (INDEXF/BITMAP/MFD) + user headers, + headroom. */
    uint32_t needed_files = VMSFS_RESERVED_FIDS + user_headers;
    uint32_t max_files = needed_files + needed_files / 4 + 16;
    if (max_files > 65535) max_files = 65535;
    if (needed_files > max_files) {
        fprintf(stderr, "%%MASTER-F-TOOMANY, %u files exceed header cap\n",
                needed_files);
        return 1;
    }

    /* Converge total_blocks with the bitmap size that describes it. */
    uint32_t bm_blocks = 1;
    uint32_t total_blocks = 0;
    for (int iter = 0; iter < 8; iter++) {
        uint32_t fixed = 2 /*boot+home*/ + bm_blocks + max_files;
        uint32_t auto_total = fixed + data_needed;
        auto_total += auto_total / 10 + 16;   /* free-space headroom */
        total_blocks = auto_total;
        if (size_mb > 0) {
            uint64_t sz = (uint64_t)size_mb * 1024 * 1024;
            total_blocks = (uint32_t)(sz / VMSFS_BLOCK_SIZE);
        }
        uint32_t nb = bitmap_blocks_needed(total_blocks);
        if (nb == bm_blocks) break;
        bm_blocks = nb;
    }

    uint32_t bm_lbn   = VMSFS_BITMAP_LBN;         /* 2 */
    uint32_t idx_lbn  = bm_lbn + bm_blocks;
    uint32_t data_lbn = idx_lbn + max_files;

    if (total_blocks < MIN_BLOCKS || data_lbn + data_needed > total_blocks) {
        fprintf(stderr,
                "%%MASTER-F-TOOSMALL, volume too small: need >= %u blocks, have %u\n",
                data_lbn + data_needed, total_blocks);
        return 1;
    }

    /* ---- Assign FIDs and data runs. ---- */
    uint32_t next_fid = VMSFS_FID_FIRST_USER;
    assign_fids(root, &next_fid);

    uint32_t next_lbn = data_lbn;
    assign_data(root, &next_lbn);      /* fills root->data_lbn first */
    uint32_t data_end = next_lbn;      /* first free block after all data */

    /* ---- Create and size the image. ---- */
    int fd = open(image, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fprintf(stderr, "%%MASTER-F-CREATE, cannot create %s: %s\n",
                image, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)total_blocks * VMSFS_BLOCK_SIZE) < 0) {
        fprintf(stderr, "%%MASTER-F-TRUNC, cannot size %s: %s\n",
                image, strerror(errno));
        close(fd);
        return 1;
    }

    time_t now = master_build_time();
    uint8_t zero[VMSFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));

    int rc = 0;

    /* Boot block. */
    if (wr_block(fd, VMSFS_BOOT_LBN, zero) < 0) { rc = 1; goto out; }

    /* Zero the whole index area. */
    for (uint32_t i = 0; i < max_files; i++)
        if (wr_block(fd, idx_lbn + i, zero) < 0) { rc = 1; goto out; }

    /* Reserved FID 1: INDEXF.SYS (the index area itself). */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));
        fh.fh_magic = VMSFS_FH_MAGIC;
        fh.fh_fid = VMSFS_FID_INDEXF;
        fh.fh_flags = VMSFS_FH_INUSE | VMSFS_FH_CONTIGUOUS;
        fh.fh_version = 1;
        fh.fh_size = (uint64_t)max_files * VMSFS_BLOCK_SIZE;
        fh.fh_blocks = max_files;
        fh.fh_protection = 0xFF00;
        fh.fh_uic_group = MASTER_UIC_GROUP;
        fh.fh_uic_member = MASTER_UIC_MEMBER;
        fh.fh_link_count = 1;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created = fh.fh_modified = (uint64_t)now;
        strncpy(fh.fh_name, "INDEXF", sizeof(fh.fh_name) - 1);
        strncpy(fh.fh_type, "SYS", sizeof(fh.fh_type) - 1);
        fh.fh_map_count = 1;
        fh.fh_map[0].rp_lbn = idx_lbn;
        fh.fh_map[0].rp_count = max_files;
        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));
        if (wr_block(fd, idx_lbn + VMSFS_FID_INDEXF - 1, &fh) < 0) { rc = 1; goto out; }
    }

    /* Reserved FID 2: BITMAP.SYS. */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));
        fh.fh_magic = VMSFS_FH_MAGIC;
        fh.fh_fid = VMSFS_FID_BITMAP;
        fh.fh_flags = VMSFS_FH_INUSE | VMSFS_FH_CONTIGUOUS;
        fh.fh_version = 1;
        fh.fh_size = (uint64_t)bm_blocks * VMSFS_BLOCK_SIZE;
        fh.fh_blocks = bm_blocks;
        fh.fh_protection = 0xFF00;
        fh.fh_uic_group = MASTER_UIC_GROUP;
        fh.fh_uic_member = MASTER_UIC_MEMBER;
        fh.fh_link_count = 1;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created = fh.fh_modified = (uint64_t)now;
        strncpy(fh.fh_name, "BITMAP", sizeof(fh.fh_name) - 1);
        strncpy(fh.fh_type, "SYS", sizeof(fh.fh_type) - 1);
        fh.fh_map_count = 1;
        fh.fh_map[0].rp_lbn = bm_lbn;
        fh.fh_map[0].rp_count = bm_blocks;
        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));
        if (wr_block(fd, idx_lbn + VMSFS_FID_BITMAP - 1, &fh) < 0) { rc = 1; goto out; }
    }

    /* Reserved FID 3: 000000.DIR (the root MFD). */
    if (write_header(fd, idx_lbn, root, VMSFS_FID_MFD, now) < 0) { rc = 1; goto out; }

    /* All user headers and file/dir data. */
    if (emit_tree(fd, idx_lbn, root, now) < 0) { rc = 1; goto out; }

    /* ---- Storage bitmap. ---- */
    {
        uint32_t bm_bytes = bm_blocks * VMSFS_BLOCK_SIZE;
        uint8_t *bm = calloc(1, bm_bytes);
        if (!bm) { fprintf(stderr, "%%MASTER-F-NOMEM, out of memory\n"); rc = 1; goto out; }

        /* Metadata: boot, home, bitmap, entire index area. */
        for (uint32_t i = 0; i < data_lbn; i++)
            bitmap_set(bm, i);
        /* All allocated data blocks. */
        mark_tree_bitmap(bm, root);
        /* Blocks past the volume end (bitmap padding). */
        uint32_t cap = bm_blocks * VMSFS_BLOCK_SIZE * 8;
        for (uint32_t i = total_blocks; i < cap; i++)
            bitmap_set(bm, i);

        for (uint32_t i = 0; i < bm_blocks; i++)
            if (wr_block(fd, bm_lbn + i, bm + (size_t)i * VMSFS_BLOCK_SIZE) < 0) {
                free(bm); rc = 1; goto out;
            }
        free(bm);
    }

    /* ---- Home block. ---- */
    {
        uint32_t allocated = data_lbn + (data_end - data_lbn);
        uint32_t free_blocks = total_blocks - allocated;

        struct vmsfs_home_block hb;
        memset(&hb, 0, sizeof(hb));
        hb.hb_magic = VMSFS_HOME_MAGIC;
        hb.hb_format_ver = VMSFS_FORMAT_VERSION;
        hb.hb_created = hb.hb_modified = (uint64_t)now;
        hb.hb_block_size = VMSFS_BLOCK_SIZE;
        hb.hb_total_blocks = total_blocks;
        hb.hb_free_blocks = free_blocks;
        hb.hb_bitmap_lbn = bm_lbn;
        hb.hb_bitmap_blocks = bm_blocks;
        hb.hb_index_lbn = idx_lbn;
        hb.hb_max_files = max_files;
        hb.hb_data_lbn = data_lbn;
        hb.hb_cluster_size = 1;
        hb.hb_protection = 0;
        hb.hb_default_prot = VMSFS_PROT_DEFAULT;

        memset(hb.hb_volname, ' ', sizeof(hb.hb_volname));
        for (size_t i = 0; i < llen; i++) {
            char c = label[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            hb.hb_volname[i] = c;
        }
        hb.hb_checksum = vmsfs_checksum(&hb, sizeof(hb));
        if (wr_block(fd, VMSFS_HOME_LBN, &hb) < 0) { rc = 1; goto out; }

        printf("%%MASTER-I-DONE, mastered %s (volume %.12s)\n", image, hb.hb_volname);
        printf("%%MASTER-I-LAYOUT, %u blocks, %u bitmap, %u headers (%u used), data@%u\n",
               total_blocks, bm_blocks, max_files, needed_files, data_lbn);
        printf("%%MASTER-I-CONTENT, %u files/dirs, %u data blocks, %u free blocks\n",
               user_headers, data_end - data_lbn, free_blocks);
    }

out:
    if (fsync(fd) < 0)
        fprintf(stderr, "%%MASTER-W-FSYNC, sync warning: %s\n", strerror(errno));
    close(fd);
    return rc;
}

/* ================================================================
 * Reader (extract / list)
 * ================================================================ */

struct reader {
    int fd;
    struct vmsfs_home_block hb;
};

static int rd_block(int fd, uint32_t lbn, void *buf)
{
    off_t off = (off_t)lbn * VMSFS_BLOCK_SIZE;
    ssize_t n = pread(fd, buf, VMSFS_BLOCK_SIZE, off);
    if (n != VMSFS_BLOCK_SIZE) {
        fprintf(stderr, "%%MASTER-F-READ, read at LBN %u failed\n", lbn);
        return -1;
    }
    return 0;
}

static int reader_open(struct reader *r, const char *image)
{
    r->fd = open(image, O_RDONLY);
    if (r->fd < 0) {
        fprintf(stderr, "%%MASTER-F-OPENIN, cannot open %s: %s\n",
                image, strerror(errno));
        return -1;
    }
    off_t off = (off_t)VMSFS_HOME_LBN * VMSFS_BLOCK_SIZE;
    if (pread(r->fd, &r->hb, sizeof(r->hb), off) != (ssize_t)sizeof(r->hb) ||
        r->hb.hb_magic != VMSFS_HOME_MAGIC) {
        fprintf(stderr, "%%MASTER-F-NOTVMSFS, %s is not a VMSFS volume\n", image);
        close(r->fd);
        return -1;
    }
    return 0;
}

static int read_header(struct reader *r, uint32_t fid, struct vmsfs_file_header *fh)
{
    if (fid == 0 || fid > r->hb.hb_max_files)
        return -1;
    uint32_t lbn = r->hb.hb_index_lbn + fid - 1;
    if (rd_block(r->fd, lbn, fh) < 0)
        return -1;
    if (fh->fh_magic != VMSFS_FH_MAGIC) {
        fprintf(stderr, "%%MASTER-E-BADFH, FID %u has bad magic\n", fid);
        return -1;
    }
    uint32_t want = fh->fh_checksum;
    uint32_t got = vmsfs_checksum(fh, sizeof(*fh));
    if (want != got) {
        fprintf(stderr, "%%MASTER-E-CHKSUM, FID %u header checksum mismatch\n", fid);
        return -1;
    }
    if (!(fh->fh_flags & VMSFS_FH_INUSE)) {
        fprintf(stderr, "%%MASTER-E-NOTINUSE, FID %u not in use\n", fid);
        return -1;
    }
    return 0;
}

/* Translate 1-based VBN to LBN via the header's retrieval pointers. */
static int vbn_to_lbn(const struct vmsfs_file_header *fh, uint32_t vbn,
                      uint32_t *lbn_out)
{
    uint32_t cur = 1;
    for (uint16_t i = 0; i < fh->fh_map_count && i < VMSFS_MAX_RETRIEVAL; i++) {
        uint32_t cnt = fh->fh_map[i].rp_count;
        if (vbn >= cur && vbn < cur + cnt) {
            *lbn_out = fh->fh_map[i].rp_lbn + (vbn - cur);
            return 0;
        }
        cur += cnt;
    }
    return -1;
}

/* Reconstruct the Linux basename from a header's name/type. */
static void header_basename(const struct vmsfs_file_header *fh, char *out, size_t n)
{
    char name[41], type[41];
    memcpy(name, fh->fh_name, 40); name[40] = '\0';
    memcpy(type, fh->fh_type, 40); type[40] = '\0';
    if (fh->fh_flags & VMSFS_FH_DIRECTORY)
        snprintf(out, n, "%s", name);
    else if (type[0] == '\0')
        snprintf(out, n, "%s", name);
    else
        snprintf(out, n, "%s.%s", name, type);
}

/* Walk a directory FID; for each entry call visit(). */
typedef int (*visit_fn)(struct reader *r, uint32_t child_fid,
                        const struct vmsfs_file_header *child_fh, void *ctx);

static int walk_dir(struct reader *r, const struct vmsfs_file_header *dir_fh,
                    visit_fn visit, void *ctx)
{
    uint8_t block[VMSFS_BLOCK_SIZE];
    for (uint32_t vbn = 1; ; vbn++) {
        uint32_t lbn;
        if (vbn_to_lbn(dir_fh, vbn, &lbn) != 0)
            break;
        if (rd_block(r->fd, lbn, block) < 0)
            return -1;
        for (unsigned j = 0; j < VMSFS_DIR_PER_BLOCK; j++) {
            struct vmsfs_dir_entry *de =
                (struct vmsfs_dir_entry *)(block + j * VMSFS_DIR_ENTRY_SIZE);
            if (de->de_fid == 0)
                continue;
            struct vmsfs_file_header cfh;
            if (read_header(r, de->de_fid, &cfh) < 0)
                return -1;
            if (visit(r, de->de_fid, &cfh, ctx) < 0)
                return -1;
        }
    }
    return 0;
}

/* ---- extract ---- */

struct extract_ctx { char dir[4096]; };

static int extract_file(struct reader *r, const struct vmsfs_file_header *fh,
                        const char *path)
{
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "%%MASTER-F-CREATE, cannot write %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    uint8_t block[VMSFS_BLOCK_SIZE];
    uint64_t remaining = fh->fh_size;
    int rc = 0;
    for (uint32_t vbn = 1; remaining > 0; vbn++) {
        uint32_t lbn;
        if (vbn_to_lbn(fh, vbn, &lbn) != 0) {
            fprintf(stderr, "%%MASTER-E-SHORT, FID %u short of data at VBN %u\n",
                    fh->fh_fid, vbn);
            rc = -1;
            break;
        }
        if (rd_block(r->fd, lbn, block) < 0) { rc = -1; break; }
        size_t w = remaining > VMSFS_BLOCK_SIZE ? VMSFS_BLOCK_SIZE : (size_t)remaining;
        if (write(out, block, w) != (ssize_t)w) {
            fprintf(stderr, "%%MASTER-F-WRITE, error writing %s\n", path);
            rc = -1;
            break;
        }
        remaining -= w;
    }
    close(out);
    return rc;
}

static int extract_visit(struct reader *r, uint32_t child_fid,
                         const struct vmsfs_file_header *cfh, void *vctx)
{
    (void)child_fid;
    struct extract_ctx *ctx = vctx;
    char base[128];
    header_basename(cfh, base, sizeof(base));

    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", ctx->dir, base) >= (int)sizeof(path)) {
        fprintf(stderr, "%%MASTER-E-PATHLONG, path too long: %s/%s\n", ctx->dir, base);
        return -1;
    }

    if (cfh->fh_flags & VMSFS_FH_DIRECTORY) {
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "%%MASTER-F-MKDIR, cannot create %s: %s\n",
                    path, strerror(errno));
            return -1;
        }
        struct extract_ctx sub;
        snprintf(sub.dir, sizeof(sub.dir), "%s", path);
        return walk_dir(r, cfh, extract_visit, &sub);
    }
    return extract_file(r, cfh, path);
}

static int do_extract(const char *image, const char *outdir)
{
    struct reader r;
    if (reader_open(&r, image) < 0)
        return 1;
    if (mkdir(outdir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "%%MASTER-F-MKDIR, cannot create %s: %s\n",
                outdir, strerror(errno));
        close(r.fd);
        return 1;
    }
    struct vmsfs_file_header mfd;
    if (read_header(&r, VMSFS_FID_MFD, &mfd) < 0) { close(r.fd); return 1; }

    struct extract_ctx ctx;
    snprintf(ctx.dir, sizeof(ctx.dir), "%s", outdir);
    int rc = walk_dir(&r, &mfd, extract_visit, &ctx);
    close(r.fd);
    if (rc == 0)
        printf("%%MASTER-I-EXTRACT, extracted %s -> %s\n", image, outdir);
    return rc == 0 ? 0 : 1;
}

/* ---- list ---- */

struct list_ctx { int depth; };

static int list_visit(struct reader *r, uint32_t child_fid,
                      const struct vmsfs_file_header *cfh, void *vctx)
{
    struct list_ctx *ctx = vctx;
    char base[128];
    header_basename(cfh, base, sizeof(base));
    for (int i = 0; i < ctx->depth; i++) printf("  ");
    if (cfh->fh_flags & VMSFS_FH_DIRECTORY) {
        printf("[%s]  (FID %u)\n", base, child_fid);
        struct list_ctx sub = { ctx->depth + 1 };
        return walk_dir(r, cfh, list_visit, &sub);
    }
    printf("%s;%u  (FID %u, %llu bytes)\n", base, cfh->fh_version, child_fid,
           (unsigned long long)cfh->fh_size);
    return 0;
}

static int do_list(const char *image)
{
    struct reader r;
    if (reader_open(&r, image) < 0)
        return 1;
    char vol[13];
    memcpy(vol, r.hb.hb_volname, 12); vol[12] = '\0';
    for (int i = 11; i >= 0 && vol[i] == ' '; i--) vol[i] = '\0';
    printf("Volume %s: %u blocks, %u free, %u max files\n",
           vol, r.hb.hb_total_blocks, r.hb.hb_free_blocks, r.hb.hb_max_files);
    struct vmsfs_file_header mfd;
    if (read_header(&r, VMSFS_FID_MFD, &mfd) < 0) { close(r.fd); return 1; }
    struct list_ctx ctx = { 0 };
    int rc = walk_dir(&r, &mfd, list_visit, &ctx);
    close(r.fd);
    return rc == 0 ? 0 : 1;
}

/* ================================================================
 * main
 * ================================================================ */

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  vmsfs_master master  <image> <volume-label> <source-dir> [size-in-MB]\n"
        "  vmsfs_master extract <image> <output-dir>\n"
        "  vmsfs_master list    <image>\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(); return 1; }

    if (strcmp(argv[1], "master") == 0) {
        if (argc < 5 || argc > 6) { usage(); return 1; }
        long size_mb = 0;
        if (argc == 6) {
            size_mb = strtol(argv[5], NULL, 10);
            if (size_mb <= 0) {
                fprintf(stderr, "%%MASTER-F-BADSIZE, size must be positive MB\n");
                return 1;
            }
        }
        return do_master(argv[2], argv[3], argv[4], size_mb);
    } else if (strcmp(argv[1], "extract") == 0) {
        if (argc != 4) { usage(); return 1; }
        return do_extract(argv[2], argv[3]);
    } else if (strcmp(argv[1], "list") == 0) {
        if (argc != 3) { usage(); return 1; }
        return do_list(argv[2]);
    }

    usage();
    return 1;
}
