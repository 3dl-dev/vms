/*
 * mkimage_vmsfs.c - Create a VMSFS test image with a known test file
 *
 * Builds a 1MB VMSFS-formatted image containing:
 *   - INDEXF.SYS (FID 1)
 *   - BITMAP.SYS (FID 2)
 *   - 000000.DIR (FID 3, root MFD)
 *   - HELLO.TXT;1 (FID 4, test file with known content)
 *
 * Used by the QEMU test suite to exercise the block-device mount path.
 *
 * Usage: mkimage_vmsfs <output-file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "vmsfs_ondisk.h"

#define IMAGE_SIZE_MB   1
#define IMAGE_SIZE      ((uint64_t)IMAGE_SIZE_MB * 1024 * 1024)
#define TOTAL_BLOCKS    ((uint32_t)(IMAGE_SIZE / VMSFS_BLOCK_SIZE))

/* Test file content — must match test_kmod_vmsfs_blkdev.c */
#define TEST_CONTENT     "Hello from VMSFS block device!\n"
#define TEST_CONTENT_LEN (sizeof(TEST_CONTENT) - 1)

static int write_block(int fd, uint32_t lbn, const void *buf)
{
    off_t offset = (off_t)lbn * VMSFS_BLOCK_SIZE;
    if (lseek(fd, offset, SEEK_SET) != offset)
        return -1;
    if (write(fd, buf, VMSFS_BLOCK_SIZE) != VMSFS_BLOCK_SIZE)
        return -1;
    return 0;
}

static void bitmap_set(uint8_t *bm, uint32_t bit)
{
    bm[bit / 8] |= (1 << (bit % 8));
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: mkimage_vmsfs <output-file>\n");
        return 1;
    }

    int fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    if (ftruncate(fd, IMAGE_SIZE) < 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    time_t now = time(NULL);

    /* Layout: 1MB = 2048 blocks */
    uint32_t bm_blocks = 1;
    uint32_t bm_lbn = VMSFS_BITMAP_LBN;        /* 2 */
    uint32_t idx_lbn = bm_lbn + bm_blocks;      /* 3 */
    uint32_t max_files = TOTAL_BLOCKS / 100;     /* 20 */
    if (max_files < 16) max_files = 16;
    uint32_t data_lbn = idx_lbn + max_files;     /* 23 */
    uint32_t mfd_data_lbn = data_lbn;            /* 23 */
    uint32_t hello_data_lbn = data_lbn + 1;      /* 24 */
    uint32_t metadata_end = data_lbn;
    uint32_t allocated = metadata_end + 2;       /* +MFD data +HELLO data */
    uint32_t free_blocks = TOTAL_BLOCKS - allocated;

    uint8_t zero_block[VMSFS_BLOCK_SIZE];
    memset(zero_block, 0, sizeof(zero_block));

    /* ---- Block 0: Boot block (zeroed) ---- */
    write_block(fd, 0, zero_block);

    /* ---- Bitmap ---- */
    uint8_t bitmap[VMSFS_BLOCK_SIZE];
    memset(bitmap, 0, sizeof(bitmap));

    /* Mark metadata blocks as allocated */
    for (uint32_t i = 0; i < metadata_end; i++)
        bitmap_set(bitmap, i);

    /* Mark MFD and HELLO data blocks */
    bitmap_set(bitmap, mfd_data_lbn);
    bitmap_set(bitmap, hello_data_lbn);

    /* Mark beyond-volume blocks as allocated */
    uint32_t bitmap_cap = VMSFS_BLOCK_SIZE * 8;
    for (uint32_t i = TOTAL_BLOCKS; i < bitmap_cap; i++)
        bitmap_set(bitmap, i);

    write_block(fd, bm_lbn, bitmap);

    /* ---- Home block ---- */
    struct vmsfs_home_block hb;
    memset(&hb, 0, sizeof(hb));
    hb.hb_magic        = VMSFS_HOME_MAGIC;
    hb.hb_format_ver   = VMSFS_FORMAT_VERSION;
    hb.hb_created      = (uint64_t)now;
    hb.hb_modified     = (uint64_t)now;
    hb.hb_block_size   = VMSFS_BLOCK_SIZE;
    hb.hb_total_blocks = TOTAL_BLOCKS;
    hb.hb_free_blocks  = free_blocks;
    hb.hb_bitmap_lbn   = bm_lbn;
    hb.hb_bitmap_blocks = bm_blocks;
    hb.hb_index_lbn    = idx_lbn;
    hb.hb_max_files    = max_files;
    hb.hb_data_lbn     = data_lbn;
    hb.hb_cluster_size = 1;
    hb.hb_protection   = 0;
    hb.hb_default_prot = VMSFS_PROT_DEFAULT;
    memcpy(hb.hb_volname, "TESTVOL     ", 12);
    hb.hb_checksum = vmsfs_checksum(&hb, sizeof(hb));
    write_block(fd, VMSFS_HOME_LBN, &hb);

    /* ---- Zero the entire index area ---- */
    for (uint32_t i = 0; i < max_files; i++)
        write_block(fd, idx_lbn + i, zero_block);

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
        fh.fh_protection = 0xFF00;
        fh.fh_uic_group  = 1;
        fh.fh_uic_member = 1;
        fh.fh_link_count = 1;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created    = (uint64_t)now;
        fh.fh_modified   = (uint64_t)now;
        strncpy(fh.fh_name, "INDEXF", sizeof(fh.fh_name));
        strncpy(fh.fh_type, "SYS", sizeof(fh.fh_type));
        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = idx_lbn;
        fh.fh_map[0].rp_count = max_files;
        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));
        write_block(fd, idx_lbn + (VMSFS_FID_INDEXF - 1), &fh);
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
        write_block(fd, idx_lbn + (VMSFS_FID_BITMAP - 1), &fh);
    }

    /* ---- FID 3: 000000.DIR (MFD) — with one entry for HELLO.TXT ---- */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));
        fh.fh_magic      = VMSFS_FH_MAGIC;
        fh.fh_fid        = VMSFS_FID_MFD;
        fh.fh_flags      = VMSFS_FH_INUSE | VMSFS_FH_DIRECTORY;
        fh.fh_version    = 1;
        fh.fh_size       = sizeof(struct vmsfs_dir_entry); /* 88 bytes */
        fh.fh_blocks     = 1;
        fh.fh_protection = 0;
        fh.fh_uic_group  = 1;
        fh.fh_uic_member = 1;
        fh.fh_link_count = 2;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created    = (uint64_t)now;
        fh.fh_modified   = (uint64_t)now;
        strncpy(fh.fh_name, "000000", sizeof(fh.fh_name));
        strncpy(fh.fh_type, "DIR", sizeof(fh.fh_type));
        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = mfd_data_lbn;
        fh.fh_map[0].rp_count = 1;
        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));
        write_block(fd, idx_lbn + (VMSFS_FID_MFD - 1), &fh);
    }

    /* ---- FID 4: HELLO.TXT;1 ---- */
    {
        struct vmsfs_file_header fh;
        memset(&fh, 0, sizeof(fh));
        fh.fh_magic      = VMSFS_FH_MAGIC;
        fh.fh_fid        = VMSFS_FID_FIRST_USER;
        fh.fh_flags      = VMSFS_FH_INUSE;
        fh.fh_version    = 1;
        fh.fh_size       = TEST_CONTENT_LEN;
        fh.fh_blocks     = 1;
        fh.fh_protection = VMSFS_PROT_DEFAULT;
        fh.fh_uic_group  = 1;
        fh.fh_uic_member = 4;
        fh.fh_link_count = 1;
        fh.fh_parent_fid = VMSFS_FID_MFD;
        fh.fh_created    = (uint64_t)now;
        fh.fh_modified   = (uint64_t)now;
        strncpy(fh.fh_name, "HELLO", sizeof(fh.fh_name));
        strncpy(fh.fh_type, "TXT", sizeof(fh.fh_type));
        fh.fh_map_count  = 1;
        fh.fh_map[0].rp_lbn   = hello_data_lbn;
        fh.fh_map[0].rp_count = 1;
        fh.fh_checksum = vmsfs_checksum(&fh, sizeof(fh));
        write_block(fd, idx_lbn + (VMSFS_FID_FIRST_USER - 1), &fh);
    }

    /* ---- MFD directory data block — one entry for HELLO.TXT ---- */
    {
        uint8_t mfd_data[VMSFS_BLOCK_SIZE];
        memset(mfd_data, 0, sizeof(mfd_data));

        struct vmsfs_dir_entry *de = (struct vmsfs_dir_entry *)mfd_data;
        de->de_fid     = VMSFS_FID_FIRST_USER;
        de->de_version = 1;
        const char *name = "HELLO.TXT";
        de->de_name_len = strlen(name);
        strncpy(de->de_name, name, sizeof(de->de_name));

        write_block(fd, mfd_data_lbn, mfd_data);
    }

    /* ---- HELLO.TXT data block ---- */
    {
        uint8_t file_data[VMSFS_BLOCK_SIZE];
        memset(file_data, 0, sizeof(file_data));
        memcpy(file_data, TEST_CONTENT, TEST_CONTENT_LEN);
        write_block(fd, hello_data_lbn, file_data);
    }

    fsync(fd);
    close(fd);

    printf("Created vmsfs test image: %s\n", argv[1]);
    printf("  Volume: TESTVOL, %u blocks (%u KB)\n",
           TOTAL_BLOCKS, TOTAL_BLOCKS / 2);
    printf("  Layout: bitmap@%u, index@%u (%u headers), data@%u\n",
           bm_lbn, idx_lbn, max_files, data_lbn);
    printf("  HELLO.TXT;1: %zu bytes at LBN %u\n",
           (size_t)TEST_CONTENT_LEN, hello_data_lbn);
    printf("  Free blocks: %u\n", free_blocks);

    return 0;
}
