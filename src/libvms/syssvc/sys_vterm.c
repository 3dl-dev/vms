/*
 * sys_vterm.c - the VIRTUAL TERMINAL service (rd vms-f40): mint and release the
 * RTAn: an inbound network login session runs on. See ovmx_vterm.h for the
 * full reasoning; the short version is that this is the floor the DECnet CTERM
 * host stands on so that IT does not have to open a pty, and therefore cannot
 * quietly grow a login path of its own.
 *
 * THE PTY LIVES HERE, BELOW THE VMS LAYER, for the same reason the open/dup2/
 * TIOCSCTTY sequence lives inside $CREPRC (sys_process.c,
 * creprc_bind_terminal): it is a substrate mechanic, it has exactly one
 * legitimate home, and every home it has ABOVE the VMS layer has historically
 * become a second, weaker login path. tests/integration/
 * test_creprc_session_primitive.sh enforces that -- it scans the network
 * daemons for openpty/fork/exec and fails if any of them come back.
 *
 * ORDER MATTERS. The substrate pair is prepared FIRST and the executive row is
 * entered SECOND, because the row must name a device that already exists: a
 * row pointing at a pty that failed to open would be a device $CREPRC could
 * resolve and then fail to bind, i.e. a half-created session. If the executive
 * refuses (no free unit, module absent), the pty is closed again and NOTHING
 * is left behind -- INV-6, no invented device, no session.
 *
 * Clean-room (Rule 8): the RTAn: naming, its OPA0:-shape characteristics and
 * its lifetime (appears with the session, disappears with it) are
 * doc/oracle-derived (docs/oracle/vax73-terminal-device.md,
 * docs/oracle/vax-sethost-cterm.md sec 2); the pty backing is the labelled
 * OVMX substrate choice. No VSI source.
 */

#define _GNU_SOURCE     /* posix_openpt, ptsname_r */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ovmx_vterm.h"
#include "ssdef.h"
#include "vms_kif.h"

/* The executive records a device's backing RELATIVE to /dev (a disk row holds
 * "vda", not "/dev/vda"). Strip the prefix the substrate hands us so the one
 * place that knows it is this file and $CREPRC's resolver. */
static const char *vterm_strip_dev_prefix(const char *path)
{
    static const char pfx[] = "/dev/";
    size_t n = sizeof(pfx) - 1;

    if (path && strncmp(path, pfx, n) == 0)
        return path + n;
    return path;
}

uint32_t ovmx_vterm_create(char *devnam, size_t devnam_size, int *master_fd)
{
    char slave[256];
    const char *backing;
    int mfd;
    uint32_t st;

    if (!devnam || devnam_size == 0 || !master_fd)
        return SS$_BADPARAM;

    devnam[0] = '\0';
    *master_fd = -1;

    /* 1. The substrate pair. O_NOCTTY: this process is the network daemon, not
     *    the session -- the controlling-terminal claim belongs to the created
     *    process and is made inside $CREPRC. */
    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0)
        return SS$_DEVALLOC;
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0) {
        close(mfd);
        return SS$_DEVOFFLINE;
    }
    /* ptsname_r() is a GNU extension; the vax substrate (NetBSD) has only the
     * POSIX ptsname(). Both are used the same way here -- the name is copied
     * out immediately -- and this is the only substrate difference in the
     * service, kept to one branch rather than spread through the caller. */
#if defined(__linux__)
    if (ptsname_r(mfd, slave, sizeof(slave)) != 0) {
        close(mfd);
        return SS$_DEVOFFLINE;
    }
#else
    {
        const char *sn = ptsname(mfd);
        if (!sn) {
            close(mfd);
            return SS$_DEVOFFLINE;
        }
        snprintf(slave, sizeof(slave), "%s", sn);
    }
#endif

    backing = vterm_strip_dev_prefix(slave);
    if (!backing || !backing[0]) {
        close(mfd);
        return SS$_DEVOFFLINE;
    }

    /* 2. The executive row. The NAME comes back from the executive -- this
     *    process does not choose it and could not know what is free. */
    st = vms_kif_terminal_create(backing, devnam, (uint32_t)devnam_size);
    if (!(st & 1)) {
        close(mfd);
        devnam[0] = '\0';
        return st;          /* the executive's own honest status */
    }
    if (devnam[0] == '\0') {
        /* A success that named no device is not a success. Fail rather than
         * hand a caller an empty name it would turn into a $CREPRC refusal it
         * cannot explain. */
        close(mfd);
        return SS$_DEVOFFLINE;
    }

    *master_fd = mfd;
    return SS$_NORMAL;
}

uint32_t ovmx_vterm_delete(const char *devnam, int master_fd)
{
    uint32_t st = SS$_NORMAL;

    if (devnam && devnam[0])
        st = vms_kif_terminal_delete(devnam);
    if (master_fd >= 0)
        close(master_fd);
    return st;
}
