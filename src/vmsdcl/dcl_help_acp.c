/*
 * dcl_help_acp.c - the Files-11 ACP read seam for the HELP engine (vms-4ac).
 *
 * dcl_help.c is the pure hierarchical HELP engine; it must stay free of the RMS
 * and vmsfs dependencies so the hermetic engine unit test (tests/dcl/
 * test_help_engine.c) can compile and link it alone. The library READ -- which
 * on the product runtime must go over the executive Files-11 ODS-2 ACP, because
 * the /vms POSIX passthrough it used to fopen() was retired by the atomic flip
 * (epic vms-208) -- lives HERE instead, and dcl_help.c reaches it through two
 * WEAK seams (help_acp_library_text / help_acp_vms_to_linux). This TU is
 * compiled into the images that DO link RMS/vmsfs (DCL.EXE, HELP.EXE); the
 * engine test links it not at all, so those weak references resolve to NULL and
 * dcl_help.c's VMS-spec branch is inert there.
 *
 * This is the same #pragma-weak layering discipline rms_textfile.c itself uses
 * (LIBVMS sits below RMS, so a hard reference would invert the layering).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ssdef.h"
#include "rms_textfile.h"       /* RMS text reader over the ACP (LIBVMS) */
#include "vmsfs/filespec.h"     /* vmsfs_to_linux_path (POSIX fallback) */

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
 * Translate a VMS filespec to its Linux /vms path for the POSIX fallback.
 * Returns 1 on success (buf filled), 0 otherwise. vmsfs_to_linux_path returns a
 * VMS status code (odd == success), NOT 0-on-success.
 */
int help_acp_vms_to_linux(const char *vms_spec, char *buf, size_t bufsz)
{
    return $VMS_STATUS_SUCCESS(vmsfs_to_linux_path(vms_spec, buf, bufsz)) ? 1 : 0;
}
