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

#include "rms/rms.h"
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
