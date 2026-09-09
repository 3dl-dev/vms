/*
 * dnet_cterm_host.c - an inbound $ SET HOST reaches an AUTHENTICATED LOGINOUT
 * prompt (rd vms-f40). Read dnet_cterm_host.h first: it carries the whole
 * argument, the oracle that grounds it, and the no-auth hole this replaces.
 *
 * The body below is deliberately short, and every line of it is a VMS system
 * service or a CTERM field. That is the point. The sequence is:
 *
 *      inbound connect data (untrusted bytes)
 *        -> dnet_cterm_sc_connect_parse   (bounded; rejects malformed)
 *        -> object 42?                     (else refuse)
 *        -> ovmx_vterm_create()            (executive mints RTAn:)
 *        -> $CREPRC(LOGINOUT.EXE, RTAn:, PRC$M_INTER|PRC$M_LOGINOUT)
 *        -> LOGINOUT challenges the remote user on that terminal
 *
 * NOTHING IN THIS FILE CALLS fork, execvp, openpty OR dup2, and nothing in it
 * ever may: those are the four mechanics the deleted no-auth path used, they
 * have exactly one legitimate home each ($CREPRC's creprc_bind_terminal and
 * the virtual-terminal service, both below the VMS layer), and
 * tests/integration/test_creprc_session_primitive.sh check 5 scans this file
 * for them. This paragraph naming them is deliberate: that gate strips
 * comments before scanning, and its own negative control proves it by
 * requiring that these words HERE do not read as calls.
 *
 * There is no branch out of the sequence above. In particular there is no path from the
 * decoded source identity to the created process: $CREPRC is passed uic = 0
 * and prvadr = NULL ("the creator's defaults") and PRC$M_LOGINOUT tells it not
 * to stamp even those, so the session starts with NO identity of anyone's
 * choosing and acquires one only when LOGINOUT re-personas it after a
 * successful SYSUAF authentication. The DECnet daemon therefore never holds,
 * forges, or passes a credential -- which is exactly why this call is safe to
 * make from a daemon serving unauthenticated peers.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "dnet_cterm_host.h"

#include "vmsfs/filespec.h"   /* vmsfs_to_linux_path()                       */
#include "ovmx_layout.h"      /* VMS_LOGINOUT_PATH, ovmx_boot_stage_exec_path */
#include "ovmx_vterm.h"       /* ovmx_vterm_create/_delete                    */
#include "ovmx_status.h"
#include "ssdef.h"
#include "starlet.h"          /* sys$creprc                                   */
#include "descrip.h"
#include "prcdef.h"           /* PRC$M_INTER, PRC$M_LOGINOUT                  */
#include "vms_kif.h"          /* vms_kif_getjpi_pid                           */

/*
 * Resolve SYS$SYSTEM:LOGINOUT.EXE the same way JOB_CONTROL does -- through the
 * VMS filespec translator, then the boot-staging bridge when a staged copy is
 * present. Same image, same resolution, one authenticator (design sec 2).
 */
static int cterm_host_loginout_path(char *out, size_t outsz)
{
    char staged[512];

    if (!out || outsz == 0)
        return 0;
    /* The same fall-back-to-the-spec shape JOB_CONTROL and PID 1 use: a
     * substrate that does not translate keeps the VMS spec and fails honestly
     * at activation rather than silently naming something else. */
    if (vmsfs_to_linux_path(VMS_LOGINOUT_PATH, out, outsz) != 1)
        snprintf(out, outsz, "%s", VMS_LOGINOUT_PATH);
    if (out[0] == '\0')
        return 0;
    if (ovmx_boot_stage_exec_path(out, staged, sizeof(staged)) &&
        access(staged, X_OK) == 0)
        snprintf(out, outsz, "%s", staged);
    return 1;
}

uint32_t dnet_cterm_host_open(struct dnet_cterm_host_session *hs,
                              const uint8_t *conn_data, size_t conn_len,
                              uint16_t peer_addr)
{
    char loginout_path[512];
    uint32_t st;

    if (!hs || !conn_data)
        return SS$_BADPARAM;

    memset(hs, 0, sizeof(*hs));
    hs->master_fd = -1;
    if (dnet_cterm_session_init(&hs->cterm, DNET_CTERM_ROLE_HOST) != DNET_CTERM_OK)
        return SS$_BADPARAM;

    /* 1. DECODE THE UNTRUSTED CONNECT. Bounded, and refused rather than
     *    clipped -- these bytes came from a peer that has authenticated
     *    nothing. A malformed connect ends here, before any device exists. */
    if (dnet_cterm_sc_connect_parse(conn_data, conn_len, &hs->sc) != DNET_CTERM_OK)
        return SS$_BADPARAM;
    if (hs->sc.dst_format != DNET_SC_FMT_OBJECT ||
        hs->sc.dst_object != DNET_CTERM_OBJECT)
        return SS$_BADPARAM;

    /* 2. The carried identity becomes PROXY INFORMATION and nothing else --
     *    the oracle's "Remote Port Info: 1025::SYSTEM". The ADDRESS half comes
     *    from the engine's decode of the routing header, not from anything the
     *    peer wrote in the connect message, so a peer cannot name itself
     *    something it is not on the accounting surface. */
    (void)dnet_cterm_remote_port_info(&hs->sc, peer_addr, hs->remote_port_info,
                                      sizeof(hs->remote_port_info));

    /* 3. MINT THE TERMINAL, in the executive. The name comes BACK; this
     *    process does not choose it. */
    st = ovmx_vterm_create(hs->devnam, sizeof(hs->devnam), &hs->master_fd);
    if (!(st & 1)) {
        hs->master_fd = -1;
        hs->devnam[0] = '\0';
        return st;
    }

    /* The daemon multiplexes many things on one loop; it must never block on
     * one session's terminal. */
    {
        int fl = fcntl(hs->master_fd, F_GETFL, 0);
        if (fl >= 0)
            (void)fcntl(hs->master_fd, F_SETFL, fl | O_NONBLOCK);
    }

    if (!cterm_host_loginout_path(loginout_path, sizeof(loginout_path))) {
        (void)ovmx_vterm_delete(hs->devnam, hs->master_fd);
        hs->master_fd = -1;
        hs->devnam[0] = '\0';
        return SS$_NOSUCHDEV;
    }

    /* 4. CREATE THE SESSION. The one system service, the same one the console
     *    login uses: "create a process running LOGINOUT.EXE bound to
     *    terminal-device RTAn:". No identity is passed (see the file header). */
    {
        struct dsc$descriptor_s img_d  = dsc$init(loginout_path);
        struct dsc$descriptor_s term_d = dsc$init(hs->devnam);
        uint32_t pid = 0;

        st = sys$creprc(&pid, &img_d, &term_d, &term_d, &term_d,
                        NULL, NULL, NULL, 0, 0, 0,
                        PRC$M_INTER | PRC$M_LOGINOUT);
        if (!(st & 1)) {
            /* THE SESSION WAS NOT CREATED AND NOTHING PRETENDS OTHERWISE
             * (the JOB_CONTROL ruling, vms-72c, restated for the network
             * caller): there is no fallback that admits the peer anyway. */
            (void)ovmx_vterm_delete(hs->devnam, hs->master_fd);
            hs->master_fd = -1;
            hs->devnam[0] = '\0';
            return st;
        }
        hs->session_pid = pid;
    }

    hs->active = 1;
    return SS$_NORMAL;
}

long dnet_cterm_host_read(struct dnet_cterm_host_session *hs,
                          uint8_t *buf, size_t cap)
{
    ssize_t n;

    if (!hs || !buf || cap == 0 || hs->master_fd < 0)
        return -1;

    n = read(hs->master_fd, buf, cap);
    if (n > 0)
        return (long)n;
    if (n == 0)
        return -1;                        /* the session's terminal closed */
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return 0;                         /* nothing right now             */
    if (errno == EIO)
        return -1;                        /* pty peer gone: session over   */
    return -1;
}

long dnet_cterm_host_write(struct dnet_cterm_host_session *hs,
                           const uint8_t *buf, size_t len)
{
    size_t done = 0;

    if (!hs || !buf || hs->master_fd < 0)
        return -1;

    while (done < len) {
        ssize_t n = write(hs->master_fd, buf + done, len - done);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR))
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;                        /* caller retries the remainder  */
        return -1;
    }
    return (long)done;
}

int dnet_cterm_host_fd(const struct dnet_cterm_host_session *hs)
{
    return hs ? hs->master_fd : -1;
}

int dnet_cterm_host_alive(const struct dnet_cterm_host_session *hs)
{
    struct vms_procinfo info;

    if (!hs || !hs->active || hs->session_pid == 0)
        return 0;

    /* ASK THE EXECUTIVE. An interactive process is ownerless -- the top of its
     * own job -- so this process has no child to waitpid() for and must read
     * the session's life out of the executive, exactly as JOB_CONTROL does for
     * the console session it creates. */
    memset(&info, 0, sizeof(info));
    return (vms_kif_getjpi_pid(hs->session_pid, &info) & 1) ? 1 : 0;
}

uint32_t dnet_cterm_host_close(struct dnet_cterm_host_session *hs)
{
    uint32_t st;

    if (!hs)
        return SS$_BADPARAM;

    st = ovmx_vterm_delete(hs->devnam, hs->master_fd);
    hs->master_fd = -1;
    hs->devnam[0] = '\0';
    hs->active = 0;
    return st;
}
