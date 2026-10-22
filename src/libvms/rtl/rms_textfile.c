/*
 * rms_textfile.c - sequential VMS text-file reader over the Files-11 ODS-2 ACP
 * (epic vms-208, vms-274). See rms_textfile.h for the contract and the reason
 * the SYSUAF/RIGHTSLIST/$GETUAI readers reach their file this way now.
 *
 * OVMX_HAVE_ACP  : RMS $OPEN/$CONNECT/$GET/$CLOSE. rms_impl_open (src/vmsrms,
 *              vms-bc7) routes these to $ASSIGN + IO$_ACCESS + IO$_READVBLK on
 *              the mounted ODS-2 volume in the executive -- no _linux_fd, no
 *              POSIX. Fail-honest: a failed $OPEN (no mounted ACP volume /
 *              no /dev/vms / no such file) yields NULL, never a POSIX fallback.
 * !OVMX_HAVE_ACP : no executive in this link at all (a bare libvms unit build).
 *              POSIX fopen/fgets.
 *
 * KEYED ON OVMX_HAVE_ACP, NOT __linux__ (vms-329). The gate used to read
 * __linux__ because the netbsd-vax runtime still VFS-mounted SYS$DISK and so
 * still had a /vms POSIX tree to fopen(); the coupled VAX cutover retired that
 * mount, so on the VAX the POSIX arm reads a path that does not exist -- every
 * SYSUAF/RIGHTSLIST/$GETUAI consumer would fail (or, worse, read an empty file
 * and decide the account is absent) while the boot claimed to have flipped.
 * Same re-key ovmx_provision.c and dcl_script.c took, for the same reason.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "rms_textfile.h"
#include "ssdef.h"

#if defined(OVMX_HAVE_ACP)

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
    uint32_t   last_st;        /* the last $GET's status (RMS$_EOF at the end)  */
};

static uint32_t rmstf_open_wait(struct FAB *fab);   /* below: $OPEN, waiting out RMS$_SHR */

static rms_textfile_t *rmstf_open_unl(const char *vms_spec, uint32_t *st)
{
    *st = RMS$_FNM;
    if (!vms_spec || !*vms_spec)
        return NULL;
    *st = SS$_NOSUCHDEV;
    if (!rms_services_present())
        return NULL;   /* no LIBVMSRMS in this image: no file layer at all */

    *st = RMS$_DME;
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

    if ((*st = rmstf_open_wait(&tf->fab)) != RMS$_NORMAL) {
        free(tf);
        return NULL;
    }

    tf->rab = cc$rms_rab;
    tf->rab.rab$l_fab = &tf->fab;
    tf->rab.rab$l_ubf = tf->rbuf;
    tf->rab.rab$w_usz = (uint16_t)sizeof(tf->rbuf);

    if ((*st = sys$connect(&tf->rab, 0, 0)) != RMS$_NORMAL) {
        sys$close(&tf->fab, 0, 0);
        free(tf);
        return NULL;
    }
    tf->connected = 1;
    tf->last_st = RMS$_NORMAL;
    *st = RMS$_NORMAL;
    return tf;
}

static int rmstf_getline_unl(rms_textfile_t *tf, char *buf, size_t bufsz, int *too_long)
{
    if (too_long)
        *too_long = 0;
    if (!tf || !buf || bufsz < 2)
        return 0;

    /* RMS$_EOF or a read error: no more records. Which one is kept for
     * rms_textfile_last_status() -- a reader that must not mistake a failed
     * read for the end of the file asks (rd vms-acf3). */
    if ((tf->last_st = sys$get(&tf->rab, 0, 0)) != RMS$_NORMAL)
        return 0;

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

static void rmstf_close_unl(rms_textfile_t *tf)
{
    if (!tf)
        return;
    if (tf->connected)
        sys$disconnect(&tf->rab, 0, 0);
    sys$close(&tf->fab, 0, 0);
    free(tf);
}

/* $OPEN, waiting out a brief share conflict (rd vms-acf3). Another process
 * holding the file for a conflicting access turns an $OPEN away with RMS$_SHR;
 * an operator-log or node-database access is short, so retry for up to
 * ~1 s before reporting it. Any other status is returned at once. */
static uint32_t rmstf_open_wait(struct FAB *fab)
{
    uint32_t st = RMS$_SHR;
    for (int i = 0; i < 100; i++) {
        st = sys$open(fab, 0, 0);
        if (st != RMS$_SHR)
            break;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return st;
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

static int rmstf_append_line_unl(const char *vms_spec, const char *line)
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

    /* Append to the existing file, or create it on first write -- ONLY when it
     * is not there (RMS$_FNF). A file that is there but could not be opened
     * (another accessor holding it past the wait) is a failed append, never
     * a reason to start a NEW VERSION: that split OPERATOR.LOG, leaving this
     * boot's earlier records in the older version (rd vms-acf3). */
    uint32_t ost = rmstf_open_wait(&fab);
    if (ost != RMS$_NORMAL) {
        if (ost != RMS$_FNF)
            return -1;
        fab = cc$rms_fab;
        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        fab.fab$b_org = FAB$C_SEQ;
        fab.fab$b_rfm = FAB$C_STMLF;
        fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;
        if (sys$create(&fab, 0, 0) != RMS$_NORMAL)
            return -1;
    }

    /* RAB$M_EOF positions the record context at end-of-file so the $PUT lands
     * as an append (IO$_WRITEVBLK past EOF with implicit extend). */
    int rc = rmstf_put(&fab, line, RAB$M_EOF);
    sys$close(&fab, 0, 0);
    return rc;
}

static int rmstf_write_line_unl(const char *vms_spec, const char *line)
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

static int rmstf_create_unl(const char *vms_spec)
{
    if (!vms_spec || !*vms_spec)
        return -1;
    if (!rms_services_present() || !sys$create)
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
    sys$close(&fab, 0, 0);
    return 0;
}

/* ONE RMS CALLER AT A TIME (rd vms-158d / vms-c89). An image can reach this
 * layer from two threads: NETACP's SYS$OUTPUT reader (sysoutput_rms.c) files
 * its lines here while the image's main thread files an operator request
 * ($SNDOPR -> OPERATOR.LOG). The RMS record services keep per-process state,
 * and two interleaved $OPEN/$PUT/$CLOSE sequences lost a record (NETACP.LOG
 * without its DATALINK line, measured in CI). Every entry point below holds
 * this lock for its whole RMS sequence. */
static pthread_mutex_t rmstf_lock = PTHREAD_MUTEX_INITIALIZER;

rms_textfile_t *rms_textfile_open_st(const char *vms_spec, uint32_t *status)
{
    uint32_t st = RMS$_NORMAL;
    pthread_mutex_lock(&rmstf_lock);
    rms_textfile_t *tf = rmstf_open_unl(vms_spec, &st);
    pthread_mutex_unlock(&rmstf_lock);
    if (status)
        *status = st;
    return tf;
}

rms_textfile_t *rms_textfile_open(const char *vms_spec)
{
    return rms_textfile_open_st(vms_spec, NULL);
}

uint32_t rms_textfile_last_status(const rms_textfile_t *tf)
{
    return tf ? tf->last_st : RMS$_IFI;
}

int rms_textfile_getline(rms_textfile_t *tf, char *buf, size_t bufsz, int *too_long)
{
    pthread_mutex_lock(&rmstf_lock);
    int rc = rmstf_getline_unl(tf, buf, bufsz, too_long);
    pthread_mutex_unlock(&rmstf_lock);
    return rc;
}

void rms_textfile_close(rms_textfile_t *tf)
{
    pthread_mutex_lock(&rmstf_lock);
    rmstf_close_unl(tf);
    pthread_mutex_unlock(&rmstf_lock);
}

int rms_textfile_append_line(const char *vms_spec, const char *line)
{
    pthread_mutex_lock(&rmstf_lock);
    int rc = rmstf_append_line_unl(vms_spec, line);
    pthread_mutex_unlock(&rmstf_lock);
    return rc;
}

int rms_textfile_write_line(const char *vms_spec, const char *line)
{
    pthread_mutex_lock(&rmstf_lock);
    int rc = rmstf_write_line_unl(vms_spec, line);
    pthread_mutex_unlock(&rmstf_lock);
    return rc;
}

int rms_textfile_create(const char *vms_spec)
{
    pthread_mutex_lock(&rmstf_lock);
    int rc = rmstf_create_unl(vms_spec);
    pthread_mutex_unlock(&rmstf_lock);
    return rc;
}

#else  /* !OVMX_HAVE_ACP : no executive in this link (bare libvms unit build) */

#include <errno.h>
#include <stdio.h>
#include "rms/rms.h"
#include "vmsfs/filespec.h"

struct rms_textfile {
    FILE *fp;
};

rms_textfile_t *rms_textfile_open_st(const char *vms_spec, uint32_t *status)
{
    if (status)
        *status = RMS$_FNM;
    if (!vms_spec || !*vms_spec)
        return NULL;
    char linux_path[1024];
    vmsfs_to_linux_path(vms_spec, linux_path, sizeof(linux_path));
    FILE *fp = fopen(linux_path, "r");
    if (!fp) {
        if (status)
            *status = (errno == ENOENT) ? RMS$_FNF : RMS$_ACC;
        return NULL;
    }
    rms_textfile_t *tf = calloc(1, sizeof(*tf));
    if (!tf) {
        fclose(fp);
        if (status)
            *status = RMS$_DME;
        return NULL;
    }
    tf->fp = fp;
    if (status)
        *status = RMS$_NORMAL;
    return tf;
}

rms_textfile_t *rms_textfile_open(const char *vms_spec)
{
    return rms_textfile_open_st(vms_spec, NULL);
}

uint32_t rms_textfile_last_status(const rms_textfile_t *tf)
{
    if (!tf || !tf->fp)
        return RMS$_IFI;
    return ferror(tf->fp) ? RMS$_RER : RMS$_EOF;
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

int rms_textfile_create(const char *vms_spec)
{
    if (!vms_spec || !*vms_spec)
        return -1;
    char linux_path[1024];
    vmsfs_to_linux_path(vms_spec, linux_path, sizeof(linux_path));
    FILE *f = fopen(linux_path, "w");
    if (!f)
        return -1;
    fclose(f);
    return 0;
}

#endif /* OVMX_HAVE_ACP */
