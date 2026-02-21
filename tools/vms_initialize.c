/*
 * vms_initialize.c - INITIALIZE.EXE: format a device or image with VMSFS
 *
 * VMS equivalent: INITIALIZE /SYSTEM DKA0: OVMX
 *
 * Creates the on-disk structures for a VMSFS volume:
 *   Block 0:       Boot block (zeroed)
 *   Block 1:       Home block (superblock)
 *   Blocks 2..N:   Storage bitmap
 *   Blocks N+1..M: File header area (INDEXF.SYS, BITMAP.SYS, 000000.DIR)
 *   Blocks M+1..:  Data area
 *
 * Usage: INITIALIZE <device-or-file> <volume-label> [size-in-MB]
 *   If the target is a regular file and doesn't exist, size-in-MB is required.
 *   If the target is a block device, size is determined automatically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>   /* BLKGETSIZE64 */

#include "vmsfs_ondisk.h"

#define MAX_VOLNAME 12

/* Minimum volume: boot + home + 1 bitmap + 16 headers + some data */
#define MIN_BLOCKS  64

/*
 * Compute the number of bitmap blocks needed to track total_blocks.
 * Each bitmap block covers VMSFS_BLOCK_SIZE * 8 blocks (one bit per block).
 */
static uint32_t bitmap_blocks_needed(uint32_t total_blocks)
{
    uint32_t bits_per_block = VMSFS_BLOCK_SIZE * 8;
    return (total_blocks + bits_per_block - 1) / bits_per_block;
}

/*
 * Write a block-sized buffer to a specific LBN on the volume.
 */
static int write_block(int fd, uint32_t lbn, const void *buf)
{
    off_t offset = (off_t)lbn * VMSFS_BLOCK_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset) {
        perror("%INIT-F-SEEKERR, seek failed");
        return -1;
    }
    ssize_t n = write(fd, buf, VMSFS_BLOCK_SIZE);
    if (n != VMSFS_BLOCK_SIZE) {
        perror("%INIT-F-WRITERR, write failed");
        return -1;
    }
    return 0;
}

/*
 * Set a bit in a bitmap buffer (bit index is the LBN).
 */
static void bitmap_set(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

/*
 * Format the volume.
 */
static int format_volume(int fd, uint64_t size_bytes, const char *volname)
{
    uint32_t total_blocks = (uint32_t)(size_bytes / VMSFS_BLOCK_SIZE);

    if (total_blocks < MIN_BLOCKS) {
        fprintf(stderr, "%%INIT-F-TOOSMALL, volume too small (%u blocks, need %d)\n",
                total_blocks, MIN_BLOCKS);
        return -1;
    }

    /* Calculate layout */
    uint32_t bm_blocks = bitmap_blocks_needed(total_blocks);
    uint32_t bm_lbn = VMSFS_BITMAP_LBN;       /* bitmap starts at block 2 */
    uint32_t idx_lbn = bm_lbn + bm_blocks;     /* index file follows bitmap */

    /* Allocate ~1% of volume for file headers, minimum 16, cap at 65535 */
    uint32_t max_files = total_blocks / 100;
    if (max_files < 16)    max_files = 16;
    if (max_files > 65535) max_files = 65535;

    uint32_t data_lbn = idx_lbn + max_files;    /* data area follows index */

    if (data_lbn >= total_blocks) {
        fprintf(stderr, "%%INIT-F-TOOSMALL, volume too small for metadata\n");
        return -1;
    }

    uint32_t free_blocks = total_blocks;        /* will subtract allocated */
    time_t now = time(NULL);

    printf("%%INIT-I-FORMAT, formatting %s\n", volname);
    printf("%%INIT-I-LAYOUT, %u blocks, %u bitmap, %u headers, data at %u\n",
           total_blocks, bm_blocks, max_files, data_lbn);

    /* ---- Block 0: Boot block (zeroed) ---- */
    {
        uint8_t boot[VMSFS_BLOCK_SIZE];
        memset(boot, 0, sizeof(boot));
        if (write_block(fd, VMSFS_BOOT_LBN, boot) < 0) return -1;
    }

    /* ---- Allocate and write bitmap ---- */
    /* Bitmap is built in memory, then written block by block */
    uint32_t bitmap_bytes = bm_blocks * VMSFS_BLOCK_SIZE;
    uint8_t *bitmap = calloc(1, bitmap_bytes);
    if (!bitmap) {
        fprintf(stderr, "%%INIT-F-NOMEM, cannot allocate bitmap\n");
        return -1;
    }

    /* Mark metadata blocks as allocated: boot, home, bitmap, index area */
    uint32_t metadata_end = data_lbn;
    for (uint32_t i = 0; i < metadata_end; i++) {
        bitmap_set(bitmap, i);
    }
    free_blocks -= metadata_end;

    /* Mark blocks beyond volume end as allocated (if bitmap covers more) */
    uint32_t bitmap_capacity = bm_blocks * VMSFS_BLOCK_SIZE * 8;
    for (uint32_t i = total_blocks; i < bitmap_capacity; i++) {
        bitmap_set(bitmap, i);
    }

    /* Write bitmap blocks */
    for (uint32_t i = 0; i < bm_blocks; i++) {
        if (write_block(fd, bm_lbn + i,
                        bitmap + (i * VMSFS_BLOCK_SIZE)) < 0) {
            free(bitmap);
            return -1;
        }
    }
    free(bitmap);

    /* ---- Block 1: Home block ---- */
    {
        struct vmsfs_home_block hb;
        memset(&hb, 0, sizeof(hb));

        hb.hb_magic        = VMSFS_HOME_MAGIC;
        hb.hb_format_ver   = VMSFS_FORMAT_VERSION;
        hb.hb_flags        = 0;
        hb.hb_created      = (uint64_t)now;
        hb.hb_modified     = (uint64_t)now;
        hb.hb_block_size   = VMSFS_BLOCK_SIZE;
        hb.hb_total_blocks = total_blocks;
        hb.hb_free_blocks  = free_blocks;
        hb.hb_bitmap_lbn   = bm_lbn;
        hb.hb_bitmap_blocks = bm_blocks;
        hb.hb_index_lbn    = idx_lbn;
        hb.hb_max_files    = max_files;
        hb.hb_data_lbn     = data_lbn;
        hb.hb_cluster_size = 1;
        hb.hb_protection   = 0;          /* S:RWED,O:RWED,G:RWED,W:RWED */
        hb.hb_default_prot = VMSFS_PROT_DEFAULT;

        /* Volume name: uppercase, space-padded */
        memset(hb.hb_volname, ' ', sizeof(hb.hb_volname));
        size_t vlen = strlen(volname);
        if (vlen > MAX_VOLNAME) vlen = MAX_VOLNAME;
        for (size_t i = 0; i < vlen; i++) {
            char c = volname[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            hb.hb_volname[i] = c;
        }

        hb.hb_checksum = vmsfs_checksum(&hb, sizeof(hb));

        if (write_block(fd, VMSFS_HOME_LBN, &hb) < 0) return -1;
    }

    /* ---- File headers: zero the entire index area first ---- */
    {
        uint8_t zero[VMSFS_BLOCK_SIZE];
        memset(zero, 0, sizeof(zero));
        for (uint32_t i = 0; i < max_files; i++) {
            if (write_block(fd, idx_lbn + i, zero) < 0) return -1;
        }
    }

    /* ---- FID 1: INDEXF.SYS ---- */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));

        fh.fh_magic      = VMSFS_FH_MAGIC;
        fh.fh_fid        = VMSFS_FID_INDEXF;
        fh.fh_flags      = VMSFS_FH_INUSE | VMSFS_FH_CONTIGUOUS;
        fh.fh_version    = 1;
        fh.fh_size       = (uint64_t)max_files * VMSFS_BLOCK_SIZE;
        fh.fh_blocks     = max_files;
        fh.fh_protection = 0xFF00; /* S:RWED,O:RWED,G:,W: */
        fh.fh_uic_group  = 1;
        fh.fh_uic_member = 1;
        fh.fh_link_count = 1;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created    = (uint64_t)now;
        fh.fh_modified   = (uint64_t)now;

        strncpy(fh.fh_name, "INDEXF", sizeof(fh.fh_name));
        strncpy(fh.fh_type, "SYS", sizeof(fh.fh_type));

        /* Map: index file is the file header area itself */
        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = idx_lbn;
        fh.fh_map[0].rp_count = max_files;

        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));

        if (write_block(fd, idx_lbn + (VMSFS_FID_INDEXF - 1), &fh) < 0)
            return -1;
    }

    /* ---- FID 2: BITMAP.SYS ---- */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));

        fh.fh_magic      = VMSFS_FH_MAGIC;
        fh.fh_fid        = VMSFS_FID_BITMAP;
        fh.fh_flags      = VMSFS_FH_INUSE | VMSFS_FH_CONTIGUOUS;
        fh.fh_version    = 1;
        fh.fh_size       = (uint64_t)bm_blocks * VMSFS_BLOCK_SIZE;
        fh.fh_blocks     = bm_blocks;
        fh.fh_protection = 0xFF00;
        fh.fh_uic_group  = 1;
        fh.fh_uic_member = 1;
        fh.fh_link_count = 1;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created    = (uint64_t)now;
        fh.fh_modified   = (uint64_t)now;

        strncpy(fh.fh_name, "BITMAP", sizeof(fh.fh_name));
        strncpy(fh.fh_type, "SYS", sizeof(fh.fh_type));

        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = bm_lbn;
        fh.fh_map[0].rp_count = bm_blocks;

        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));

        if (write_block(fd, idx_lbn + (VMSFS_FID_BITMAP - 1), &fh) < 0)
            return -1;
    }

    /* ---- FID 3: 000000.DIR (Master File Directory) ---- */
    /* Allocate one data block for the empty MFD */
    uint32_t mfd_data_lbn = data_lbn; /* first data block */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));

        fh.fh_magic      = VMSFS_FH_MAGIC;
        fh.fh_fid        = VMSFS_FID_MFD;
        fh.fh_flags      = VMSFS_FH_INUSE | VMSFS_FH_DIRECTORY;
        fh.fh_version    = 1;
        fh.fh_size       = 0;   /* empty directory — no entries yet */
        fh.fh_blocks     = 1;
        fh.fh_protection = 0;   /* S:RWED,O:RWED,G:RWED,W:RWED */
        fh.fh_uic_group  = 1;
        fh.fh_uic_member = 1;
        fh.fh_link_count = 2;   /* . and self */
        fh.fh_parent_fid = VMSFS_FID_MFD; /* root's parent is itself */
        fh.fh_created    = (uint64_t)now;
        fh.fh_modified   = (uint64_t)now;

        strncpy(fh.fh_name, "000000", sizeof(fh.fh_name));
        strncpy(fh.fh_type, "DIR", sizeof(fh.fh_type));

        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = mfd_data_lbn;
        fh.fh_map[0].rp_count = 1;

        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));

        if (write_block(fd, idx_lbn + (VMSFS_FID_MFD - 1), &fh) < 0)
            return -1;
    }

    /* Write empty MFD data block and mark it allocated */
    {
        uint8_t empty[VMSFS_BLOCK_SIZE];
        memset(empty, 0, sizeof(empty));
        if (write_block(fd, mfd_data_lbn, empty) < 0) return -1;

        /* Re-read bitmap block 0 to set the MFD data block bit */
        uint8_t bm_buf[VMSFS_BLOCK_SIZE];
        off_t bm_off = (off_t)bm_lbn * VMSFS_BLOCK_SIZE;
        if (lseek(fd, bm_off, SEEK_SET) != bm_off) return -1;
        if (read(fd, bm_buf, VMSFS_BLOCK_SIZE) != VMSFS_BLOCK_SIZE) return -1;

        bitmap_set(bm_buf, mfd_data_lbn);

        if (write_block(fd, bm_lbn, bm_buf) < 0) return -1;

        /* Update home block free count */
        struct vmsfs_home_block hb;
        off_t hb_off = (off_t)VMSFS_HOME_LBN * VMSFS_BLOCK_SIZE;
        if (lseek(fd, hb_off, SEEK_SET) != hb_off) return -1;
        if (read(fd, &hb, sizeof(hb)) != sizeof(hb)) return -1;

        hb.hb_free_blocks--;
        hb.hb_checksum = vmsfs_checksum(&hb, sizeof(hb));

        if (write_block(fd, VMSFS_HOME_LBN, &hb) < 0) return -1;
    }

    printf("%%INIT-I-COMPLETE, volume %.12s initialized\n", volname);
    printf("%%INIT-I-SUMMARY, %u total blocks, %u free, %u file headers\n",
           total_blocks, free_blocks - 1, max_files);

    return 0;
}

/*
 * Get size of a block device via ioctl.
 */
static int get_device_size(int fd, uint64_t *size)
{
    return ioctl(fd, BLKGETSIZE64, size) == 0 ? 0 : -1;
}

int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: INITIALIZE <device-or-file> <volume-label> [size-in-MB]\n");
        fprintf(stderr, "  Format a device or image file with VMSFS structure.\n");
        fprintf(stderr, "  Size is required when creating a new image file.\n");
        return 1;
    }

    const char *target = argv[1];
    const char *volname = argv[2];
    uint64_t size_bytes = 0;
    int create_file = 0;

    /* Validate volume label */
    size_t vlen = strlen(volname);
    if (vlen == 0 || vlen > MAX_VOLNAME) {
        fprintf(stderr, "%%INIT-F-BADLABEL, volume label must be 1-%d characters\n",
                MAX_VOLNAME);
        return 1;
    }

    struct stat st;
    if (stat(target, &st) == 0) {
        if (S_ISBLK(st.st_mode)) {
            /* Block device — get size via ioctl */
            int fd = open(target, O_RDWR);
            if (fd < 0) {
                perror(target);
                return 1;
            }
            if (get_device_size(fd, &size_bytes) < 0) {
                perror("%%INIT-F-IOCTL, cannot determine device size");
                close(fd);
                return 1;
            }
            close(fd);
        } else if (S_ISREG(st.st_mode)) {
            /* Existing regular file — use its size */
            size_bytes = (uint64_t)st.st_size;
        } else {
            fprintf(stderr, "%%INIT-F-BADTARGET, target must be a block device or regular file\n");
            return 1;
        }
    } else {
        /* Target doesn't exist — create file, need size argument */
        if (argc < 4) {
            fprintf(stderr, "%%INIT-F-NEEDSIZE, size-in-MB required for new image file\n");
            return 1;
        }
        create_file = 1;
    }

    /* Parse optional size argument (overrides detected size for files) */
    if (argc == 4) {
        long mb = strtol(argv[3], NULL, 10);
        if (mb <= 0) {
            fprintf(stderr, "%%INIT-F-BADSIZE, size must be a positive number of MB\n");
            return 1;
        }
        size_bytes = (uint64_t)mb * 1024 * 1024;
    }

    if (size_bytes < (uint64_t)MIN_BLOCKS * VMSFS_BLOCK_SIZE) {
        fprintf(stderr, "%%INIT-F-TOOSMALL, volume must be at least %u bytes\n",
                MIN_BLOCKS * VMSFS_BLOCK_SIZE);
        return 1;
    }

    /* Open or create the target */
    int flags = O_RDWR;
    if (create_file) flags |= O_CREAT | O_TRUNC;

    int fd = open(target, flags, 0644);
    if (fd < 0) {
        perror(target);
        return 1;
    }

    /* Extend file to requested size if creating */
    if (create_file) {
        if (ftruncate(fd, (off_t)size_bytes) < 0) {
            perror("%%INIT-F-TRUNC, cannot set file size");
            close(fd);
            return 1;
        }
    }

    int rc = format_volume(fd, size_bytes, volname);

    if (fsync(fd) < 0)
        perror("%%INIT-W-FSYNC, sync warning");

    close(fd);
    return rc < 0 ? 1 : 0;
}
