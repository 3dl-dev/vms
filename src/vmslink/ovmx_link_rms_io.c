/*
 * ovmx_link_rms_io.c — OVMX addition (vms-b5a), NOT part of the base LINK.EXE
 * MVP. Implements the RMS-backed whole-file read/write shim declared in
 * ovmx_link_rms_io.h. All sys$ calls trace to stderr ("OVMX-RMS: ...") so the
 * harness (run_link_native.sh) can grep proof that the RMS path — not raw POSIX
 * open/read/write — is what actually ran, and cross-check the byte totals.
 *
 * See ovmx_link_rms_io.h for the scope and the byte-exactness rationale
 * (mrs=1 read, mrs=0 per-put write).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rms/rms.h>

#include "ovmx_link_rms_io.h"

/* --------------------------------------------------------------------- */
/* Read side: byte-exact whole-file slurp via sys$open + sys$connect + a  */
/* sys$get loop (FAB$C_FIX, mrs=1 -> one exact byte per record, no        */
/* space-padding at EOF) + sys$close.                                     */
/* --------------------------------------------------------------------- */

/* Open `path` for a byte-exact sequential read. On success fills *fab/*rab and
 * returns 1; on failure traces and returns 0. */
static int rms_open_read(const char *path, struct FAB *fab, struct RAB *rab,
                         char *fnbuf, size_t fnbuf_sz)
{
    memset(fab, 0, sizeof *fab);
    memset(rab, 0, sizeof *rab);
    strncpy(fnbuf, path, fnbuf_sz - 1);
    fnbuf[fnbuf_sz - 1] = '\0';

    *fab = cc$rms_fab;
    fab->fab$l_fna = fnbuf;
    fab->fab$b_fns = (uint8_t)strlen(fnbuf);
    fab->fab$b_fac = FAB$M_GET;
    fab->fab$b_org = FAB$C_SEQ;
    fab->fab$b_rfm = FAB$C_FIX;
    fab->fab$w_mrs = 1;            /* one exact byte per get: byte-exact, no
                                    * EOF space-pad (see header) */

    uint32_t st = sys$open(fab);
    fprintf(stderr, "OVMX-RMS: sys$open(\"%s\") -> %08X\n", path, st);
    if (!(st & 1))
        return 0;

    *rab = cc$rms_rab;
    rab->rab$l_fab = fab;
    st = sys$connect(rab);
    fprintf(stderr, "OVMX-RMS: sys$connect(read \"%s\") -> %08X\n", path, st);
    if (!(st & 1)) {
        sys$close(fab);
        return 0;
    }
    return 1;
}

uint8_t *ovmx_link_rms_slurp(const char *path, size_t *out_size)
{
    struct FAB fab;
    struct RAB rab;
    char fnbuf[1024];

    if (!rms_open_read(path, &fab, &rab, fnbuf, sizeof fnbuf))
        return NULL;

    size_t cap = 4096, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fprintf(stderr, "OVMX-RMS: slurp(\"%s\"): oom\n", path);
        sys$close(&fab);
        return NULL;
    }

    long ngets = 0;
    for (;;) {
        uint8_t ch;
        rab.rab$l_ubf = (char *)&ch;
        rab.rab$w_usz = 1;
        uint32_t st = sys$get(&rab);
        if (st == RMS$_EOF)
            break;
        if (!(st & 1)) {
            fprintf(stderr, "OVMX-RMS: sys$get(\"%s\") -> %08X (error)\n", path, st);
            free(buf);
            sys$close(&fab);
            return NULL;
        }
        if (len == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) {
                fprintf(stderr, "OVMX-RMS: slurp(\"%s\"): oom growing to %zu\n", path, cap);
                free(buf);
                sys$close(&fab);
                return NULL;
            }
            buf = nb;
        }
        buf[len++] = ch;
        ngets++;
    }

    fprintf(stderr, "OVMX-RMS: sys$get loop(\"%s\") done, %ld gets, %zu bytes, reached EOF\n",
            path, ngets, len);

    uint32_t st = sys$close(&fab);
    fprintf(stderr, "OVMX-RMS: sys$close(read \"%s\") -> %08X\n", path, st);

    *out_size = len;
    return buf;
}

int ovmx_link_rms_peek(const char *path, void *buf, int n)
{
    struct FAB fab;
    struct RAB rab;
    char fnbuf[1024];

    if (n <= 0)
        return 0;
    if (!rms_open_read(path, &fab, &rab, fnbuf, sizeof fnbuf))
        return -1;

    int got = 0;
    uint8_t *out = (uint8_t *)buf;
    while (got < n) {
        uint8_t ch;
        rab.rab$l_ubf = (char *)&ch;
        rab.rab$w_usz = 1;
        uint32_t st = sys$get(&rab);
        if (st == RMS$_EOF || !(st & 1))
            break;
        out[got++] = ch;
    }
    sys$close(&fab);
    fprintf(stderr, "OVMX-RMS: peek(\"%s\") -> %d byte(s)\n", path, got);
    return got;
}

/* --------------------------------------------------------------------- */
/* Write side: byte-exact whole-buffer write via sys$create + sys$connect */
/* + a chunked sys$put loop (FAB$C_FIX, mrs=0 -> each put's rab$w_rsz is  */
/* the record length, raw byte-exact passthrough) + sys$close.            */
/* --------------------------------------------------------------------- */

#define OVMX_LINK_PUT_CHUNK 32768

int ovmx_link_rms_write(const char *path, const uint8_t *buf, size_t size)
{
    struct FAB fab = cc$rms_fab;
    char fnbuf[1024];
    strncpy(fnbuf, path, sizeof(fnbuf) - 1);
    fnbuf[sizeof(fnbuf) - 1] = '\0';
    fab.fab$l_fna = fnbuf;
    fab.fab$b_fns = (uint8_t)strlen(fnbuf);
    fab.fab$b_fac = FAB$M_PUT;
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_FIX;     /* fab$w_mrs stays 0 -> rms_seq_put writes
                                    * each put's own length, no pad (byte-exact) */

    uint32_t st = sys$create(&fab);
    fprintf(stderr, "OVMX-RMS: sys$create(\"%s\") -> %08X\n", path, st);
    if (!(st & 1))
        return -1;

    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab);
    fprintf(stderr, "OVMX-RMS: sys$connect(write \"%s\") -> %08X\n", path, st);
    if (!(st & 1)) {
        sys$close(&fab);
        return -1;
    }

    size_t total = 0;
    int nputs = 0;
    while (total < size) {
        size_t chunk = size - total;
        if (chunk > OVMX_LINK_PUT_CHUNK)
            chunk = OVMX_LINK_PUT_CHUNK;
        rab.rab$l_rbf = (char *)(buf + total);
        rab.rab$w_rsz = (uint16_t)chunk;
        st = sys$put(&rab);
        if (!(st & 1)) {
            fprintf(stderr, "OVMX-RMS: sys$put(\"%s\") -> %08X (error)\n", path, st);
            sys$close(&fab);
            return -1;
        }
        total += chunk;
        nputs++;
    }
    fprintf(stderr, "OVMX-RMS: sys$put loop(\"%s\") done, %d puts, %zu bytes total\n",
            path, nputs, total);

    st = sys$close(&fab);
    fprintf(stderr, "OVMX-RMS: sys$close(write \"%s\") -> %08X\n", path, st);
    return (st & 1) ? 0 : -1;
}
