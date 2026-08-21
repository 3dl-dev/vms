/*
 * ovmx_boot_sysgen_acp.c - PID 1's SYS$SYSTEM:OVMXVMSSYS.PAR I/O over the
 * executive Files-11 (ODS-2) ACP, for the conversational SYSBOOT> path
 * (vms-46c, residual of the atomic flip epic vms-208).
 *
 * THE RESIDUAL THIS CLOSES. The atomic flip made SYS$DISK a genuine ODS-2
 * volume the executive ACP owns and retired the /vms POSIX passthrough. The
 * flagless boot already reads OVMXVMSSYS.PAR over the ACP (stage_boot_images /
 * ovmx_boot_acp_read.c). But the CONVERSATIONAL branch (bare_metal_init's
 * SYSBOOT> path) still legacy-mounted vmsfs and let sysboot.c read/write the
 * parameter file with vmsfs_to_linux_path()+fopen() off /vms -- which no longer
 * exists on the runtime path. So a SYSBOOT> WRITE never reached the real volume
 * and a SYSBOOT> read saw nothing. This module routes SYSBOOT's parameter I/O
 * through the SAME executive ACP every other on-volume file now uses.
 *
 * WHY THIS TU AND NOT src/vmsrms/sysgen_acp.c. sysgen_acp.c is the STRONG
 * definition of the ovmx_sysgen_acp_* seam for images that link LIBVMSRMS$SHR
 * (SYSGEN.EXE / SCSD.EXE / DCL.EXE, ...); it reaches the volume through
 * rms_open_named_handle() (rms_core.c) + rms_io (vms_kif_acp_*). STARTUP.EXE is
 * PID 1 -- statically linked (-static), and it links NO RMS: pulling rms_core.o
 * (the full FAB/RAB/idx record engine + concealed-logical spec expansion) into
 * the static musl PID-1 image to reach one fixed 9 KB blob is a large blast
 * radius. Instead this TU supplies the SAME two seam symbols
 * (ovmx_sysgen_acp_read / ovmx_sysgen_acp_write) as an ALTERNATE strong
 * definition backed by the imgact_acp.c ACP client that IS already linked into
 * PID 1 (the flip's image-staging bridge) -- IO$_ACCESS/IO$_READVBLK for read,
 * a self-contained IO$_CREATE/IO$_WRITEVBLK for write, all over /dev/vms. Two
 * definitions of one seam for two link contexts: exactly the freestanding-vs-
 * hosted seam pattern imgact_acp.c itself uses. ovmx_init does not link vmsrms,
 * so there is no duplicate-symbol clash.
 *
 * The shared sysgen_params.h reader/writer machinery (sysgen_load_working /
 * sysgen_commit_working) calls this seam, so SYSBOOT uses the EXACT reader every
 * other consumer uses -- it adds no parser of its own (vms-9b7 spirit).
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6). Every entry point fails honestly --
 * SS$_NOSUCHDEV when /dev/vms is unreachable or the boot volume is not
 * ACP-mounted, SS$_NOSUCHFILE when the file is not on the volume, SS$_DATACHECK
 * on a short/corrupt read -- and NEVER falls back to a /vms POSIX read/write.
 *
 * LINUX SUBSTRATE ONLY. The atomic flip of SYS$DISK to a genuine ODS-2 ACP
 * volume is the Linux flip; the NetBSD-vax boot path keeps its own parameter I/O
 * until vms-d5d flips it, so this TU compiles in only on the Linux backend
 * (CMakeLists guards it by OVMX_BOOT_LINUX, same as ovmx_boot_acp_read.c).
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The ACP ioctl arg structs / request numbers
 * are OVMX design choices labelled as such in src/kernel/vms_acp.h; this file
 * only marshals into them. It reads no OpenVMS byte-level wire.
 */

/* This is a DEFINING TU of the seam: keep ovmx_sysgen_acp_read/write STRONG so
 * they are not #pragma-weaked by sysgen_params.h (a weak definition would let
 * the inline callers see NULL). Must be set BEFORE sysgen_params.h. */
#define OVMX_SYSGEN_ACP_STRONG 1

#include <stdint.h>

/* One include pulls in the _IOWR encoding, VMS_IOC_MAGIC, VMS_DEVNAM_SIZE, and
 * every ACP / register / dassgn arg struct + request number. */
#include "vms_ioctl.h"
#include "ssdef.h"
#include "vmsfs/ods2.h"      /* ODS2_FK_DATA (RFM=VAR data-file kind for CREATE) */
#include "ovmx_layout.h"     /* SYSDISK_DEVICE, VMS_SYSTEM_DIR                   */
#include "sysgen_params.h"   /* struct sysgen_file, SYSGEN_MAGIC/VERSION, seam   */

/* imgact_acp.c is compiled into PID 1 (the flip's image-staging bridge) and its
 * three host primitives (imgact_acp_dev_open/close/ioctl) are libc-backed in
 * ovmx_boot_acp_read.c. Reuse both for the read path and the raw write ioctls. */
#include "imgact_acp.h"

/* The ACP-mounted boot/system unit (DKA0:) -- the same unit PID 1 $MOUNTs and
 * IMGACT.EXE activates images from. */
#define BOOT_SYSDISK_UNIT   SYSDISK_DEVICE ":"

/* SYS$SYSTEM as a '/'-separated ODS-2 path into the boot volume, plus the
 * parameter file. VMS_SYSTEM_DIR is SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE";
 * imgact_acp.c strips a leading "/vms" mount-point component. This form needs
 * NO device table / logical-name resolution -- which is exactly why SYSBOOT can
 * use it this early in bare_metal_init() (sysboot.h's header). */
#define BOOT_PARAMS_ACP_PATH   VMS_SYSTEM_DIR "/OVMXVMSSYS.PAR"

/* ==========================================================================
 * READ -- reuse the imgact_acp.c ACP client verbatim (IO$_ACCESS + IO$_READVBLK).
 * ========================================================================== */

uint32_t ovmx_sysgen_acp_read(struct sysgen_file *out)
{
    struct imgact_acp_file f;
    uint32_t st;

    if (!out)
        return SS$_BADPARAM;

    st = imgact_acp_open(&f, BOOT_SYSDISK_UNIT, BOOT_PARAMS_ACP_PATH);
    if (!$VMS_STATUS_SUCCESS(st))
        return st;                     /* SS$_NOSUCHFILE / SS$_NOSUCHDEV */

    struct sysgen_file tmp;
    unsigned long total = 0;
    while (total < sizeof(tmp)) {
        long r = imgact_acp_pread(&f, (char *)&tmp + total,
                                  sizeof(tmp) - total, (long)total);
        if (r < 0) {
            imgact_acp_close(&f);
            return SS$_DATACHECK;
        }
        if (r == 0)
            break;                     /* EOF before a full record */
        total += (unsigned long)r;
    }
    imgact_acp_close(&f);

    if (total < sizeof(tmp))
        return SS$_DATACHECK;          /* short/truncated file */
    if (tmp.magic != SYSGEN_MAGIC || tmp.version != SYSGEN_VERSION)
        return SS$_DATACHECK;
    if (tmp.count > SYSGEN_MAX_PARAMS)
        tmp.count = SYSGEN_MAX_PARAMS;

    *out = tmp;
    return SS$_NORMAL;
}

/* ==========================================================================
 * WRITE -- self-contained raw ACP client: $ASSIGN + directory walk +
 * IO$_CREATE (highest+1) + IO$_WRITEVBLK, over /dev/vms.
 *
 * The register/assign/directory-walk marshaling mirrors imgact_acp.c's read
 * path exactly (same ioctls, same DID-chaining); the CREATE fileop mirrors
 * rms_core.c's ACP create (func=CREATE, modifiers=CREATE|ACCESS, acctl=WRITE,
 * kind=ODS2_FK_DATA, version 0 => highest+1). imgact_acp.c has no write path,
 * so this is the one piece not reused.
 * ========================================================================== */

static void boot_acp_memset(void *d, int c, unsigned long n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
}

static unsigned long boot_acp_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (unsigned long)(p - s);
}

static void boot_acp_strlcpy(char *dst, const char *src, unsigned long cap)
{
    unsigned long i = 0;
    if (cap == 0)
        return;
    for (; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Pull the next '/'-separated component out of *pp into buf; skip leading
 * slashes; return the component length (0 when the path is exhausted). */
static unsigned long boot_next_component(const char **pp, char *buf,
                                         unsigned long cap)
{
    const char *p = *pp;
    unsigned long n = 0;
    while (*p == '/')
        p++;
    while (*p && *p != '/') {
        if (n + 1 < cap)
            buf[n] = *p;
        n++;
        p++;
    }
    buf[n < cap ? n : cap - 1] = '\0';
    *pp = p;
    return n;
}

static uint32_t boot_acp_register(int fd)
{
    struct vms_register_args a;
    boot_acp_memset(&a, 0, sizeof(a));
    if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_REGISTER, &a) < 0)
        return SS$_NOSUCHDEV;
    return a.status;
}

static uint32_t boot_acp_assign(int fd, const char *dev, uint32_t *chan)
{
    struct vms_acp_assign_args a;
    boot_acp_memset(&a, 0, sizeof(a));
    boot_acp_strlcpy(a.devnam, dev, sizeof(a.devnam));
    if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_ASSIGN, &a) < 0)
        return SS$_NOSUCHDEV;
    if ($VMS_STATUS_SUCCESS(a.status))
        *chan = a.chan;
    return a.status;
}

static void boot_acp_dassgn(int fd, uint32_t chan)
{
    struct vms_dassgn_args a;
    boot_acp_memset(&a, 0, sizeof(a));
    a.chan = chan;
    (void)imgact_acp_dev_ioctl(fd, VMS_IOCTL_DASSGN, &a);
}

static void boot_acp_deaccess(int fd, uint32_t chan)
{
    struct vms_acp_deaccess_args a;
    boot_acp_memset(&a, 0, sizeof(a));
    a.chan = chan;
    (void)imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_DEACCESS, &a);
}

/* IO$_ACCESS one directory component ("NAME.DIR") in DID {d_*}; return its FID
 * in *f_* on success. Leaves the directory ACCESSED; caller deaccesses. */
static uint32_t boot_acp_access_dir(int fd, uint32_t chan, const char *dirname,
                                    uint16_t d_num, uint16_t d_seq,
                                    uint8_t d_rvn, uint8_t d_nmx,
                                    uint16_t *f_num, uint16_t *f_seq,
                                    uint8_t *f_rvn, uint8_t *f_nmx)
{
    struct vms_acp_access_args a;
    boot_acp_memset(&a, 0, sizeof(a));
    a.chan    = chan;
    a.did_num = d_num;
    a.did_seq = d_seq;
    a.did_rvn = d_rvn;
    a.did_nmx = d_nmx;
    boot_acp_strlcpy(a.name, dirname, sizeof(a.name));
    if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_ACCESS, &a) < 0)
        return SS$_NOSUCHDEV;
    if ($VMS_STATUS_SUCCESS(a.status)) {
        if (f_num) *f_num = a.fid_num;
        if (f_seq) *f_seq = a.fid_seq;
        if (f_rvn) *f_rvn = a.fid_rvn;
        if (f_nmx) *f_nmx = a.fid_nmx;
    }
    return a.status;
}

/*
 * Walk BOOT_PARAMS_ACP_PATH's DIRECTORY components (everything but the final
 * file component) and leave *d_* holding the FID of the innermost directory
 * (SYS$SYSTEM). The final component (the file name) is returned in file_out.
 * Returns SS$_NORMAL on success.
 */
static uint32_t boot_acp_resolve_parent(int fd, uint32_t chan,
                                        uint16_t *d_num, uint16_t *d_seq,
                                        uint8_t *d_rvn, uint8_t *d_nmx,
                                        char *file_out, unsigned long file_cap)
{
    const char *p = BOOT_PARAMS_ACP_PATH;
    char comp[VMS_ACP_NAME_SIZE];
    char next[VMS_ACP_NAME_SIZE];

    *d_num = 0; *d_seq = 0; *d_rvn = 0; *d_nmx = 0;

    /* Strip a leading "/vms" mount-point component if present. */
    {
        const char *q = p;
        while (*q == '/') q++;
        if ((q[0] == 'v' || q[0] == 'V') &&
            (q[1] == 'm' || q[1] == 'M') &&
            (q[2] == 's' || q[2] == 'S') &&
            (q[3] == '/' || q[3] == '\0'))
            p = q + 3;
    }

    if (boot_next_component(&p, comp, sizeof(comp)) == 0)
        return SS$_NOSUCHFILE;

    for (;;) {
        int has_next = (boot_next_component(&p, next, sizeof(next)) != 0);

        if (!has_next) {
            /* comp is the final component: the file name. */
            boot_acp_strlcpy(file_out, comp, file_cap);
            return SS$_NORMAL;
        }

        /* Intermediate component: an ODS-2 directory "NAME.DIR". */
        {
            char dirname[VMS_ACP_NAME_SIZE];
            uint16_t fnum = 0, fseq = 0;
            uint8_t  frvn = 0, fnmx = 0;
            unsigned long L = boot_acp_strlen(comp);
            uint32_t st;

            boot_acp_strlcpy(dirname, comp, sizeof(dirname));
            if (L + 4 < sizeof(dirname)) {
                dirname[L + 0] = '.';
                dirname[L + 1] = 'D';
                dirname[L + 2] = 'I';
                dirname[L + 3] = 'R';
                dirname[L + 4] = '\0';
            }

            st = boot_acp_access_dir(fd, chan, dirname,
                                     *d_num, *d_seq, *d_rvn, *d_nmx,
                                     &fnum, &fseq, &frvn, &fnmx);
            if (!$VMS_STATUS_SUCCESS(st))
                return st;
            boot_acp_deaccess(fd, chan);

            *d_num = fnum; *d_seq = fseq; *d_rvn = frvn; *d_nmx = fnmx;
        }

        boot_acp_strlcpy(comp, next, sizeof(comp));
    }
}

uint32_t ovmx_sysgen_acp_write(const struct sysgen_file *db, int new_version,
                               int *out_version)
{
    int fd;
    uint32_t st, chan = 0;
    uint16_t d_num, d_seq;
    uint8_t  d_rvn, d_nmx;
    char fname[VMS_ACP_NAME_SIZE];
    struct vms_acp_fileop_args fop;

    (void)new_version;   /* SYSBOOT WRITE always mints a new highest+1 version */

    if (!db)
        return SS$_BADPARAM;

    fd = imgact_acp_dev_open();
    if (fd < 0)
        return SS$_NOSUCHDEV;

    st = boot_acp_register(fd);
    if (!$VMS_STATUS_SUCCESS(st)) { imgact_acp_dev_close(fd); return st; }

    st = boot_acp_assign(fd, BOOT_SYSDISK_UNIT, &chan);
    if (!$VMS_STATUS_SUCCESS(st)) { imgact_acp_dev_close(fd); return st; }

    st = boot_acp_resolve_parent(fd, chan, &d_num, &d_seq, &d_rvn, &d_nmx,
                                 fname, sizeof(fname));
    if (!$VMS_STATUS_SUCCESS(st)) {
        boot_acp_dassgn(fd, chan);
        imgact_acp_dev_close(fd);
        return st;
    }

    /* IO$_CREATE a fresh highest+1 version and ACCESS it for write. */
    boot_acp_memset(&fop, 0, sizeof(fop));
    fop.chan      = chan;
    fop.func      = VMS_ACP_FOP_CREATE;
    fop.modifiers = VMS_ACP_M_CREATE | VMS_ACP_M_ACCESS;
    fop.acctl     = VMS_ACP_ACCTL_WRITE;
    fop.kind      = (uint8_t)ODS2_FK_DATA;   /* RFM=VAR; block I/O is RFM-oblivious */
    fop.did_num   = d_num;
    fop.did_seq   = d_seq;
    fop.did_rvn   = d_rvn;
    fop.did_nmx   = d_nmx;
    fop.version   = 0;                        /* highest existing + 1 */
    boot_acp_strlcpy(fop.name, fname, sizeof(fop.name));

    if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_FILEOP, &fop) < 0) {
        boot_acp_dassgn(fd, chan);
        imgact_acp_dev_close(fd);
        return SS$_NOSUCHDEV;
    }
    if (!$VMS_STATUS_SUCCESS(fop.status)) {
        st = fop.status;
        boot_acp_dassgn(fd, chan);
        imgact_acp_dev_close(fd);
        return st;
    }

    /* IO$_WRITEVBLK the whole blob from VBN 1, offset 0. The ACP handles a
     * length spanning block boundaries in one call (up to 1 MiB); the blob is
     * ~9 KB, so this is a single transfer, but loop defensively. */
    st = SS$_NORMAL;
    {
        const uint8_t *p = (const uint8_t *)db;
        unsigned long total = 0;
        while (total < sizeof(*db)) {
            struct vms_acp_rw_args rw;
            boot_acp_memset(&rw, 0, sizeof(rw));
            rw.chan   = chan;
            rw.vbn    = (uint32_t)(total / 512u) + 1u;
            rw.offset = (uint32_t)(total % 512u);
            rw.length = (uint32_t)(sizeof(*db) - total);
            rw.buffer = (uint64_t)(uintptr_t)(p + total);

            if (imgact_acp_dev_ioctl(fd, VMS_IOCTL_ACP_WRITEVBLK, &rw) < 0) {
                st = SS$_ABORT;
                break;
            }
            if (!$VMS_STATUS_SUCCESS(rw.status)) {
                st = rw.status;
                break;
            }
            if (rw.xferred == 0) {
                st = SS$_ABORT;
                break;
            }
            total += rw.xferred;
        }
    }

    if ($VMS_STATUS_SUCCESS(st) && out_version)
        *out_version = (int)fop.out_version;

    boot_acp_deaccess(fd, chan);
    boot_acp_dassgn(fd, chan);
    imgact_acp_dev_close(fd);
    return st;
}
