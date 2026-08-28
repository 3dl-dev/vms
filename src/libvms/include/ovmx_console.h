/*
 * ovmx_console.h - the ONE place that maps a VMS console-terminal device
 * NAME to its Linux backing path (vms-948).
 *
 * OVMX registers exactly one terminal in the executive device table --
 * OPA0:, the operator console (src/kernel-core/vms_devtab.c,
 * VMS_CONSOLE_DEVNAM). Four spellings name that same row: the generic
 * job-terminal aliases TT:/TT0: and the console's logical/physical names
 * OPA0:/_OPA0:. Its Linux backing is /dev/console, NOT /dev/tty (measured,
 * vms-1c57: /dev/tty needs a controlling terminal OVMX never establishes).
 *
 * WHY THIS HEADER EXISTS. That name->path mapping was hand-copied in
 * src/libvms/syssvc/sys_assign.c (sys$assign) and, worse, a shipped DCL
 * procedure spelled the SUBSTRATE PATH directly -- RUN /DETACHED
 * /INPUT="/dev/console" in JOB_CONTROL_STARTUP.COM -- because
 * dcl_resolve_path() routed OPA0: (any ':'-bearing spec) through the vmsfs
 * DISK translator and so never reached this terminal-alias resolution. A raw
 * Linux path inside a DCL qualifier on a VMS-surface .COM is the exact
 * substrate leak the authenticity rules forbid (INV-6 / "what would VMS do").
 * This header is the single source both sys$assign and DCL's RUN /INPUT=
 * resolution call, so OPA0: resolves the ONE way everywhere and the shipped
 * .COM names the VMS device (OPA0:), never the substrate path.
 *
 * static inline (no linkage): every caller compiles its own copy, so there is
 * no cross-image symbol to register in the native-link/self-host symbol
 * vector -- the mapping is shared as SOURCE, not as a linked entry point.
 *
 * Clean-room (CLAUDE.md Rule 8): device names + console semantics from public
 * OpenVMS documentation and OVMX's own device table; no VSI source.
 */
#ifndef OVMX_CONSOLE_H
#define OVMX_CONSOLE_H

#include <stddef.h>
#include <string.h>
#include <ctype.h>

/* The console's VMS device name (matches src/kernel-core/vms_devtab.c). */
#ifndef OVMX_CONSOLE_DEVICE
#define OVMX_CONSOLE_DEVICE      "OPA0:"
#endif

/* The Linux device that backs OPA0:. The ONE home for this substrate path;
 * every layer that needs it takes it from here, never a fresh literal. */
#define OVMX_CONSOLE_LINUX_PATH  "/dev/console"

/*
 * If `name' is a VMS console-terminal device name (TT:/TT0:/OPA0:/_OPA0:,
 * case-insensitive, with the VMS trailing ':'), copy its Linux backing path
 * into `out' and return 1. Otherwise leave `out' untouched and return 0 --
 * the caller then treats the spec as whatever else it is (a disk filespec,
 * a mailbox, ...). Never matches a disk device (DKA0:, no such alias).
 */
static inline int ovmx_console_terminal_path(const char *name,
                                             char *out, size_t outsz)
{
    char upper[64];
    size_t len;

    if (!name || !name[0] || !out || outsz == 0)
        return 0;

    len = strlen(name);
    if (len >= sizeof(upper))
        return 0;                       /* too long to be a device name */
    for (size_t i = 0; i < len; i++)
        upper[i] = (char)toupper((unsigned char)name[i]);
    upper[len] = '\0';

    /* VMS device names end with ':' -- strip it for matching. */
    if (len == 0 || upper[len - 1] != ':')
        return 0;
    upper[len - 1] = '\0';

    if (strcmp(upper, "TT")   == 0 || strcmp(upper, "TT0")   == 0 ||
        strcmp(upper, "OPA0") == 0 || strcmp(upper, "_OPA0") == 0) {
        strncpy(out, OVMX_CONSOLE_LINUX_PATH, outsz - 1);
        out[outsz - 1] = '\0';
        return 1;
    }
    return 0;
}

#endif /* OVMX_CONSOLE_H */
