/*
 * sshd_session.c - the SSH SESSION HANDOFF for the wrapped OpenVMS OpenSSH sshd
 * seam (rd vms-843a / vms-29b / vms-65b). See sshd_session.h.
 *
 * This is the FAITHFUL SSH -> DCL session establishment (design
 * docs/design-ssh-loginout-handoff.md, Option A). It replaces the retired
 * SSH C-reimpl session shim (the raw __real_execve of /vms/.../DCL.EXE, which
 * ENOENTs on a booted distro where DCL.EXE is ODS-2/ACP-only) with the ONE
 * session-establish primitive the console login and DECnet SET HOST already
 * use:
 *
 *      ovmx_vterm_create()                            # executive mints RTAn:
 *   -> VMS_IOCTL_TERM_SETLOGIN (vms-65b conveyance)   # vouch the pre-authed user
 *   -> $CREPRC(LOGINOUT.EXE, RTAn:, PRC$M_INTER|PRC$M_LOGINOUT)
 *   -> pump the SSH channel <-> the vterm until the session ends
 *
 * SSH already authenticated the user IN-PROTOCOL against the SAME SYSUAF/Purdy
 * authority (sshd_auth.c, proven vms-9cc), so LOGINOUT does NOT re-challenge:
 * we stamp the pre-authenticated user name onto the RTAn: this handoff mints,
 * and LOGINOUT -- created on that same terminal -- reads it back (network-login
 * mode, tools/vms_login.c) and skips the prompt. The stamp is authorized here
 * because this runs BEFORE OpenSSH's credential drop, while sshd is still root
 * (VMS_IOCTL_TERM_SETLOGIN is CAP_SYS_ADMIN/SETPRV-gated). The note is a NAME,
 * never a credential: LOGINOUT builds the persona from the binary SYSUAF record
 * and grants nothing beyond it.
 *
 * WHY HERE (the pre-drop window, __wrap_permanently_set_uid) AND NOT __wrap_
 * execve: the created LOGINOUT child establishes SYSTEM to read the World-denied
 * SYS$SYSTEM:SYSUAF.DAT, which needs the creator to still hold CAP_SYS_ADMIN --
 * true only before OpenSSH's permanently_set_uid drop. __wrap_execve runs AFTER
 * the drop, too late to create a privileged LOGINOUT.
 *
 * NOTHING HERE forks, execs, or opens a pty: ovmx_vterm_create is the ONE home
 * for the pty (below the VMS layer) and $CREPRC is the ONE home for process
 * creation, exactly as the DECnet CTERM host uses them
 * (tests/integration/test_creprc_session_primitive.sh gates that). This file
 * only mints, stamps, creates, and relays bytes.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/select.h>

#include "sshd_session.h"
#include "sshd_auth.h"        /* ovmx_sshd_loginout_path (the LOGINOUT image)  */
#include "sysuaf.h"
#include "vms_kif.h"          /* vms_kif_terminal_setlogin (vms-65b)          */
#include "ovmx_vterm.h"       /* ovmx_vterm_create/_delete                    */
#include "ovmx_status.h"
#include "ssdef.h"
#include "starlet.h"          /* sys$creprc                                   */
#include "descrip.h"
#include "prcdef.h"           /* PRC$M_INTER, PRC$M_LOGINOUT                  */

/* Write the whole buffer or fail. Returns 0 on success, -1 on a hard error. */
static int session_write_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return -1;
    }
    return 0;
}

/*
 * Relay the SSH channel (this process's stdin/stdout, which OpenSSH's do_child
 * has already pointed at the session channel -- pty slave for interactive, the
 * materialized BGn: socket otherwise) to and from the vterm master, until the
 * session ends. Then _exit -- this process IS the session relay and never
 * returns to OpenSSH.
 *
 *   client -> server  : stdin  -> master_fd   (keystrokes / piped DCL commands)
 *   server -> client  : master_fd -> stdout   (DCL output, the login banner)
 *
 * The session ends when the vterm master EOFs (the DCL session logged out and
 * LOGINOUT exited -> the slave closed), or the SSH client goes away. Client
 * stdin EOF alone does NOT end it: a non-interactive `ssh host <<cmds` closes
 * its stdin after the last command, but the DCL those commands drive (ending in
 * LOGOUT) must still flush its output back first -- so we stop forwarding stdin
 * and keep draining the master. Returns when the session is over; the caller
 * then releases the vterm.
 */
static void ovmx_sshd_pump(int master_fd)
{
    int stdin_open = 1;

    for (;;) {
        fd_set rfds;
        int maxfd = master_fd;
        int r;

        FD_ZERO(&rfds);
        FD_SET(master_fd, &rfds);
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &rfds);
            if (STDIN_FILENO > maxfd)
                maxfd = STDIN_FILENO;
        }

        r = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return;
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (session_write_all(master_fd, buf, (size_t)n) != 0)
                    return;
            } else if (n == 0) {
                stdin_open = 0;   /* client done sending; keep draining output */
            } else if (errno != EINTR && errno != EAGAIN) {
                stdin_open = 0;
            }
        }

        if (FD_ISSET(master_fd, &rfds)) {
            char buf[4096];
            ssize_t n = read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                if (session_write_all(STDOUT_FILENO, buf, (size_t)n) != 0)
                    return;
            } else if (n == 0) {
                return;           /* DCL session ended: the slave closed        */
            } else if (errno == EIO) {
                return;           /* pty peer gone: session over                */
            } else if (errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
                return;
            }
        }
    }
}

void ovmx_sshd_pre_drop_pw(const struct passwd *pw)
{
    sysuaf_record_t rec;
    char devnam[VMS_DEVNAM_SIZE];
    char loginout_path[512];
    int master_fd = -1;
    uint32_t st, pid = 0;

    if (pw == NULL || pw->pw_name == NULL)
        return;

    /* SYSUAF account? A non-SYSUAF name (the privsep 'sshd' user, which also
     * routes through the permanently_set_uid wrap) is NOT a login session: do
     * nothing and let OpenSSH's real drop proceed. */
    memset(&rec, 0, sizeof(rec));
    if (sysuaf_lookup(pw->pw_name, &rec) != 0)
        return;

    /* ---- from here the process NEVER returns to OpenSSH: it either becomes
     * the session relay (success) or _exits fail-closed. A SYSUAF login does
     * not fall through to OpenSSH's own credential drop + shell exec. ---- */

    /* 1. Mint the virtual terminal for this SSH channel. The name comes BACK
     *    from the executive; this process does not choose it. */
    st = ovmx_vterm_create(devnam, sizeof(devnam), &master_fd);
    if (!(st & 1)) {
        printf("%%OVMX-F-NOTERM, could not create a terminal for the SSH "
               "session (status %u)\n", (unsigned)st);
        fflush(stdout);
        _exit(1);
    }

    /* 2. Vouch the SSH-pre-authenticated user onto that RTAn: (vms-65b). We are
     *    still root here (pre-drop), so VMS_IOCTL_TERM_SETLOGIN is authorized.
     *    rec.username is the upcased SYSUAF key. */
    st = vms_kif_terminal_setlogin(devnam, rec.username);
    if (!(st & 1)) {
        printf("%%OVMX-F-NOTRUST, could not convey the authenticated identity "
               "to LOGINOUT (status %u)\n", (unsigned)st);
        fflush(stdout);
        (void)ovmx_vterm_delete(devnam, master_fd);
        _exit(1);
    }

    /* 3. Create the session: LOGINOUT.EXE bound to the vterm, PRC$M_LOGINOUT so
     *    it establishes SYSTEM, reads SYSUAF, and re-personas to the vouched
     *    user. uic=0/prvadr=NULL: the creator stamps no identity (the note is
     *    the only thing conveyed). */
    if (!ovmx_sshd_loginout_path(loginout_path, sizeof(loginout_path))) {
        (void)ovmx_vterm_delete(devnam, master_fd);
        _exit(1);
    }
    {
        struct dsc$descriptor_s img_d  = dsc$init(loginout_path);
        struct dsc$descriptor_s term_d = dsc$init(devnam);

        st = sys$creprc(&pid, &img_d, &term_d, &term_d, &term_d,
                        NULL, NULL, NULL, 0, 0, 0,
                        PRC$M_INTER | PRC$M_LOGINOUT);
        if (!(st & 1)) {
            /* No session, and nothing pretends otherwise (INV-6): there is no
             * fallback that admits the peer anyway. */
            printf("%%OVMX-F-NOSESSION, could not create the LOGINOUT session "
                   "(status %u)\n", (unsigned)st);
            fflush(stdout);
            (void)ovmx_vterm_delete(devnam, master_fd);
            _exit(1);
        }
    }

    /* 4. Relay bytes until the session ends, then release the RTAn: this
     *    handoff minted (the creator owns it, as the DECnet CTERM host does) and
     *    _exit. The relay process held the vterm master; closing it here + the
     *    executive delete withdraws the dynamic unit rather than leaking it. */
    ovmx_sshd_pump(master_fd);
    (void)ovmx_vterm_delete(devnam, master_fd);
    _exit(0);
}
