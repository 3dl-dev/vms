/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_inject_sysgen.c - offline SYS$SYSTEM:OVMXVMSSYS.PAR injector/reader for
 * the OVMX/x86 cluster web demo (rd vms-f0f, CORRECTED to ODS-2-resident).
 *
 * ROOT CAUSE (why the .PAR must live on the ODS-2 system disk, NOT the
 * initramfs): the x86 PID 1 (src/ovmx_init/ovmx_init.c) ACP-stages the DISK's
 * SYS$SYSTEM:OVMXVMSSYS.PAR into OVMX_BOOT_STAGE_DIR ("/run/ovmx-boot",
 * ovmx_init.c:~1046-1049) BEFORE read_boot_parameters(), and read_boot_parameters()
 * (ovmx_init.c:1147) then points OVMX_SYSGEN_PATH at that staged copy. So a
 * .PAR pre-placed in the initramfs at /run/ovmx-boot/OVMXVMSSYS.PAR is
 * OVERWRITTEN by the disk's stock copy. The boot reads the HIGHEST version of
 * SYS$SYSTEM:OVMXVMSSYS.PAR off the genuine Files-11 (ODS-2) volume over the
 * ACP (sysgen_params.h:293). Therefore the VAXCLUSTER=2 config must be written
 * ONTO the ODS-2 disk as a new, higher version of SYS$SYSTEM:OVMXVMSSYS.PAR.
 *
 * REUSE (HARD GUARDRAIL 1): this tool builds NO ODS-2 format knowledge of its
 * own. It is a thin driver over the project's OWN genuine-ODS-2 codec library
 * (src/vmsfs/ods2: ods2_reader.c/ods2_writer.c/ods2_edit.c/ods2_bdev.c/
 * ods2_path.c/ods2_block_posix.c) -- the SAME library vmsfs_master's
 * emit_tree_ods2() uses to master the distribution image in the first place
 * (ods2_wvolume_open_bdev + create_file_raw + dir_insert), and the SAME
 * block-backed reader (ods2_bdev_*) the live runtime uses. It is the exact
 * shape of tests/ods2/test_ods2_wvolume_reopen_append.c: reattach a writer to
 * an EXISTING volume and add a file, then read it back with the block reader.
 *
 * SS$-honest (Rule 9 / INV-6): every failure returns a non-zero exit with the
 * codec's own status; nothing is faked.
 *
 * Modes (operate on a RAW ODS-2 image file; the qcow2<->raw plumbing is the
 * caller's, via qemu-img -- see inject-ods2-config.sh):
 *
 *   inject   <raw-image> <par-file>
 *       Reattach a writer to <raw-image>, resolve the SYS$SYSTEM directory
 *       ([SYS0.SYSCOMMON.SYSEXE]), find the current highest version of
 *       OVMXVMSSYS.PAR, and write <par-file>'s bytes VERBATIM (RFM=FIXED, via
 *       create_file_raw -- the binary-image path, never the VAR reframing) as
 *       version+1. Highest-version-wins is faithful VMS: the boot's HIGHEST-
 *       version read then returns the injected config.
 *
 *   readback <raw-image> <out-par-file>
 *       Resolve SYS$SYSTEM:OVMXVMSSYS.PAR (highest version) with the project's
 *       OWN block-backed ODS-2 reader and write its exact bytes to
 *       <out-par-file>, for offline assertion (magic/VAXCLUSTER/SCSNODE) by
 *       mk_democonfig.parse_sysgen_store().
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* SYS$SYSTEM == SYSDISK:[SYS0.SYSCOMMON.SYSEXE] (ovmx_layout.h VMS_SYSEXE). */
static const char *const SYSEXE_COMPS[] = { "SYS0", "SYSCOMMON", "SYSEXE" };
#define SYSEXE_NDIRS 3
#define PAR_NAME     "OVMXVMSSYS.PAR"

/* SYSGEN store magic, cross-checked here as a cheap sanity gate (the full
 * param assertion is mk_democonfig.parse_sysgen_store()'s job). Matches
 * sysgen_params.h SYSGEN_MAGIC / mk_democonfig SYSGEN_MAGIC. */
#define SYSGEN_MAGIC 0x53595347u   /* "SYSG" */

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Slurp a whole file into a freshly malloc'd buffer. */
static uint8_t *slurp(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long sz;

    if (!fp) {
        fprintf(stderr, "ods2_inject_sysgen: cannot open %s: %s\n",
                path, strerror(errno));
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) < 0) {
        fprintf(stderr, "ods2_inject_sysgen: seek failed on %s\n", path);
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    buf = malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fprintf(stderr, "ods2_inject_sysgen: short read on %s\n", path);
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *len_out = (size_t)sz;
    return buf;
}

static int do_inject(const char *image, const char *parfile)
{
    uint8_t   *par;
    size_t     parlen = 0;
    int        fd;
    ods2_bdev_t   rd;
    ods2_wvolume_t wvol;
    ods2_fid_t sysexe_fid, cur_fid, new_fid;
    uint16_t   cur_ver = 0, new_ver;
    uint8_t    dirhdr[ODS2_BLOCK_SIZE];
    ods2_status_t st;

    par = slurp(parfile, &parlen);
    if (!par)
        return 2;
    if (parlen < 4 || rd_le32(par) != SYSGEN_MAGIC) {
        fprintf(stderr, "ods2_inject_sysgen: %s is not a SYSGEN store "
                "(magic %#x != %#x)\n", parfile,
                parlen >= 4 ? rd_le32(par) : 0u, SYSGEN_MAGIC);
        free(par);
        return 2;
    }

    fd = open(image, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "ods2_inject_sysgen: cannot open image %s: %s\n",
                image, strerror(errno));
        free(par);
        return 2;
    }

    /* --- READER pass: resolve SYS$SYSTEM + find current highest .PAR ver --- */
    st = ods2_bdev_open(&rd, fd, 0 /* auto-detect span */);
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: %s is not a genuine ODS-2 volume "
                "(ods2_bdev_open status %d)\n", image, (int)st);
        goto fail;
    }
    st = ods2_bdev_resolve_dir(&rd, SYSEXE_COMPS, SYSEXE_NDIRS,
                               &sysexe_fid, dirhdr, sizeof(dirhdr));
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: cannot resolve "
                "SYS$SYSTEM ([SYS0.SYSCOMMON.SYSEXE]) (status %d)\n", (int)st);
        goto fail;
    }
    st = ods2_bdev_dir_find(&rd, dirhdr, PAR_NAME, 0 /* highest */,
                            &cur_fid, &cur_ver);
    if (st == ODS2_OK) {
        new_ver = (uint16_t)(cur_ver + 1);
        printf("ods2_inject_sysgen: existing SYS$SYSTEM:%s highest version=%u "
               "-> injecting version=%u\n", PAR_NAME, cur_ver, new_ver);
    } else {
        /* Faithful default: a fresh install lands ;1 (MASTER_FILE_VER). */
        new_ver = 1;
        printf("ods2_inject_sysgen: no existing SYS$SYSTEM:%s -> injecting "
               "version=1\n", PAR_NAME);
    }

    /* --- WRITER pass: reattach + create_file_raw + dir_insert --- */
    st = ods2_wvolume_open_bdev(fd, 0 /* auto-detect */, &wvol);
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: ods2_wvolume_open_bdev failed "
                "(status %d)\n", (int)st);
        goto fail;
    }
    /* Binary store -> RFM=FIXED verbatim, EXACTLY the path emit_tree_ods2()
     * masters OVMXVMSSYS.PAR;1 with (vmsfs_master.c). create_file_raw sizes
     * ceil(len/512) blocks itself, so the 9484-byte store lands intact. */
    st = ods2_wvolume_create_file_raw(&wvol, PAR_NAME, new_ver,
                                      par, parlen, sysexe_fid, &new_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: create_file_raw(%s;%u) failed "
                "(status %d)\n", PAR_NAME, new_ver, (int)st);
        ods2_wvolume_close(&wvol);
        goto fail;
    }
    st = ods2_wvolume_dir_insert(&wvol, sysexe_fid, PAR_NAME, new_ver, new_fid);
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: dir_insert(%s;%u) failed "
                "(status %d)\n", PAR_NAME, new_ver, (int)st);
        ods2_wvolume_close(&wvol);
        goto fail;
    }
    if (wvol.io_error != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: sticky I/O error after write "
                "(status %d)\n", (int)wvol.io_error);
        ods2_wvolume_close(&wvol);
        goto fail;
    }
    ods2_wvolume_close(&wvol);   /* flush + free; fd stays open */

    if (fsync(fd) != 0)
        fprintf(stderr, "ods2_inject_sysgen: warning: fsync: %s\n",
                strerror(errno));
    close(fd);
    free(par);
    printf("ods2_inject_sysgen: injected SYS$SYSTEM:%s;%u (%zu bytes) into %s\n",
           PAR_NAME, new_ver, parlen, image);
    return 0;

fail:
    close(fd);
    free(par);
    return 2;
}

static int do_readback(const char *image, const char *outpar)
{
    int fd;
    ods2_bdev_t rd;
    ods2_fid_t fid;
    uint8_t hdr[ODS2_BLOCK_SIZE];
    uint8_t *buf;
    size_t got = 0;
    const size_t cap = 512 * 1024;   /* >> 9484-byte store */
    ods2_status_t st;
    FILE *fp;
    uint16_t ver = 0;

    fd = open(image, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "ods2_inject_sysgen: cannot open image %s: %s\n",
                image, strerror(errno));
        return 2;
    }
    st = ods2_bdev_open(&rd, fd, 0);
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: ods2_bdev_open failed (status %d)\n",
                (int)st);
        close(fd);
        return 2;
    }
    /* Report which version the HIGHEST-version read resolves (the one the
     * boot's read_boot_parameters() would consume). */
    {
        uint8_t dirhdr[ODS2_BLOCK_SIZE];
        ods2_fid_t sfid;
        if (ods2_bdev_resolve_dir(&rd, SYSEXE_COMPS, SYSEXE_NDIRS,
                                  &sfid, dirhdr, sizeof(dirhdr)) == ODS2_OK)
            (void)ods2_bdev_dir_find(&rd, dirhdr, PAR_NAME, 0, &sfid, &ver);
    }
    st = ods2_bdev_resolve_file(&rd, SYSEXE_COMPS, SYSEXE_NDIRS,
                                PAR_NAME, 0 /* highest */, &fid,
                                hdr, sizeof(hdr));
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: SYS$SYSTEM:%s not found "
                "(status %d)\n", PAR_NAME, (int)st);
        close(fd);
        return 2;
    }
    buf = malloc(cap);
    if (!buf) { close(fd); return 2; }
    st = ods2_bdev_read_file(&rd, hdr, buf, cap, &got);
    if (st != ODS2_OK) {
        fprintf(stderr, "ods2_inject_sysgen: read_file(%s) failed (status %d)\n",
                PAR_NAME, (int)st);
        free(buf);
        close(fd);
        return 2;
    }
    close(fd);

    if (got < 4 || rd_le32(buf) != SYSGEN_MAGIC) {
        fprintf(stderr, "ods2_inject_sysgen: read-back %s magic %#x != %#x\n",
                PAR_NAME, got >= 4 ? rd_le32(buf) : 0u, SYSGEN_MAGIC);
        free(buf);
        return 3;
    }
    fp = fopen(outpar, "wb");
    if (!fp || fwrite(buf, 1, got, fp) != got) {
        fprintf(stderr, "ods2_inject_sysgen: cannot write %s\n", outpar);
        if (fp) fclose(fp);
        free(buf);
        return 2;
    }
    fclose(fp);
    free(buf);
    printf("ods2_inject_sysgen: read SYS$SYSTEM:%s;%u back off %s "
           "(%zu bytes, magic OK) -> %s\n",
           PAR_NAME, ver, image, got, outpar);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage:\n"
        "  %s inject   <raw-ods2-image> <par-file>       add SYS$SYSTEM:%s;N+1\n"
        "  %s readback <raw-ods2-image> <out-par-file>   read highest %s out\n",
        prog, PAR_NAME, prog, PAR_NAME);
}

int main(int argc, char *argv[])
{
    if (argc != 4) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "inject") == 0)
        return do_inject(argv[2], argv[3]);
    if (strcmp(argv[1], "readback") == 0)
        return do_readback(argv[2], argv[3]);
    usage(argv[0]);
    return 2;
}
