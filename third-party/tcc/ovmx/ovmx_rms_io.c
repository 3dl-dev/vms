/*
 * ovmx_rms_io.c — OVMX addition (vms-4ba.5), NOT part of stock tinycc.
 *
 * Implements the RMS-backed read/write shim declared in ovmx_rms_io.h. See
 * that header for the scope/rationale. All sys$ calls trace to stderr
 * ("OVMX-RMS: ...") so the harness (run_tcc_rms.sh) can grep proof that the
 * RMS path — not raw POSIX open/read/write — is what actually ran.
 */

#include <stdio.h>
#include <string.h>

#include <rms/rms.h>

#include "ovmx_rms_io.h"

/* --------------------------------------------------------------------- */
/* Read side: sys$open + sys$connect at open, a chunked sys$get loop at   */
/* read, sys$close at close.                                              */
/* --------------------------------------------------------------------- */

#define OVMX_RMS_MAX_OPEN 16
#define OVMX_RMS_CHUNK    512  /* FAB$C_FIX record size for chunked reads */

typedef struct {
    int         in_use;
    struct FAB  fab;
    struct RAB  rab;
    char        filename[1024];
    char        chunkbuf[OVMX_RMS_CHUNK];
    int         chunk_len;   /* valid bytes currently in chunkbuf */
    int         chunk_pos;   /* bytes of chunkbuf already handed to caller */
    int         eof;
} ovmx_rms_read_slot;

static ovmx_rms_read_slot g_read_slots[OVMX_RMS_MAX_OPEN];

int ovmx_rms_open_read(const char *filename)
{
    int i;
    for (i = 0; i < OVMX_RMS_MAX_OPEN; i++) {
        if (!g_read_slots[i].in_use)
            break;
    }
    if (i == OVMX_RMS_MAX_OPEN) {
        fprintf(stderr, "OVMX-RMS: ovmx_rms_open_read(\"%s\"): no free slots\n", filename);
        return -1;
    }

    ovmx_rms_read_slot *sl = &g_read_slots[i];
    memset(sl, 0, sizeof(*sl));
    strncpy(sl->filename, filename, sizeof(sl->filename) - 1);

    sl->fab = cc$rms_fab;
    sl->fab.fab$l_fna = sl->filename;
    sl->fab.fab$b_fns = (uint8_t)strlen(sl->filename);
    sl->fab.fab$b_fac = FAB$M_GET;
    sl->fab.fab$b_org = FAB$C_SEQ;
    sl->fab.fab$b_rfm = FAB$C_FIX; /* raw fixed-size chunks: byte-exact, no
                                     * text delimiter munging of C source */

    uint32_t st = sys$open(&sl->fab);
    fprintf(stderr, "OVMX-RMS: sys$open(\"%s\") -> %08X\n", filename, st);
    if (!(st & 1))
        return -1;

    sl->rab = cc$rms_rab;
    sl->rab.rab$l_fab = &sl->fab;
    st = sys$connect(&sl->rab);
    fprintf(stderr, "OVMX-RMS: sys$connect(read \"%s\") -> %08X\n", filename, st);
    if (!(st & 1)) {
        sys$close(&sl->fab);
        return -1;
    }

    sl->in_use = 1;
    return OVMX_RMS_FD_BASE + i;
}

/* Pull one more chunk into sl->chunkbuf via sys$get. Returns bytes newly
 * available (0 at EOF, <0 on error). */
static int ovmx_rms_refill(ovmx_rms_read_slot *sl)
{
    if (sl->eof)
        return 0;

    sl->fab.fab$w_mrs = OVMX_RMS_CHUNK;
    sl->rab.rab$l_ubf = sl->chunkbuf;
    sl->rab.rab$w_usz = OVMX_RMS_CHUNK;

    uint32_t st = sys$get(&sl->rab);
    if (st == RMS$_EOF) {
        fprintf(stderr, "OVMX-RMS: sys$get(\"%s\") -> EOF\n", sl->filename);
        sl->eof = 1;
        return 0;
    }
    if (!(st & 1)) {
        fprintf(stderr, "OVMX-RMS: sys$get(\"%s\") -> %08X (error)\n", sl->filename, st);
        sl->eof = 1;
        return -1;
    }

    sl->chunk_len = sl->rab.rab$w_rsz;
    sl->chunk_pos = 0;
    fprintf(stderr, "OVMX-RMS: sys$get(\"%s\") -> %08X, %d bytes\n",
            sl->filename, st, sl->chunk_len);
    return sl->chunk_len;
}

int ovmx_rms_read(int fd, void *buf, int count)
{
    int idx = fd - OVMX_RMS_FD_BASE;
    if (idx < 0 || idx >= OVMX_RMS_MAX_OPEN || !g_read_slots[idx].in_use)
        return -1;
    ovmx_rms_read_slot *sl = &g_read_slots[idx];

    int copied = 0;
    char *out = (char *)buf;
    while (copied < count) {
        if (sl->chunk_pos >= sl->chunk_len) {
            int r = ovmx_rms_refill(sl);
            if (r <= 0)
                break; /* EOF or error: return whatever we already copied */
        }
        int avail = sl->chunk_len - sl->chunk_pos;
        int want = count - copied;
        int take = avail < want ? avail : want;
        memcpy(out + copied, sl->chunkbuf + sl->chunk_pos, (size_t)take);
        sl->chunk_pos += take;
        copied += take;
    }
    return copied;
}

int ovmx_rms_close_read(int fd)
{
    int idx = fd - OVMX_RMS_FD_BASE;
    if (idx < 0 || idx >= OVMX_RMS_MAX_OPEN || !g_read_slots[idx].in_use)
        return -1;
    ovmx_rms_read_slot *sl = &g_read_slots[idx];

    uint32_t st = sys$close(&sl->fab);
    fprintf(stderr, "OVMX-RMS: sys$close(read \"%s\") -> %08X\n", sl->filename, st);
    sl->in_use = 0;
    return (st & 1) ? 0 : -1;
}

/* --------------------------------------------------------------------- */
/* Write side: sys$create + sys$connect, a chunked sys$put loop over a    */
/* scratch file tcc already finished writing (raw ELF assembly untouched)*/
/* sys$close on the RMS-delivered destination.                            */
/* --------------------------------------------------------------------- */

#define OVMX_RMS_PUT_CHUNK 512

int ovmx_rms_deliver_file(const char *scratch_path, const char *dest_path)
{
    FILE *in = fopen(scratch_path, "rb");
    if (!in) {
        fprintf(stderr, "OVMX-RMS: ovmx_rms_deliver_file: could not reopen scratch '%s'\n",
                scratch_path);
        return -1;
    }

    struct FAB fab = cc$rms_fab;
    char fnbuf[1024];
    strncpy(fnbuf, dest_path, sizeof(fnbuf) - 1);
    fnbuf[sizeof(fnbuf) - 1] = '\0';
    fab.fab$l_fna = fnbuf;
    fab.fab$b_fns = (uint8_t)strlen(fnbuf);
    fab.fab$b_fac = FAB$M_PUT;
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_FIX; /* fab$w_mrs stays 0 -> rms_seq_put uses each
                                 * put's own length as the record size, i.e.
                                 * a raw byte-exact passthrough with no
                                 * padding or delimiter (see rms_seq.c) */

    uint32_t st = sys$create(&fab);
    fprintf(stderr, "OVMX-RMS: sys$create(\"%s\") -> %08X\n", dest_path, st);
    if (!(st & 1)) {
        fclose(in);
        return -1;
    }

    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab);
    fprintf(stderr, "OVMX-RMS: sys$connect(write \"%s\") -> %08X\n", dest_path, st);
    if (!(st & 1)) {
        sys$close(&fab);
        fclose(in);
        return -1;
    }

    char buf[OVMX_RMS_PUT_CHUNK];
    size_t n;
    long total = 0;
    int nputs = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        rab.rab$l_rbf = buf;
        rab.rab$w_rsz = (uint16_t)n;
        st = sys$put(&rab);
        if (!(st & 1)) {
            fprintf(stderr, "OVMX-RMS: sys$put(\"%s\") -> %08X (error)\n", dest_path, st);
            sys$close(&fab);
            fclose(in);
            return -1;
        }
        total += (long)n;
        nputs++;
    }
    fprintf(stderr, "OVMX-RMS: sys$put loop(\"%s\") done, %d puts, %ld bytes total\n",
            dest_path, nputs, total);

    fclose(in);
    st = sys$close(&fab);
    fprintf(stderr, "OVMX-RMS: sys$close(write \"%s\") -> %08X\n", dest_path, st);
    return (st & 1) ? 0 : -1;
}
