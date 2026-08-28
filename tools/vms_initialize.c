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
 *   If the target is a VMS device name (e.g. DKA100:), it is resolved through
 *     the executive to its real backing block device and THAT is formatted;
 *     an unresolvable unit fails honestly (never a bogus local file).
 *   If the target is a regular file and doesn't exist, size-in-MB is required.
 *   If the target is a block device, size is determined automatically.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <linux/fs.h>       /* BLKGETSIZE64 */
#elif defined(__NetBSD__)
#include <sys/disklabel.h>  /* DIOCGDINFO / struct disklabel (vms-64a: netbsd-vax port) */
#endif

#include "vmsfs_ondisk.h"
#include "vmsfs/ods2.h" /* GENUINE ODS-2 (Files-11 L2) writer: ods2_volume_format */
#include "vms_kif.h"    /* vms_kif_disk_resolve -- executive device table */
#include "ssdef.h"      /* SS$_NORMAL / SS$_NOSUCHDEV / SS$_IVDEVNAM */

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
 * Format the volume as a GENUINE ODS-2 (Files-11 On-Disk Structure Level 2)
 * volume (vms-6ef, R4 of the real-ODS-2-runtime epic vms-5eb).
 *
 * Unlike format_volume() above -- which writes OVMX's bespoke, NON-genuine
 * VMFS/VFH2 structure (hb_magic == VMSFS_HOME_MAGIC, MFD == FID 3) -- this
 * delegates to the byte-genuine writer in src/vmsfs/ods2 (ods2_volume_format,
 * the same [F15] path that MOUNTs clean on a real VAX). The result is a real
 * "DECFILE11B  " Files-11 volume: home block pair with dual additive
 * checksums, the ten reserved system files (INDEXF.SYS FID 1, BITMAP.SYS FID
 * 2, 000000.DIR/MFD FID 4, ... SECURITY.SYS FID 10), the SCB + storage
 * bitmap, and the MFD populated with directory entries for all ten.
 *
 * ORACLE VALUES: every rw on-disk constant this path emits is inherited
 * unchanged from ods2_volume_format / ods2.h, where each is grounded per
 * Rule 8 -- ODS2_STRUCLEV_V2 (0x0201) from the public Files-11 structure-
 * level encoding [S]; hm2_resfiles == 10 and the FID 6-10 reserved names
 * from the real-VAX fixture tests/ods2/real_vax_ods2.dsk [F]; the SCB fields
 * still flagged [OVMX-inferred] (scb_blksize==1, scb_status/status2==0,
 * scb_writecnt, scb_volockname width) carried exactly as the writer emits
 * them. INITIALIZE introduces NO new unpinned rw value of its own.
 *
 * The writer operates over a caller-owned in-memory image; INITIALIZE builds
 * the full image, then writes only the populated metadata region
 * [0, wvol.next_free_lbn) to the device -- the data area beyond it is marked
 * free in the storage bitmap and left untouched (a real VMS INITIALIZE does
 * not scrub the platter either). NOTE: the current writer requires the whole
 * total_blocks*512 image in RAM (it zero-fills the full buffer); a windowed
 * writer is left to the writer-completeness rung (R8, vms-af7a).
 */
static int format_volume_ods2(int fd, uint64_t size_bytes, const char *volname)
{
    if (size_bytes / ODS2_BLOCK_SIZE > 0xFFFFFFFFULL) {
        fprintf(stderr, "%%INIT-F-TOOBIG, volume exceeds the 2^32-block ODS-2 limit\n");
        return -1;
    }
    uint32_t total_blocks = (uint32_t)(size_bytes / ODS2_BLOCK_SIZE);

    if (total_blocks < MIN_BLOCKS) {
        fprintf(stderr, "%%INIT-F-TOOSMALL, volume too small (%u blocks, need %d)\n",
                total_blocks, MIN_BLOCKS);
        return -1;
    }

    /* Index-file capacity: ~1% of the volume, floored well above the ten
     * reserved files and capped so the fixed reserved-file layout (which
     * grows with maxfiles) stays a small fraction of a modest volume. */
    uint32_t maxfiles = total_blocks / 100;
    if (maxfiles < 16)    maxfiles = 16;
    if (maxfiles > 65535) maxfiles = 65535;
    if (maxfiles < ODS2_RESFILES) maxfiles = ODS2_RESFILES;

    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = calloc(1, image_len);
    if (!image) {
        fprintf(stderr,
                "%%INIT-F-NOMEM, cannot allocate a %zu-byte ODS-2 image "
                "(volume too large for the in-memory writer)\n", image_len);
        return -1;
    }

    ods2_format_params_t params;
    params.total_blocks = total_blocks;
    params.maxfiles     = maxfiles;
    params.volname      = volname;

    ods2_wvolume_t wvol;
    ods2_status_t st = ods2_volume_format(image, image_len, &params, &wvol);
    if (st != ODS2_OK) {
        fprintf(stderr, "%%INIT-F-FORMAT, ODS-2 format failed: %s\n",
                ods2_strerror(st));
        free(image);
        return -1;
    }

    printf("%%INIT-I-FORMAT, formatting %.12s as genuine ODS-2 (DECFILE11B)\n",
           volname);
    printf("%%INIT-I-LAYOUT, %u blocks, %u max files, %u metadata blocks\n",
           total_blocks, maxfiles, wvol.next_free_lbn);

    /* Write only the populated metadata region; the data area is already
     * accounted for as free in the on-disk storage bitmap. */
    for (uint32_t lbn = 0; lbn < wvol.next_free_lbn; lbn++) {
        if (write_block(fd, lbn, image + (size_t)lbn * ODS2_BLOCK_SIZE) < 0) {
            free(image);
            return -1;
        }
    }

    free(image);

    printf("%%INIT-I-COMPLETE, volume %.12s initialized (ODS-2)\n", volname);
    return 0;
}

/*
 * Get size of a block device via ioctl. Hardware-generic per-platform query
 * (Rule 9: the block-device path must not be QEMU/virtio-specific, and must
 * fail honestly rather than fake a size) -- Linux uses BLKGETSIZE64; NetBSD
 * (vms-64a: netbsd-vax cross-build) has no such ioctl, so this uses the
 * standard NetBSD disklabel(9) query (DIOCGDINFO -> struct disklabel's
 * d_secsize/d_secperunit), documented public NetBSD kernel API, not any VMS
 * format (Rule 8 does not apply here).
 */
static int get_device_size(int fd, uint64_t *size)
{
#if defined(__linux__)
    return ioctl(fd, BLKGETSIZE64, size) == 0 ? 0 : -1;
#elif defined(__NetBSD__)
    struct disklabel dl;
    if (ioctl(fd, DIOCGDINFO, &dl) != 0)
        return -1;
    *size = (uint64_t)dl.d_secsize * (uint64_t)dl.d_secperunit;
    return 0;
#else
    (void)fd; (void)size;
    return -1;
#endif
}

/*
 * A VMS device name (DKA100:, DKB0:, ...) is not a filesystem path: it has no
 * '/' and ends with a colon. Anything else is treated as an image-file path so
 * the build tooling that formats plain .dsk images keeps working.
 */
static int target_is_vms_device(const char *target)
{
    size_t n = strlen(target);
    if (n < 2 || target[n - 1] != ':')
        return 0;
    if (strchr(target, '/'))
        return 0;
    return 1;
}

/*
 * Resolve a VMS device name to its real backing block device through the
 * executive (vms_kif_disk_resolve, vms-3e8 -- the same authoritative path
 * MOUNT uses). The process never scans /sys/block itself (Rule 11); the
 * executive owns the DKA0:/DKA100: -> backing mapping.
 *
 * On success, writes "/dev/<backing>" into devpath and returns SS$_NORMAL.
 * On any failure it prints an authentic %INIT- error and returns the status.
 * It NEVER falls back to a local file -- an unresolvable unit fails honestly
 * rather than formatting a bogus file and reporting success (INV-6).
 */
static uint32_t resolve_vms_device(const char *name, char *devpath,
                                   size_t devpath_size)
{
    /* Canonicalise: uppercase, trailing colon (matches cmd_mount's key). */
    char dev_name[VMS_DEVNAM_SIZE];
    size_t n = strlen(name);
    if (n >= sizeof(dev_name) - 1)
        n = sizeof(dev_name) - 2;
    for (size_t i = 0; i < n; i++)
        dev_name[i] = (char)toupper((unsigned char)name[i]);
    dev_name[n] = '\0';
    if (n == 0 || dev_name[n - 1] != ':') {
        dev_name[n] = ':';
        dev_name[n + 1] = '\0';
    }

    (void)vms_kif_open();

    char backing[VMS_BACKING_SIZE];
    memset(backing, 0, sizeof(backing));
    uint32_t st = vms_kif_disk_resolve(dev_name, backing, sizeof(backing),
                                       NULL, NULL);
    switch (st) {
    case SS$_NORMAL:
        break;
    case SS$_NOSUCHDEV:
        fprintf(stderr, "%%INIT-F-NOSUCHDEV, no such device %s\n", dev_name);
        return st;
    case SS$_IVDEVNAM:
        fprintf(stderr, "%%INIT-F-IVDEVNAM, invalid device name %s\n", dev_name);
        return st;
    default:
        /* Executive-unreachable / ioctl-level failure (e.g. no /dev/vms):
         * report honestly, never fake a format. */
        fprintf(stderr,
                "%%INIT-F-DEVRESOLVE, could not resolve %s through the "
                "executive (status %u)\n", dev_name, st);
        return st ? st : SS$_BUGCHECK;
    }

    snprintf(devpath, devpath_size, "/dev/%s", backing);
    return SS$_NORMAL;
}

/*
 * Format a real backing block device: open it, take its size from the block
 * geometry (never a caller-supplied MB count -- the hardware size is
 * authoritative), and write the vmsfs structures to the real store.
 */
static int initialize_device(const char *devpath, const char *volname,
                             int use_ods2)
{
    int fd = open(devpath, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "%%INIT-F-OPENERR, cannot open backing device %s\n",
                devpath);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%%INIT-F-NOTBLOCK, %s is not a block device\n",
                devpath);
        close(fd);
        return 1;
    }

    uint64_t size_bytes = 0;
    if (get_device_size(fd, &size_bytes) < 0) {
        fprintf(stderr, "%%INIT-F-IOCTL, cannot determine size of %s\n",
                devpath);
        close(fd);
        return 1;
    }

    if (size_bytes < (uint64_t)MIN_BLOCKS * VMSFS_BLOCK_SIZE) {
        fprintf(stderr, "%%INIT-F-TOOSMALL, %s is smaller than %u bytes\n",
                devpath, MIN_BLOCKS * VMSFS_BLOCK_SIZE);
        close(fd);
        return 1;
    }

    int rc = use_ods2 ? format_volume_ods2(fd, size_bytes, volname)
                      : format_volume(fd, size_bytes, volname);
    if (fsync(fd) < 0)
        perror("%%INIT-W-FSYNC, sync warning");
    close(fd);
    return rc < 0 ? 1 : 0;
}

int main(int argc, char *argv[])
{
    /*
     * On-disk format selection (vms-6ef, R4 of epic vms-5eb):
     *   --ods2   write a GENUINE ODS-2 (Files-11 L2) "DECFILE11B" volume
     *   --vmfs   write OVMX's bespoke VMFS/VFH2 structure (the default)
     * plus OVMX_INIT_ODS2 in the environment (non-empty, != "0") as a default.
     *
     * The default stays VMFS deliberately: the VMFS home-block format
     * (VMSFS_HOME_MAGIC in src/vmsfs/include/vmsfs_ondisk.h; the vmsfs.ko
     * block-device MOUNT path that once validated it was retired by vms-165),
     * the INITIALIZE unit test (tests/qemu/test_syssvc_initialize.c), and the
     * install/upgrade e2e gates all still READ the VMFS structure. Flipping
     * the default to ODS-2 must land atomically with the ODS-2 read path
     * (epic vms-5eb R2/R3/R5/R6); doing it here would break those enforcing
     * consumers -- exactly the "do not flip the live MOUNT resolvers or the
     * boot path" constraint this rung is written under. ODS-2 is therefore
     * opt-in for R4: the writer capability + proof land now; the atomic flip
     * of the default is deferred to the read-path group.
     */
    const char *env_ods2 = getenv("OVMX_INIT_ODS2");
    int use_ods2 = (env_ods2 && env_ods2[0] && strcmp(env_ods2, "0") != 0);

    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
        if (strcmp(argv[argi], "--ods2") == 0) {
            use_ods2 = 1;
        } else if (strcmp(argv[argi], "--vmfs") == 0) {
            use_ods2 = 0;
        } else if (strcmp(argv[argi], "--") == 0) {
            argi++;
            break;
        } else {
            fprintf(stderr, "%%INIT-F-BADOPT, unknown option %s\n", argv[argi]);
            return 1;
        }
        argi++;
    }

    int posargc = argc - argi;   /* positional args after any flags */
    if (posargc < 2 || posargc > 3) {
        fprintf(stderr, "Usage: INITIALIZE [--ods2|--vmfs] <device-or-file> <volume-label> [size-in-MB]\n");
        fprintf(stderr, "  Format a device or image file. Default structure is VMFS;\n");
        fprintf(stderr, "  --ods2 writes a genuine ODS-2 (Files-11 L2) DECFILE11B volume.\n");
        fprintf(stderr, "  Size is required when creating a new image file.\n");
        return 1;
    }

    const char *target = argv[argi];
    const char *volname = argv[argi + 1];
    const char *size_arg = (posargc == 3) ? argv[argi + 2] : NULL;
    uint64_t size_bytes = 0;
    int create_file = 0;

    /* Validate volume label */
    size_t vlen = strlen(volname);
    if (vlen == 0 || vlen > MAX_VOLNAME) {
        fprintf(stderr, "%%INIT-F-BADLABEL, volume label must be 1-%d characters\n",
                MAX_VOLNAME);
        return 1;
    }

    /*
     * A VMS device name (DKA100:) is resolved through the executive to its
     * real backing block device and THAT is formatted. This is the whole
     * point of vms-cf62: the previous code treated "DKA100:" as a literal
     * filesystem path, so an unresolvable name landed in the create-a-file
     * branch below and formatted a bogus local file called "DKA100:",
     * reporting success -- the disk the operator meant to initialize was
     * never touched (INV-6 fake-success). A device name never gets to touch
     * the file path now: it resolves or it fails honestly.
     */
    if (target_is_vms_device(target)) {
        char devpath[VMS_BACKING_SIZE + 8];
        if (resolve_vms_device(target, devpath, sizeof(devpath)) != SS$_NORMAL)
            return 1;   /* honest %INIT- error already printed */
        return initialize_device(devpath, volname, use_ods2);
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
        if (size_arg == NULL) {
            fprintf(stderr, "%%INIT-F-NEEDSIZE, size-in-MB required for new image file\n");
            return 1;
        }
        create_file = 1;
    }

    /* Parse optional size argument (overrides detected size for files) */
    if (size_arg != NULL) {
        long mb = strtol(size_arg, NULL, 10);
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

    int rc = use_ods2 ? format_volume_ods2(fd, size_bytes, volname)
                      : format_volume(fd, size_bytes, volname);

    if (fsync(fd) < 0)
        perror("%%INIT-W-FSYNC, sync warning");

    close(fd);
    return rc < 0 ? 1 : 0;
}
