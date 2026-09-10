/*
 * crtl_rms_stdio.c — OVMX addition (vms-47e): the C RTL stdio FILE veneer over
 * RMS, declared in include/rms/crtl_stdio.h. fopen/fwrite/fread/fclose drive
 * sys$create/$open/$connect/$put/$get/$close against the real Files-11 ODS-2
 * volume through the executive ACP — the do-it-like-VMS binding (Rule 1: the
 * CRTL is a thin veneer over RMS, as on real OpenVMS), NOT musl POSIX to a raw
 * kernel filesystem.
 *
 * Byte-exactness (FIX mrs=0 per-put write, FIX mrs=1 byte-loop read) and the
 * fail-honest / no-POSIX-fallback contract are documented in crtl_stdio.h; this
 * file is the mechanical mirror of ovmx_link_rms_io.c (LINK.EXE's proven RMS
 * shim), reshaped from a whole-file slurp/write into the incremental FILE*
 * stdio contract the port's fopen/fwrite/fread callers expect.
 *
 * All sys$ calls trace to stderr ("OVMX-CRTL-RMS: ...") so a harness can grep
 * proof that the RMS path — not raw POSIX — is what ran, and cross-check byte
 * totals (tests/qemu/test_syssvc_crtl_rms_veneer.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>          /* vms-3320: O_CREAT/O_WRONLY/O_RDWR/O_TRUNC (target ABI) */

#include "rms/rms.h"          /* sys$create/$open/$erase/$parse/$search/$rename */
#include "rms/crtl_stdio.h"

/* The RMS-backed stream. The FAB/RAB live for the life of the handle (the
 * record context stays connected between fwrite/fread calls and is torn down at
 * fclose), exactly as a DEC C FILE keeps its RMS internals. */
struct ovmx_crtl_file {
    struct FAB fab;
    struct RAB rab;
    char       fnbuf[512];   /* stable backing for fab$l_fna */
    int        writing;      /* 1 = opened for $PUT, 0 = opened for $GET */
    int        connected;    /* RAB connected? */
};

/* ------------------------------------------------------------------ open ---- */

OVMX_CRTL_FILE *ovmx_crtl_fopen(const char *path, const char *mode)
{
    if (!path || !mode)
        return NULL;

    int writing;
    switch (mode[0]) {
        case 'r': writing = 0; break;
        case 'w': writing = 1; break;
        default:
            fprintf(stderr, "OVMX-CRTL-RMS: fopen(\"%s\",\"%s\"): unsupported mode "
                            "(fail-honest, no POSIX fallback)\n", path, mode);
            return NULL;                    /* 'a'/'r+'/... not yet veneered */
    }

    OVMX_CRTL_FILE *fh = calloc(1, sizeof *fh);
    if (!fh) {
        fprintf(stderr, "OVMX-CRTL-RMS: fopen(\"%s\"): oom\n", path);
        return NULL;
    }

    strncpy(fh->fnbuf, path, sizeof fh->fnbuf - 1);
    fh->fnbuf[sizeof fh->fnbuf - 1] = '\0';

    fh->fab = cc$rms_fab;
    fh->fab.fab$l_fna = fh->fnbuf;
    fh->fab.fab$b_fns = (uint8_t)strlen(fh->fnbuf);
    fh->fab.fab$b_org = FAB$C_SEQ;
    fh->fab.fab$b_rfm = FAB$C_FIX;          /* byte-exact (see crtl_stdio.h) */
    fh->fab.fab$w_mrs = writing ? 0 : 1;    /* w: per-put rsz; r: 1 byte/get   */
    fh->fab.fab$b_fac = writing ? FAB$M_PUT : FAB$M_GET;

    uint32_t st = writing ? sys$create(&fh->fab, 0, 0)
                          : sys$open(&fh->fab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: sys$%s(\"%s\") -> %u\n",
            writing ? "create" : "open", path, st);
    if (st != RMS$_NORMAL) {
        free(fh);
        return NULL;                        /* fail-honest */
    }

    /* sys$open restores the STORED record format from the file's metadata; LINK
     * and this veneer want RAW BYTES regardless of how the producer framed the
     * file, so re-assert FIX/mrs=1 after the open (mirrors ovmx_link_rms_io.c). */
    if (!writing) {
        fh->fab.fab$b_rfm = FAB$C_FIX;
        fh->fab.fab$w_mrs = 1;
    }

    fh->rab = cc$rms_rab;
    fh->rab.rab$l_fab = &fh->fab;
    st = sys$connect(&fh->rab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: sys$connect(\"%s\") -> %u\n", path, st);
    if (st != RMS$_NORMAL) {
        sys$close(&fh->fab, 0, 0);
        free(fh);
        return NULL;
    }
    fh->connected = 1;
    fh->writing = writing;
    return fh;
}

/* ------------------------------------------------------------------ write --- */

size_t ovmx_crtl_fwrite(const void *ptr, size_t size, size_t nmemb,
                        OVMX_CRTL_FILE *fh)
{
    if (!fh || !fh->writing || !ptr || size == 0 || nmemb == 0)
        return 0;

    size_t nbytes = size * nmemb;
    if (nbytes > 0xFFFF) {
        /* One $PUT is one FIX record; rab$w_rsz is 16-bit. A larger request
         * would need chunking into successive $PUTs — deferred to the child
         * (the core port-test writes <= 8 KiB). Fail-honest short count. */
        fprintf(stderr, "OVMX-CRTL-RMS: fwrite %zu bytes exceeds one-record cap "
                        "(chunking deferred, vms-47e child)\n", nbytes);
        return 0;
    }

    fh->rab.rab$l_rbf = (char *)ptr;
    fh->rab.rab$w_rsz = (uint16_t)nbytes;
    uint32_t st = sys$put(&fh->rab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: sys$put(%zu bytes) -> %u\n", nbytes, st);
    if (st != RMS$_NORMAL)
        return 0;                           /* fail-honest short count */
    return nmemb;
}

/* ------------------------------------------------------------------ read ---- */

size_t ovmx_crtl_fread(void *ptr, size_t size, size_t nmemb, OVMX_CRTL_FILE *fh)
{
    if (!fh || fh->writing || !ptr || size == 0 || nmemb == 0)
        return 0;

    size_t want = size * nmemb;
    size_t got = 0;
    uint8_t *out = (uint8_t *)ptr;

    while (got < want) {
        uint8_t ch;
        fh->rab.rab$l_ubf = (char *)&ch;
        fh->rab.rab$w_usz = 1;
        uint32_t st = sys$get(&fh->rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (st != RMS$_NORMAL) {
            fprintf(stderr, "OVMX-CRTL-RMS: sys$get -> %u (error)\n", st);
            break;
        }
        out[got++] = ch;
    }

    fprintf(stderr, "OVMX-CRTL-RMS: fread got %zu of %zu bytes\n", got, want);
    return got / size;                      /* whole members only (C contract) */
}

/* ------------------------------------------------------------------ close --- */

int ovmx_crtl_fclose(OVMX_CRTL_FILE *fh)
{
    if (!fh)
        return -1;
    uint32_t st = sys$close(&fh->fab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: sys$close(\"%s\") -> %u\n", fh->fnbuf, st);
    free(fh);
    return (st == RMS$_NORMAL) ? 0 : -1;
}

/* ======================================================================== *
 * vms-3320: file-ops beyond the stdio family (open/creat/unlink/remove/     *
 * rename/opendir/readdir/closedir), each over the SAME proven RMS engine.   *
 * ======================================================================== */

/* Shared open helper (the fopen body, parameterized). Mints a connected RMS
 * stream: writing/create => sys$create (FAB$C_FIX mrs=0 byte-exact put),
 * else sys$open (re-asserted FIX mrs=1 for byte-exact get). Returns a handle or
 * NULL (fail-honest, no POSIX fallback). Kept separate from ovmx_crtl_fopen so
 * that proven stdio entry point stays byte-identical (its gates depend on it). */
static OVMX_CRTL_FILE *crtl_rms_open_handle(const char *path, int writing,
                                            int create)
{
    OVMX_CRTL_FILE *fh = calloc(1, sizeof *fh);
    if (!fh) {
        fprintf(stderr, "OVMX-CRTL-RMS: open(\"%s\"): oom\n", path);
        return NULL;
    }
    strncpy(fh->fnbuf, path, sizeof fh->fnbuf - 1);
    fh->fnbuf[sizeof fh->fnbuf - 1] = '\0';

    fh->fab = cc$rms_fab;
    fh->fab.fab$l_fna = fh->fnbuf;
    fh->fab.fab$b_fns = (uint8_t)strlen(fh->fnbuf);
    fh->fab.fab$b_org = FAB$C_SEQ;
    fh->fab.fab$b_rfm = FAB$C_FIX;
    fh->fab.fab$w_mrs = writing ? 0 : 1;
    fh->fab.fab$b_fac = writing ? FAB$M_PUT : FAB$M_GET;

    uint32_t st = create ? sys$create(&fh->fab, 0, 0)
                         : sys$open(&fh->fab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: sys$%s(\"%s\") -> %u\n",
            create ? "create" : "open", path, st);
    if (st != RMS$_NORMAL) {
        free(fh);
        return NULL;                            /* fail-honest */
    }
    if (!writing) {
        fh->fab.fab$b_rfm = FAB$C_FIX;
        fh->fab.fab$w_mrs = 1;
    }

    fh->rab = cc$rms_rab;
    fh->rab.rab$l_fab = &fh->fab;
    st = sys$connect(&fh->rab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: sys$connect(\"%s\") -> %u\n", path, st);
    if (st != RMS$_NORMAL) {
        sys$close(&fh->fab, 0, 0);
        free(fh);
        return NULL;
    }
    fh->connected = 1;
    fh->writing = writing;
    return fh;
}

/* --------------------------------------------------------------- open/creat -
 * The fd->RMS-handle table. Values start at OVMX_CRTL_FD_BASE so a veneer fd
 * can never collide with a musl POSIX fd; a musl op on a foreign value fails
 * EBADF (honest), never a silent wrong success. */
static OVMX_CRTL_FILE *ovmx_crtl_fd_tbl[OVMX_CRTL_FD_MAX];

static int crtl_fd_alloc(OVMX_CRTL_FILE *fh)
{
    for (int i = 0; i < OVMX_CRTL_FD_MAX; i++) {
        if (!ovmx_crtl_fd_tbl[i]) {
            ovmx_crtl_fd_tbl[i] = fh;
            return OVMX_CRTL_FD_BASE + i;
        }
    }
    return -1;
}

int ovmx_crtl_open(const char *path, int oflag)   /* NON-variadic: see crtl_stdio.h */
{
    if (!path)
        return -1;

    int create  = (oflag & O_CREAT) != 0;
    int writing = create || ((oflag & O_ACCMODE) != O_RDONLY);

    OVMX_CRTL_FILE *fh = crtl_rms_open_handle(path, writing, create);
    if (!fh) {
        fprintf(stderr, "OVMX-CRTL-RMS: open(\"%s\",0x%x) -> -1 "
                        "(fail-honest, no POSIX fallback)\n", path, oflag);
        return -1;                              /* fail-honest */
    }
    int fd = crtl_fd_alloc(fh);
    if (fd < 0) {
        fprintf(stderr, "OVMX-CRTL-RMS: open(\"%s\"): fd table full\n", path);
        ovmx_crtl_fclose(fh);
        return -1;
    }
    fprintf(stderr, "OVMX-CRTL-RMS: open(\"%s\",0x%x) -> fd %d (%s over the ACP)\n",
            path, oflag, fd, create ? "sys$create" : "sys$open");
    return fd;
}

int ovmx_crtl_creat(const char *path, int mode)
{
    (void)mode;                                 /* ODS-2 protection is the header's */
    /* creat == open(path, O_CREAT|O_WRONLY|O_TRUNC): always mint a new file. */
    return ovmx_crtl_open(path, O_CREAT | O_WRONLY | O_TRUNC);
}

int ovmx_crtl_fdclose(int fd)
{
    int i = fd - OVMX_CRTL_FD_BASE;
    if (i < 0 || i >= OVMX_CRTL_FD_MAX || !ovmx_crtl_fd_tbl[i]) {
        fprintf(stderr, "OVMX-CRTL-RMS: fdclose(%d): not a veneer fd "
                        "(fail-honest)\n", fd);
        return -1;
    }
    OVMX_CRTL_FILE *fh = ovmx_crtl_fd_tbl[i];
    ovmx_crtl_fd_tbl[i] = NULL;
    return ovmx_crtl_fclose(fh);
}

/* ------------------------------------------------------------ unlink/remove -
 * sys$erase: IO$_DELETE removes the directory entry + deallocates the header
 * and blocks on the real ODS-2 volume. remove() is the ISO C spelling. */
int ovmx_crtl_unlink(const char *path)
{
    if (!path)
        return -1;
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)path;
    fab.fab$b_fns = (uint8_t)strlen(path);
    uint32_t st = sys$erase(&fab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: unlink/remove(\"%s\") -> sys$erase %u\n",
            path, st);
    return (st == RMS$_NORMAL) ? 0 : -1;        /* fail-honest */
}

int ovmx_crtl_remove(const char *path)
{
    return ovmx_crtl_unlink(path);
}

/* -------------------------------------------------------------------- rename -
 * sys$rename -> executive ACP MODIFY!M_MOVE: atomic directory-entry re-link,
 * the file KEEPS its File ID + allocation (NOT erase+create). */
int ovmx_crtl_rename(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath)
        return -1;
    struct FAB ofab = cc$rms_fab, nfab = cc$rms_fab;
    ofab.fab$l_fna = (char *)oldpath;
    ofab.fab$b_fns = (uint8_t)strlen(oldpath);
    nfab.fab$l_fna = (char *)newpath;
    nfab.fab$b_fns = (uint8_t)strlen(newpath);
    uint32_t st = sys$rename(&ofab, 0, 0, &nfab);
    fprintf(stderr, "OVMX-CRTL-RMS: rename(\"%s\",\"%s\") -> sys$rename %u "
                    "(ACP MODIFY!M_MOVE, same File ID)\n", oldpath, newpath, st);
    return (st == RMS$_NORMAL) ? 0 : -1;        /* fail-honest */
}

/* --------------------------------------------------- opendir/readdir/closedir
 * A DIR*-equivalent over the sys$parse + sys$search wildcard context. */
struct ovmx_crtl_dir {
    struct FAB fab;
    struct NAM nam;
    char       pattern[512];      /* stable backing for fab$l_fna              */
    char       esa[512];          /* expanded-string area (sys$parse)          */
    char       rsa[512];          /* resultant-string area (each sys$search)   */
    struct ovmx_crtl_dirent de;   /* returned entry (overwritten per readdir)  */
    int        parsed;            /* sys$parse succeeded => search context live */
};

OVMX_CRTL_DIR *ovmx_crtl_opendir(const char *name)
{
    if (!name)
        return NULL;

    OVMX_CRTL_DIR *d = calloc(1, sizeof *d);
    if (!d) {
        fprintf(stderr, "OVMX-CRTL-RMS: opendir(\"%s\"): oom\n", name);
        return NULL;
    }

    /* Compose "<dirspec>*.*;*" -- every file, type and version in the dir. If
     * the caller already handed a wildcard (contains '*'), take it verbatim. */
    if (strchr(name, '*')) {
        strncpy(d->pattern, name, sizeof d->pattern - 1);
    } else {
        strncpy(d->pattern, name, sizeof d->pattern - 1);
        size_t l = strlen(d->pattern);
        strncpy(d->pattern + l, "*.*;*", sizeof d->pattern - 1 - l);
    }
    d->pattern[sizeof d->pattern - 1] = '\0';

    d->fab = cc$rms_fab;
    d->fab.fab$l_fna = d->pattern;
    d->fab.fab$b_fns = (uint8_t)strlen(d->pattern);
    d->nam = cc$rms_nam;
    d->nam.nam$l_esa = d->esa;
    d->nam.nam$b_ess = (uint8_t)(sizeof d->esa > 255 ? 255 : sizeof d->esa);
    d->nam.nam$l_rsa = d->rsa;
    d->nam.nam$b_rss = (uint8_t)(sizeof d->rsa > 255 ? 255 : sizeof d->rsa);
    d->fab.fab$l_nam = &d->nam;

    uint32_t st = sys$parse(&d->fab, 0, 0);
    fprintf(stderr, "OVMX-CRTL-RMS: opendir(\"%s\") -> sys$parse(\"%s\") %u\n",
            name, d->pattern, st);
    if (st != RMS$_NORMAL) {
        rms_search_end(&d->nam);
        free(d);
        return NULL;                            /* fail-honest */
    }
    d->parsed = 1;
    return d;
}

struct ovmx_crtl_dirent *ovmx_crtl_readdir(OVMX_CRTL_DIR *dirp)
{
    if (!dirp || !dirp->parsed)
        return NULL;

    uint32_t st = sys$search(&dirp->fab, 0, 0);
    if (st != RMS$_NORMAL) {
        /* RMS$_NMF at end of the real directory, or a fail-honest error. */
        fprintf(stderr, "OVMX-CRTL-RMS: readdir -> sys$search %u (end/none)\n", st);
        return NULL;
    }

    /* Resultant spec is "DEV:[DIR]NAME.TYP;VER"; the dirent name is the tail
     * after the closing ']' (or ':' if no dir) -- the filename component. */
    size_t rl = dirp->nam.nam$b_rsl;
    if (rl >= sizeof dirp->rsa) rl = sizeof dirp->rsa - 1;
    dirp->rsa[rl] = '\0';
    const char *nm = dirp->rsa;
    const char *rb = strrchr(dirp->rsa, ']');
    if (!rb) rb = strrchr(dirp->rsa, ':');
    if (rb) nm = rb + 1;

    strncpy(dirp->de.d_name, nm, sizeof dirp->de.d_name - 1);
    dirp->de.d_name[sizeof dirp->de.d_name - 1] = '\0';
    dirp->de.d_namlen = (unsigned short)strlen(dirp->de.d_name);

    uint16_t num = 0, seq = 0; uint8_t rvn = 0, nmx = 0;
    rms_search_fid(&dirp->nam, &num, &seq, &rvn, &nmx);
    dirp->de.d_fileid = num;

    fprintf(stderr, "OVMX-CRTL-RMS: readdir -> \"%s\" fid=(%u,%u,%u)\n",
            dirp->de.d_name, num, seq, rvn);
    return &dirp->de;
}

int ovmx_crtl_closedir(OVMX_CRTL_DIR *dirp)
{
    if (!dirp)
        return -1;
    rms_search_end(&dirp->nam);                 /* release the executive context */
    fprintf(stderr, "OVMX-CRTL-RMS: closedir\n");
    free(dirp);
    return 0;
}
