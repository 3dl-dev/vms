/*
 * dcl_help_acp.c - the RMS read side of the HELP engine (vms-4ac, vms-3381).
 *
 * dcl_help.c is the hierarchical HELP engine (parse, navigate, display); every
 * byte of a HELP library it reads comes from HERE, through RMS:
 *
 *   help_acp_library_text(spec)   a .HLP source, record by record ($OPEN,
 *                                 $CONNECT, $GET via rms_textfile_*);
 *   help_acp_library_bytes(spec)  a file's bytes by block I/O ($OPEN with
 *                                 FAC=BIO, $READ), for the compiled .HLB.
 *
 * The engine performs no file I/O of its own (libc-drop vms-3381): a library
 * RMS cannot reach is a miss, and HELP reports %HELP-E-OPENIN. On the product
 * runtime RMS rides the Files-11 ACP over /dev/vms; where the executive is
 * absent (host build/test tooling) RMS itself defers to its POSIX layer -- that
 * is RMS's business, not HELP's. dcl_help.c reaches these through weak
 * references; this TU supplies them to HELP.EXE and the engine test
 * (tests/dcl test_help_engine), and DCL.EXE carries its own pair over DCL's
 * RMS helpers (dcl_cmd_misc.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssdef.h"
#include "rms_textfile.h"       /* RMS text reader over the ACP (LIBVMS) */
#include <stdint.h>
#include "rms/rms.h"            /* sys$open/$connect/$read/$close */

/* Grow-and-append `add_len` bytes onto *buf (*cap tracked); NUL-terminate.
 * Returns 0 on success, -1 on OOM (leaving *buf for the caller to free). */
static int hacp_append(char **buf, size_t *len, size_t *cap,
                       const char *add, size_t add_len)
{
    if (*len + add_len + 1 > *cap) {
        size_t ncap = *cap ? *cap : 256;
        while (*len + add_len + 1 > ncap) ncap *= 2;
        char *nb = realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, add, add_len);
    *len += add_len;
    (*buf)[*len] = '\0';
    return 0;
}

/*
 * Slurp a VMS text file's records over the Files-11 ODS-2 ACP into a fresh
 * NUL-terminated buffer, one '\n'-terminated record at a time. NULL if the file
 * cannot be reached over the ACP (no mounted volume / no /dev/vms / no such
 * file) -- dcl_help.c then tries the POSIX fallback. A .HLP source is
 * line-oriented, so a record-by-record read reconstructs it exactly; a binary
 * .HLB is NOT read this way (its search-list entry fails the ACP open honestly
 * and the raw .HLP entry, searched next, resolves -- and on host tooling the
 * POSIX fallback still auto-detects .HLB).
 */
char *help_acp_library_text(const char *vms_spec)
{
    rms_textfile_t *tf = rms_textfile_open(vms_spec);
    if (!tf)
        return NULL;

    char *text = NULL;
    size_t len = 0, cap = 0;
    char line[1024];
    int too_long = 0;

    /* A non-NULL "" for a readable-but-empty library. */
    if (hacp_append(&text, &len, &cap, "", 0) != 0) {
        rms_textfile_close(tf);
        return NULL;
    }
    while (rms_textfile_getline(tf, line, sizeof(line), &too_long)) {
        if (hacp_append(&text, &len, &cap, line, strlen(line)) != 0 ||
            hacp_append(&text, &len, &cap, "\n", 1) != 0) {
            free(text);
            rms_textfile_close(tf);
            return NULL;
        }
    }
    rms_textfile_close(tf);
    return text;
}

/*
 * Read a library file's bytes the VMS way: RMS $OPEN with block-I/O access
 * (FAC = GET|BIO), $CONNECT, then $READ of whole virtual blocks until RMS$_EOF
 * (VSI OpenVMS RMS Reference, "Block I/O"). A compiled .HLB is a binary
 * container, so it is read by block, not by record. NULL when RMS cannot reach
 * the file; *rms_status (if non-NULL) receives the RMS status either way, and
 * *len_out the byte count read.
 */
char *help_acp_library_bytes(const char *spec, size_t *len_out, uint32_t *rms_status)
{
    struct FAB fab = cc$rms_fab;
    struct RAB rab = cc$rms_rab;
    static char blk[63 * 512];            /* the largest whole-block USZ */
    char *buf = NULL;
    size_t len = 0, cap = 0;
    uint32_t st;

    if (len_out) *len_out = 0;
    if (!spec || !spec[0] || strlen(spec) > 255) {
        if (rms_status) *rms_status = RMS$_FNM;
        return NULL;
    }
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_fac = FAB$M_GET | FAB$M_BIO;
    fab.fab$b_shr = FAB$M_SHRGET;
    st = sys$open(&fab, 0, 0);
    if (!(st & 1)) {
        if (rms_status) *rms_status = st;
        return NULL;
    }
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab, 0, 0);
    if (!(st & 1)) {
        if (rms_status) *rms_status = st;
        sys$close(&fab, 0, 0);
        return NULL;
    }
    if (hacp_append(&buf, &len, &cap, "", 0) != 0) {
        if (rms_status) *rms_status = RMS$_DME;
        sys$close(&fab, 0, 0);
        return NULL;
    }
    for (;;) {
        rab.rab$l_ubf = blk;
        rab.rab$w_usz = (uint16_t)sizeof(blk);
        rab.rab$l_bkt = 0;                /* the next block in sequence */
        st = sys$read(&rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (!(st & 1) ||
            hacp_append(&buf, &len, &cap, blk, rab.rab$w_rsz) != 0) {
            if (rms_status) *rms_status = (st & 1) ? RMS$_DME : st;
            free(buf);
            sys$close(&fab, 0, 0);
            return NULL;
        }
        if (rab.rab$w_rsz == 0)
            break;
    }
    sys$close(&fab, 0, 0);
    if (rms_status) *rms_status = RMS$_NORMAL;
    if (len_out) *len_out = len;
    return buf;
}
