/*
 * rms_textfile.c - sequential VMS text-file reader over the Files-11 ODS-2 ACP
 * (epic vms-208, vms-274). See rms_textfile.h for the contract and the reason
 * the SYSUAF/RIGHTSLIST/$GETUAI readers reach their file this way now.
 *
 * __linux__  : RMS $OPEN/$CONNECT/$GET/$CLOSE. rms_impl_open (src/vmsrms,
 *              vms-bc7) routes these to $ASSIGN + IO$_ACCESS + IO$_READVBLK on
 *              the mounted ODS-2 volume in the executive -- no _linux_fd, no
 *              POSIX. Fail-honest: a failed $OPEN (no mounted ACP volume /
 *              no /dev/vms / no such file) yields NULL, never a POSIX fallback.
 * !__linux__ : the netbsd-vax standalone cross keeps POSIX fopen/fgets until the
 *              VAX mount retires (vms-d5d).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdlib.h>
#include <string.h>

#include "rms_textfile.h"

#if defined(__linux__)

#include "rms/rms.h"
#include "vmsfs/device.h"

/*
 * WEAK REFERENCES TO THE RMS SERVICES -- the library-layering seam.
 *
 * These readers/writers live in LIBVMS (src/libvms), which sits BELOW RMS:
 * LIBVMSRMS$SHR links LIBVMS, not the other way round. A hard reference to
 * sys$open/... from here would invert that layering and force every one of the
 * dozens of images and tests that link LIBVMS to also link LIBVMSRMS -- and no
 * static/shared cycle could satisfy it. So the RMS services are referenced
 * WEAKLY: LIBVMS$SHR builds and loads with no dependency on LIBVMSRMS$SHR.
 *
 *   - An image that ALSO links vmsrms (LOGINOUT, VMSSSHD, DCL, and the QEMU
 *     facility tests -- see their CMake) binds the real RMS services here and
 *     reads/writes SYSUAF et al. through the ODS-2 ACP.
 *   - An image that does NOT (a bare libvms unit test with no /dev/vms) sees
 *     the symbols as NULL; rms_textfile_open()/append/write then fail honestly,
 *     exactly as they would against an absent ACP volume (Rule 9 / INV-6) --
 *     never a crash, never a silent POSIX fallback.
 */
#pragma weak sys$open
#pragma weak sys$close
#pragma weak sys$connect
#pragma weak sys$disconnect
#pragma weak sys$get
#pragma weak sys$put
#pragma weak sys$create

/* True only when LIBVMSRMS is present in this image's link closure. */
static int rms_services_present(void)
{
    return sys$open && sys$close && sys$connect && sys$get;
}

/* Read records supplied as stream-LF (one line == one record, LF-delimited,
 * no embedded count). SYSUAF.DAT / RIGHTSLIST.DAT are line-oriented text; the
 * boot-master ODS-2 volume writes them stream-LF. (FAT-persist of the file's
 * RFM is a later rung; until it lands the reader supplies the format, exactly
 * as tests/qemu/test_syssvc_rms_acp.c does for the read side.) */
struct rms_textfile {
    struct FAB fab;
    struct RAB rab;
    char       spec[512];      /* resolved filespec; fab$l_fna points at it     */
    char       rbuf[1024];     /* per-$GET record buffer                        */
    int        connected;
};

rms_textfile_t *rms_textfile_open(const char *vms_spec)
{
    if (!vms_spec || !*vms_spec)
        return NULL;
    if (!rms_services_present())
        return NULL;   /* no LIBVMSRMS in this image -> fail honest */

    rms_textfile_t *tf = calloc(1, sizeof(*tf));
    if (!tf)
        return NULL;

    /* Resolve device-field logical names (SYS$SYSTEM:, SYS$SYSDEVICE:, ...)
     * through LNM$FILE_DEV before the volume is $ASSIGNed -- the same
     * resolution $PARSE performs, so the ACP $ASSIGNs a concrete mounted unit
     * rather than a logical it cannot open. A no-op translation passes the
     * spec through unchanged. */
    if (vmsfs_resolve_filespec_device(vms_spec, tf->spec, sizeof(tf->spec))
            != SS$_NORMAL || tf->spec[0] == '\0') {
        /* Fall back to the literal spec; RMS still fails honestly if the
         * device cannot be $ASSIGNed. */
        strncpy(tf->spec, vms_spec, sizeof(tf->spec) - 1);
        tf->spec[sizeof(tf->spec) - 1] = '\0';
    }

    tf->fab = cc$rms_fab;
    tf->fab.fab$l_fna = tf->spec;
    tf->fab.fab$b_fns = (uint8_t)strlen(tf->spec);
    tf->fab.fab$b_org = FAB$C_SEQ;
    tf->fab.fab$b_rfm = FAB$C_STMLF;
    tf->fab.fab$b_fac = FAB$M_GET;

    if (sys$open(&tf->fab, 0, 0) != RMS$_NORMAL) {
        free(tf);
        return NULL;
    }

    tf->rab = cc$rms_rab;
    tf->rab.rab$l_fab = &tf->fab;
    tf->rab.rab$l_ubf = tf->rbuf;
    tf->rab.rab$w_usz = (uint16_t)sizeof(tf->rbuf);

    if (sys$connect(&tf->rab, 0, 0) != RMS$_NORMAL) {
        sys$close(&tf->fab, 0, 0);
        free(tf);
        return NULL;
    }
    tf->connected = 1;
    return tf;
}

int rms_textfile_getline(rms_textfile_t *tf, char *buf, size_t bufsz, int *too_long)
{
    if (too_long)
        *too_long = 0;
    if (!tf || !buf || bufsz < 2)
        return 0;

    if (sys$get(&tf->rab, 0, 0) != RMS$_NORMAL)
        return 0;   /* RMS$_EOF or any read error -> no more records */

    size_t rsz = tf->rab.rab$w_rsz;
    size_t copy = rsz;
    if (copy >= bufsz) {
        copy = bufsz - 1;
        if (too_long)
            *too_long = 1;
    }
    memcpy(buf, tf->rbuf, copy);
    buf[copy] = '\0';
    return 1;
}

void rms_textfile_close(rms_textfile_t *tf)
{
    if (!tf)
        return;
    if (tf->connected)
        sys$disconnect(&tf->rab, 0, 0);
    sys$close(&tf->fab, 0, 0);
    free(tf);
}

/* Resolve device-field logicals into `out` (see rms_textfile_open). */
static void rmstf_resolve(const char *spec, char *out, size_t outsz)
{
    if (vmsfs_resolve_filespec_device(spec, out, outsz) != SS$_NORMAL
            || out[0] == '\0') {
        strncpy(out, spec, outsz - 1);
        out[outsz - 1] = '\0';
    }
}

/* Shared $PUT of one stream-LF record on an open+connected FAB/RAB. */
static int rmstf_put(struct FAB *fab, const char *line, uint32_t rop)
{
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = fab;
    rab.rab$l_rop = rop;
    if (sys$connect(&rab, 0, 0) != RMS$_NORMAL)
        return -1;
    rab.rab$l_rbf = (char *)line;
    rab.rab$w_rsz = (uint16_t)strlen(line);
    uint32_t st = sys$put(&rab, 0, 0);
    sys$disconnect(&rab, 0, 0);
    return (st == RMS$_NORMAL) ? 0 : -1;
}

int rms_textfile_append_line(const char *vms_spec, const char *line)
{
    if (!vms_spec || !*vms_spec || !line)
        return -1;
    if (!rms_services_present() || !sys$create || !sys$put)
        return -1;   /* no LIBVMSRMS -> fail honest, no POSIX write */

    char spec[512];
    rmstf_resolve(vms_spec, spec, sizeof(spec));

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_STMLF;
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;

    /* Append to the existing OPERATOR.LOG, or create it on first write. */
    if (sys$open(&fab, 0, 0) != RMS$_NORMAL &&
        sys$create(&fab, 0, 0) != RMS$_NORMAL)
        return -1;

    /* RAB$M_EOF positions the record context at end-of-file so the $PUT lands
     * as an append (IO$_WRITEVBLK past EOF with implicit extend). */
    int rc = rmstf_put(&fab, line, RAB$M_EOF);
    sys$close(&fab, 0, 0);
    return rc;
}

int rms_textfile_write_line(const char *vms_spec, const char *line)
{
    if (!vms_spec || !*vms_spec || !line)
        return -1;
    if (!rms_services_present() || !sys$create || !sys$put)
        return -1;   /* no LIBVMSRMS -> fail honest, no POSIX write */

    char spec[512];
    rmstf_resolve(vms_spec, spec, sizeof(spec));

    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_STMLF;
    fab.fab$b_fac = FAB$M_PUT;

    if (sys$create(&fab, 0, 0) != RMS$_NORMAL)
        return -1;

    int rc = rmstf_put(&fab, line, 0);
    sys$close(&fab, 0, 0);
    return rc;
}

#else  /* !__linux__ : netbsd-vax standalone cross keeps POSIX (vms-d5d) */

#include <stdio.h>
#include "vmsfs/filespec.h"

struct rms_textfile {
    FILE *fp;
};

rms_textfile_t *rms_textfile_open(const char *vms_spec)
{
    if (!vms_spec || !*vms_spec)
        return NULL;
    char linux_path[1024];
    vmsfs_to_linux_path(vms_spec, linux_path, sizeof(linux_path));
    FILE *fp = fopen(linux_path, "r");
    if (!fp)
        return NULL;
    rms_textfile_t *tf = calloc(1, sizeof(*tf));
    if (!tf) { fclose(fp); return NULL; }
    tf->fp = fp;
    return tf;
}

int rms_textfile_getline(rms_textfile_t *tf, char *buf, size_t bufsz, int *too_long)
{
    if (too_long)
        *too_long = 0;
    if (!tf || !tf->fp || !buf || bufsz < 2)
        return 0;
    if (!fgets(buf, (int)bufsz, tf->fp))
        return 0;
    size_t len = strlen(buf);
    if (len + 1 == bufsz && buf[len - 1] != '\n') {
        int c;
        while ((c = fgetc(tf->fp)) != EOF && c != '\n')
            ;
        if (too_long)
            *too_long = 1;
    }
    return 1;
}

void rms_textfile_close(rms_textfile_t *tf)
{
    if (!tf)
        return;
    if (tf->fp)
        fclose(tf->fp);
    free(tf);
}

static int rmstf_posix_put(const char *vms_spec, const char *line, const char *mode)
{
    if (!vms_spec || !*vms_spec || !line)
        return -1;
    char linux_path[1024];
    vmsfs_to_linux_path(vms_spec, linux_path, sizeof(linux_path));
    FILE *f = fopen(linux_path, mode);
    if (!f)
        return -1;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

int rms_textfile_append_line(const char *vms_spec, const char *line)
{
    return rmstf_posix_put(vms_spec, line, "a");
}

int rms_textfile_write_line(const char *vms_spec, const char *line)
{
    return rmstf_posix_put(vms_spec, line, "w");
}

#endif /* __linux__ */
