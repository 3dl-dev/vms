/*
 * dcl_messages.c - DCL's message helpers: the STS$K severity letter, the real
 * VMS condition behind a substrate errno, and VMS-form paths for messages.
 *
 * The compiled-in MSG_* "message database" that lived here is gone: its codes
 * were invented (see dcl/vms_messages.h). Condition texts come from the one
 * catalog, src/libvms/status.c.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <stdint.h>

#include "dcl/vms_messages.h"
#include "ssdef.h"
#include "rmsdef.h"

extern const char *vms$status_message(uint32_t code);

/* External: VMS filespec formatting */
extern int dcl_format_filespec(const char *linux_path, char *vms_spec,
                               size_t spec_size);

/* ---- Severity character table ------------------------------------------ */

static const char sev_chars[] = "WSEIF";  /* 0=W, 1=S, 2=E, 3=I, 4=F */

char vms_severity_char(int severity)
{
    if (severity >= 0 && severity <= 4)
        return sev_chars[severity];
    return 'E';
}

/* ---- errno to the real VMS condition ------------------------------------ */

/*
 * Only pairs whose VMS condition genuinely means what the errno means, and only
 * conditions the one catalog (src/libvms/status.c) carries. Everything else
 * takes the fallback below rather than a plausible-looking VMS message.
 */
static const struct { int unix_errno; uint32_t cond; } errno_table[] = {
    { EACCES,  RMS$_PRV },
    { EPERM,   SS$_NOPRIV },
    { ENOENT,  RMS$_FNF },
    { EEXIST,  RMS$_FEX },
    { ENOTDIR, RMS$_DNF },
    { ENOMEM,  SS$_INSFMEM },
    { EINVAL,  SS$_BADPARAM },
    { EBADF,   SS$_IVCHAN },
    { EMFILE,  SS$_EXQUOTA },
};

const char *vms_strerror(int unix_errno)
{
    static _Thread_local char fallback[64];

    for (size_t i = 0; i < sizeof errno_table / sizeof errno_table[0]; i++) {
        if (errno_table[i].unix_errno == unix_errno)
            return vms$status_message(errno_table[i].cond);
    }
    snprintf(fallback, sizeof fallback, "unexpected error (errno %d)", unix_errno);
    return fallback;
}

/* ---- Linux path to VMS path for error messages ------------------------- */

const char *vms_error_path(const char *linux_path)
{
    static _Thread_local char path_buf[1024];

    if (!linux_path || !linux_path[0])
        return "";

    /* Try the proper VMS filespec formatter */
    if (dcl_format_filespec(linux_path, path_buf, sizeof(path_buf)) == 0) {
        return path_buf;
    }

    /* Fallback: just uppercase the basename */
    const char *basename = strrchr(linux_path, '/');
    if (basename)
        basename++;
    else
        basename = linux_path;

    size_t i;
    for (i = 0; basename[i] && i < sizeof(path_buf) - 1; i++) {
        path_buf[i] = (char)toupper((unsigned char)basename[i]);
    }
    path_buf[i] = '\0';
    return path_buf;
}
