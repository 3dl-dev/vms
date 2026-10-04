/*
 * sysvol_stage.h - stage an IMGACT-ACTIVATED image onto the ODS-2 system volume
 * over the executive Files-11 ACP, so IMGACT resolves it the VMS way (vms-c09f).
 *
 * WHY THIS EXISTS. The self-host MMK rail swapped its subject from the STATIC
 * mmk_native binary to the OVMX-native IMGACT-ACTIVATED MMK.EXE (PT_INTERP=
 * IMGACT.EXE). A Linux `execl(mmk)` maps MMK's PT_LOAD + opens its PT_INTERP by
 * POSIX path (the initramfs copy the Dockerfile stages), but the ACTIVATOR then
 * re-reads the GENUINE main-image bytes off OVMX_SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]
 * THROUGH the ACP (imgact.c imgsrc_open -> imgact_acp_open, g_acp_sysdevice) --
 * NEVER a /vms POSIX image read (Rule 9 / INV-6). There is deliberately NO /vms
 * fallback for a MAIN image: a main image missing from the volume MUST fail
 * %IMGACT-F-IMGNOTFND (imgact.c die_imgnotfnd), which is exactly what a bare
 * activated MMK.EXE hit before this -- the subject was not on the volume.
 *
 * So the activated subject has to LIVE on the mounted system volume, exactly as
 * a real VMS image lives on the system disk. test_syssvc_mmk_build already
 * stages its PRODUCED image (OVMXRT.EXE) onto VDA300: this same way (create +
 * IO$_WRITEVBLK over the ACP); this header lifts that VDA300: mount + write-
 * over-ACP + OVMX_SYSDEVICE machinery into ONE shared unit, so
 * test_syssvc_mmk_drive (which had none of it) reuses it rather than forking a
 * copy -- and mmk_build's own OVMXRT write rides the same generalized writer.
 *
 * NOTE ON /run/ovmx-boot: MMK's execfn is the /vms SYSEXE path directly (not a
 * /run/ovmx-boot staged path), so imgsrc_map_staged() passes it through as-is
 * and IMGACT opens it off the volume over the ACP. MMK therefore needs ONLY the
 * on-volume copy this header writes -- NOT a /run/ovmx-boot stage (that stage is
 * only for images the kernel execs FROM the tmpfs, i.e. the produced OVMXRT.EXE).
 *
 * COMPILE MODEL. Each test_syssvc_* suite is compiled standalone (its own main),
 * so these are per-TU static helpers; both suites #include this after their
 * vms_kif.h / ssdef.h / vmsfs/ods2.h includes (the ACP KIF + status + file-kind
 * selectors this needs). Helpers are unused-safe so a suite that uses only a
 * subset does not warn.
 */
#ifndef OVMX_TESTS_QEMU_SYSVOL_STAGE_H
#define OVMX_TESTS_QEMU_SYSVOL_STAGE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

/* THE SYSTEM VOLUME an activated image is resolved off (the discovered
 * OVMX_SYSDEVICE the harness points IMGACT at; vms-104). Mastered by
 * mkimage_ods2_sysvol with a [SYS0.SYSCOMMON.SYSEXE] tree; the clean-room
 * real-VAX VDA0: fixture is NEVER used for OVMX toolchain/image files. */
#ifndef SYSVOL_UNIT
#define SYSVOL_UNIT "VDA300:"
#endif

/* The [SYS0.SYSCOMMON.SYSEXE] directory FID on SYSVOL_UNIT, resolved once by
 * sysvol_prepare() by walking the ACP directory tree. sysvol_write_image()
 * creates images under this DID. */
static uint16_t g_sysexe_num __attribute__((unused)) = 0, g_sysexe_seq __attribute__((unused)) = 0;
static uint8_t  g_sysexe_rvn __attribute__((unused)) = 0, g_sysexe_nmx __attribute__((unused)) = 0;
/* [SYS0.SYSCOMMON.SYSLIB] on SYSVOL_UNIT — where the activated image's --use'd
 * shareables (LIBVMS$SHR.EXE et al.) are resolved by IMGACT (vms-c09f). IMGACT's
 * load_needed() resolves a SONAME to /vms/SYS0/SYSCOMMON/SYSLIB/<soname> and
 * opens it via imgact_acp_open(g_acp_sysdevice, ...) — i.e. OFF the sysdevice
 * VOLUME over the ACP (the "/vms" is the ODS-2 spelling on OVMX_SYSDEVICE, NOT a
 * Linux POSIX read), so the shareables must LIVE on this volume, not lean on any
 * /vms fallback. Only DECC$SHR.EXE is mastered here at image-build (mkimage_
 * ods2_sysvol); the rest are staged at runtime by sysvol_stage_shareables_from(). */
static uint16_t g_syslib_num __attribute__((unused)) = 0, g_syslib_seq __attribute__((unused)) = 0;
static uint8_t  g_syslib_rvn __attribute__((unused)) = 0, g_syslib_nmx __attribute__((unused)) = 0;
static int      g_sysvol_ready __attribute__((unused)) = 0;

/* Walk MFD -> <dirs[0]>.DIR -> <dirs[1]>.DIR -> ... over the ACP on `chan`,
 * returning the FINAL directory's FID. dirs[] is NULL-terminated (names without
 * the ".DIR" type). Leaves the channel with no accessed file. Returns a VMS
 * status. */
__attribute__((unused))
static uint32_t sysvol_resolve_dir_fid(uint32_t chan, const char *const *dirs,
                                       uint16_t *num, uint16_t *seq,
                                       uint8_t *rvn, uint8_t *nmx)
{
    uint16_t d_num = 0, d_seq = 0;
    uint8_t  d_rvn = 0, d_nmx = 0;   /* 0/0/0 => MFD */
    for (int i = 0; dirs[i]; i++) {
        struct vms_acp_access_args a;
        memset(&a, 0, sizeof(a));
        a.chan = chan;
        a.did_num = d_num; a.did_seq = d_seq; a.did_rvn = d_rvn; a.did_nmx = d_nmx;
        a.version = 0;
        snprintf(a.name, VMS_ACP_NAME_SIZE, "%s.DIR", dirs[i]);
        uint32_t st = vms_kif_acp_access(&a);
        if (!$VMS_STATUS_SUCCESS(st))
            return st;
        d_num = a.fid_num; d_seq = a.fid_seq; d_rvn = a.fid_rvn; d_nmx = a.fid_nmx;
        (void)vms_kif_acp_deaccess(chan);
    }
    *num = d_num; *seq = d_seq; *rvn = d_rvn; *nmx = d_nmx;
    return SS$_NORMAL;
}

/* Mount SYSVOL_UNIT and resolve [SYS0.SYSCOMMON.SYSEXE]'s FID (into the
 * g_sysexe_* globals). Idempotent; sets g_sysvol_ready on success. Returns 0 on
 * success. The volume is LEFT MOUNTED so IMGACT can $ASSIGN + IO$_ACCESS an
 * image off it at activation time. */
__attribute__((unused))
static int sysvol_prepare(void)
{
    if (g_sysvol_ready)
        return 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_mount(SYSVOL_UNIT)))
        return -1;
    uint32_t chan = 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_assign(SYSVOL_UNIT, &chan)) || chan == 0)
        return -1;
    static const char *const exe_tree[] = { "SYS0", "SYSCOMMON", "SYSEXE", NULL };
    uint32_t st = sysvol_resolve_dir_fid(chan, exe_tree, &g_sysexe_num, &g_sysexe_seq,
                                         &g_sysexe_rvn, &g_sysexe_nmx);
    if (!$VMS_STATUS_SUCCESS(st) || g_sysexe_num == 0) {
        (void)vms_kif_dassgn(chan);
        return -1;
    }
    /* Also resolve [SYS0.SYSCOMMON.SYSLIB] so shareables can be staged there. */
    static const char *const lib_tree[] = { "SYS0", "SYSCOMMON", "SYSLIB", NULL };
    st = sysvol_resolve_dir_fid(chan, lib_tree, &g_syslib_num, &g_syslib_seq,
                                &g_syslib_rvn, &g_syslib_nmx);
    (void)vms_kif_dassgn(chan);
    if (!$VMS_STATUS_SUCCESS(st) || g_syslib_num == 0)
        return -1;
    g_sysvol_ready = 1;
    return 0;
}

/* Write `bytes[0..len)` as [SYS0.SYSCOMMON.SYSEXE]<name> on SYSVOL_UNIT,
 * byte-exact, over the executive Files-11 ACP: delete any prior version,
 * IO$_CREATE + IO$_ACCESS(write), IO$_WRITEVBLK the bytes block by block
 * (implicit extend allocates from BITMAP.SYS), padding the final block with
 * zeros. The image's valid byte count becomes a whole number of blocks (>= len),
 * which is all IMGACT needs -- it reads header/phdrs/sections/PT_LOAD at offsets
 * < len (imgact_acp_pread clamps at f.valid). Requires sysvol_prepare() first.
 * Returns 0 on success. */
__attribute__((unused))
static int sysvol_write_into(uint16_t dnum, uint16_t dseq, uint8_t drvn, uint8_t dnmx,
                            const char *name, const uint8_t *bytes, long len)
{
    if (!g_sysvol_ready || len <= 0 || !name || dnum == 0)
        return -1;
    uint32_t chan = 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_assign(SYSVOL_UNIT, &chan)) || chan == 0)
        return -1;

    /* Best-effort: delete any prior versions from an earlier stage in this VM. */
    for (int v = 0; v < 8; v++) {
        struct vms_acp_fileop_args df;
        memset(&df, 0, sizeof(df));
        df.chan = chan; df.func = VMS_ACP_FOP_DELETE; df.modifiers = VMS_ACP_M_DELETE;
        df.did_num = dnum; df.did_seq = dseq;
        df.did_rvn = drvn; df.did_nmx = dnmx;
        df.version = 0;   /* highest */
        strncpy(df.name, name, VMS_ACP_NAME_SIZE - 1);
        if (!$VMS_STATUS_SUCCESS(vms_kif_acp_fileop(&df)))
            break;
    }

    /* IO$_CREATE a fresh ;1 (dir entry + real FID), then IO$_ACCESS it for
     * WRITE by name -- the exact create->access(write)->writevb pattern
     * test_syssvc_acp_create.c proves. */
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_CREATE; f.modifiers = VMS_ACP_M_CREATE;
    f.kind = ODS2_FK_DATA_FIX;
    f.did_num = dnum; f.did_seq = dseq;
    f.did_rvn = drvn; f.did_nmx = dnmx;
    f.version = 1;
    strncpy(f.name, name, VMS_ACP_NAME_SIZE - 1);
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_fileop(&f))) {
        (void)vms_kif_dassgn(chan);
        return -1;
    }

    struct vms_acp_access_args a;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = dnum; a.did_seq = dseq;
    a.did_rvn = drvn; a.did_nmx = dnmx;
    a.version = 0;   /* highest */
    a.acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(a.name, name, VMS_ACP_NAME_SIZE - 1);
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_access(&a))) {
        (void)vms_kif_dassgn(chan);
        return -1;
    }

    /* IO$_WRITEVBLK block by block (last block zero-padded to 512). */
    static uint8_t blk[512];
    long off = 0;
    uint32_t vbn = 1;
    int ok = 1;
    while (off < len) {
        uint32_t chunk = (len - off > 512) ? 512u : (uint32_t)(len - off);
        if (chunk < 512)
            memset(blk, 0, sizeof(blk));
        memcpy(blk, bytes + off, chunk);
        struct vms_acp_rw_args r;
        memset(&r, 0, sizeof(r));
        r.chan = chan; r.vbn = vbn; r.offset = 0; r.length = 512;
        r.buffer = (uint64_t)(uintptr_t)blk;
        uint32_t st = vms_kif_acp_writevb(&r);
        if (!$VMS_STATUS_SUCCESS(st) || r.xferred != 512) { ok = 0; break; }
        off += chunk; vbn++;
    }
    (void)vms_kif_acp_deaccess(chan);
    (void)vms_kif_dassgn(chan);
    return ok ? 0 : -1;
}

/* Write `bytes` as [SYS0.SYSCOMMON.SYSEXE]<name> (an image/subject). */
__attribute__((unused))
static int sysvol_write_image(const char *name, const uint8_t *bytes, long len)
{
    return sysvol_write_into(g_sysexe_num, g_sysexe_seq, g_sysexe_rvn, g_sysexe_nmx,
                             name, bytes, len);
}

/* Write `bytes` as [SYS0.SYSCOMMON.SYSLIB]<name> (a --use'd shareable). */
__attribute__((unused))
static int sysvol_write_syslib(const char *name, const uint8_t *bytes, long len)
{
    return sysvol_write_into(g_syslib_num, g_syslib_seq, g_syslib_rvn, g_syslib_nmx,
                             name, bytes, len);
}

/* Read the whole file at `host_path` (a POSIX path -- the initramfs copy the
 * Dockerfile staged, which the Linux kernel also execve's) into a buffer and
 * write it onto SYSVOL_UNIT as [SYS0.SYSCOMMON.SYSEXE]<name> over the ACP. This
 * is the harness placing its subject image on the system disk -- exactly as
 * test_syssvc_mmk_build writes the OVMXRT it built onto the volume -- NOT a
 * runtime /vms fallback. Requires sysvol_prepare() first. Returns 0 on success. */
/* Read the whole POSIX file at `host_path` into a malloc'd buffer (caller frees).
 * Returns the byte count on success, -1 on error. */
__attribute__((unused))
static long sysvol_read_host(const char *host_path, uint8_t **out)
{
    *out = NULL;
    int fd = open(host_path, O_RDONLY);
    if (fd < 0)
        return -1;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || lseek(fd, 0, SEEK_SET) != 0) { close(fd); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { close(fd); return -1; }
    long got = 0;
    while (got < sz) {
        ssize_t n = read(fd, buf + got, (size_t)(sz - got));
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    if (got != sz) { free(buf); return -1; }
    *out = buf;
    return (long)sz;
}

__attribute__((unused))
static int sysvol_stage_host_image(const char *host_path, const char *name)
{
    uint8_t *buf = NULL;
    long len = sysvol_read_host(host_path, &buf);
    if (len < 0)
        return -1;
    int rc = sysvol_write_image(name, buf, len);
    free(buf);
    return rc;
}

/* Stage every "*$SHR.EXE" shareable found in the POSIX directory `host_syslib_dir`
 * (the initramfs SYS$LIBRARY the Dockerfile's full-mode mk_mmk_native_staged.sh
 * built the OVMX shareable graph into) onto SYSVOL_UNIT [SYS0.SYSCOMMON.SYSLIB]
 * over the ACP, so an activated image's IMGACT load_needed() resolves each
 * --use'd shareable off the VOLUME (vms-c09f), not a /vms fallback. DECC$SHR.EXE
 * is SKIPPED: it is mastered onto the volume at image-build (mkimage_ods2_sysvol)
 * and an activated MMK already resolves it there -- overwriting it with the
 * full-mode copy could diverge from the mastered bytes other consumers expect.
 * Requires sysvol_prepare() first. Returns the number staged, or -1 on any error
 * (so one shareable short reddens rather than silently activating a partial set). */
__attribute__((unused))
static int sysvol_stage_shareables_from(const char *host_syslib_dir)
{
    if (sysvol_prepare() != 0)
        return -1;
    DIR *d = opendir(host_syslib_dir);
    if (!d)
        return -1;
    int staged = 0, err = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        size_t nl = strlen(nm);
        /* match "*$SHR.EXE" */
        if (nl < 9 || strcmp(nm + nl - 8, "$SHR.EXE") != 0)
            continue;
        if (strcmp(nm, "DECC$SHR.EXE") == 0)
            continue;                     /* mastered on the volume already */
        char host[512];
        snprintf(host, sizeof(host), "%s/%s", host_syslib_dir, nm);
        uint8_t *buf = NULL;
        long len = sysvol_read_host(host, &buf);
        if (len < 0) { err = 1; break; }
        int rc = sysvol_write_syslib(nm, buf, len);
        free(buf);
        if (rc != 0) { err = 1; break; }
        staged++;
    }
    closedir(d);
    return err ? -1 : staged;
}

/* Mount + resolve SYSVOL_UNIT, stage the activated subject image `host_path`
 * onto it as [SYS0.SYSCOMMON.SYSEXE]<name> over the ACP, and point IMGACT at
 * that volume (OVMX_SYSDEVICE). After this, execl(host_path) activates: the
 * Linux kernel maps PT_LOAD + PT_INTERP=IMGACT.EXE from the host_path POSIX
 * copy, and IMGACT re-reads the genuine main-image bytes off the volume over the
 * ACP -- resolving the on-volume copy this staged (INV-6). Returns 0 on success. */
__attribute__((unused))
static int sysvol_stage_subject(const char *host_path, const char *name)
{
    if (sysvol_prepare() != 0)
        return -1;
    if (sysvol_stage_host_image(host_path, name) != 0)
        return -1;
    setenv("OVMX_SYSDEVICE", SYSVOL_UNIT, 1);
    return 0;
}

#endif /* OVMX_TESTS_QEMU_SYSVOL_STAGE_H */
