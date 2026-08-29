/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcpip_inetd.h - TCP/IP Services for OVMX: the AUXILIARY SERVER engine
 * (TCPIP$INETD-equivalent, rd vms-cdb9), the Phase-4 rung under vms-67f (the
 * TCP/IP layered product) and the precondition that unblocks the OpenSSH sshd
 * ladder (vms-9ef, "VMSSSHD.EXE launched by the auxiliary server").
 *
 * WHAT THIS IS. On real OpenVMS, TCP/IP Services runs a master listener --
 * the auxiliary server, TCPIP$INETD -- that binds the well-known port of every
 * ENABLED service, waits for an inbound connection, and on connect hands the
 * connection to a freshly created process running that service's image (the
 * service reads its client on SYS$INPUT and answers on SYS$OUTPUT). It is the
 * VMS analogue of Unix inetd. This header is the OVMX engine for exactly that:
 * read the service database, bind + listen each service's port over the
 * executive BGn: seam, accept, and spawn the configured service image with the
 * accepted connection as its stdin/stdout.
 *
 * BUILT ATOP THE PROVEN SERVER VENEER (Rule 1 / vms-ports-build ladder). The
 * bind/listen/accept path is NOT re-implemented here: it is the BSD-sockets
 * RTL veneer over BGn: (src/vmstcpip/sockets/vms_bgsock.c) whose server half
 * (ovmx_bind / ovmx_listen / ovmx_accept) the vms-698 server seam landed and
 * the sshd oracle (vms-843 / vms-9ac / vms-0cd) proves end to end. This engine
 * calls that veneer and adds only the inetd control loop on top.
 *
 * HOW A CONNECTION REACHES THE SERVICE IMAGE (Rule 9, the executive path).
 * ovmx_accept() returns an accepted BG channel whose socket is executive-
 * resident (vms.ko, over the host in-kernel socket API) -- there is NO
 * userspace socket on the VMS side. ovmx_materialize_fd() turns that accepted
 * channel into a REAL, dup2-able fd whose read()/write() route THROUGH the
 * executive to that same socket (the [bgconn] anon_inode, vms-0cd RUNG-3b);
 * the spawned service does ordinary read()/write() on its stdin/stdout and the
 * bytes transit the executive, never a host socketpair. This is the identical
 * mechanism a wrapped OpenSSH sshd uses to hand an accepted connection to its
 * per-session child (test_syssvc_ssh_server) -- which is precisely why this
 * rung unblocks vms-9ef.
 *
 * fork()+execv() OF THE CONFIGURED SERVICE IMAGE IS THE inetd CONTRACT, NOT A
 * HOST SHELL-OUT. The auxiliary server creates a process running the VMS image
 * named in the service database (the OVMX analogue of $CREPRC of the service
 * image); it does NOT shell out to a host networking tool, and it never
 * fabricates a per-process socket. If /dev/vms is absent the veneer's
 * ovmx_socket()/ovmx_bind() fail honestly (SS$_NOSUCHDEV -> ENODEV) and the
 * auxiliary server cannot start -- it reports that, never a fake (Rule 9 /
 * INV-6). The one host facility used is process creation itself (fork/execv),
 * exactly as inetd's whole purpose requires; the NETWORK path is 100 %
 * executive.
 *
 * CLEAN-ROOM (Rule 8). The inetd BEHAVIOUR -- a master listener that binds
 * enabled services' ports and spawns the service image on connect -- is from
 * the public VSI OpenVMS TCP/IP Services documentation (the "auxiliary server"
 * / TCPIP$INETD description) and the Unix inetd model. VSI does NOT publish the
 * byte-level layout of its own service database (the internal TCPIP$SERVICE
 * store), so the TCPIP$SERVICE.DAT LINE FORMAT parsed below is an OVMX DESIGN
 * CHOICE -- a plain whitespace-separated text record, the same idiom as
 * SYS$STARTUP:VMS$VMS.DAT (docs/design-boot-faithful.md) -- and is NOT
 * presented as VMS-authentic. No VSI/HPE source or binary was read.
 *
 * WHY A SINGLE-HEADER LIBRARY (same rationale as tcpip_client.h / tcpip_ping.h
 * / tcpip_config.h). Every function is `static inline` so the SAME engine backs
 * two consumers -- the shipped auxiliary-server image (tools/, TCPIP$INETD.EXE)
 * and the QEMU proof (tests/qemu/test_syssvc_tcpip_inetd.c, which drives it
 * against a real /dev/vms) -- and a translation unit that uses only a subset
 * draws no unused-function diagnostic.
 *
 * SCOPE (this rung). Bind + listen + accept + spawn over IPv4/loopback for the
 * services named in TCPIP$SERVICE.DAT; one accepted connection dispatched to
 * the configured image at a time per listener (the classic inetd wait model).
 * DEFERRED honestly to later rungs (NOT faked here): the persistent binary
 * service database (#878), UDP/dgram services, the "nowait" concurrent model,
 * per-service run-as/user identity, and access-control lists.
 */

#ifndef _OVMX_TCPIP_INETD_H
#define _OVMX_TCPIP_INETD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "vms_bgsock.h"      /* the PROVEN server veneer: ovmx_bind/listen/accept
                              * + ovmx_materialize_fd + ovmx_socket_close */

/* ---- Service database record ---------------------------------------------
 *
 * OVMX DESIGN CHOICE (Rule 8): TCPIP$SERVICE.DAT is a whitespace-separated
 * text file, one ENABLED service per line
 *
 *     service-name  port  image-filespec  [image-args...]
 *
 * Lines beginning with "!" and blank lines are comments/ignored. "port" is the
 * decimal TCP port the auxiliary server binds; "image-filespec" is the service
 * image the auxiliary server runs on an inbound connection, and any trailing
 * tokens are passed to it as arguments. This line format is an OVMX invention
 * (VSI's internal service-store layout is not published) and is NOT presented
 * as VMS-authentic.
 */
#define TCPIP_INETD_MAX_SERVICES 16
#define TCPIP_INETD_NAME_MAX     32
#define TCPIP_INETD_PATH_MAX     256
#define TCPIP_INETD_ARGS_MAX     256

struct tcpip_service {
    char     name[TCPIP_INETD_NAME_MAX];
    uint16_t port;
    char     image[TCPIP_INETD_PATH_MAX];   /* argv[0]: the service image */
    char     args[TCPIP_INETD_ARGS_MAX];    /* whitespace-separated extra argv */
};

/* Parse the TCPIP$SERVICE.DAT text in `text` into `svcs[0..max-1]`. Returns the
 * number of services parsed (>= 0), or -1 on a bad argument. Malformed lines
 * (missing port/image, non-numeric port) are skipped, not faked. */
static inline int tcpip_inetd_parse_db(const char *text,
                                       struct tcpip_service *svcs, int max)
{
    int n = 0;
    const char *p = text;

    if (!text || !svcs || max <= 0)
        return -1;

    while (*p && n < max) {
        const char *eol = p;
        char line[512];
        size_t llen;
        char *tok, *save = NULL;
        struct tcpip_service s;
        long port;

        while (*eol && *eol != '\n')
            eol++;
        llen = (size_t)(eol - p);
        if (llen >= sizeof(line))
            llen = sizeof(line) - 1;
        memcpy(line, p, llen);
        line[llen] = '\0';
        p = (*eol == '\n') ? eol + 1 : eol;

        /* Skip leading whitespace; ignore blank and "!"-comment lines. */
        {
            char *q = line;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == '\0' || *q == '!')
                continue;
        }

        memset(&s, 0, sizeof(s));

        tok = strtok_r(line, " \t", &save);         /* service-name */
        if (!tok) continue;
        strncpy(s.name, tok, sizeof(s.name) - 1);

        tok = strtok_r(NULL, " \t", &save);         /* port */
        if (!tok) continue;
        errno = 0;
        port = strtol(tok, NULL, 10);
        if (errno != 0 || port <= 0 || port > 65535)
            continue;
        s.port = (uint16_t)port;

        tok = strtok_r(NULL, " \t", &save);         /* image-filespec */
        if (!tok) continue;
        strncpy(s.image, tok, sizeof(s.image) - 1);

        /* Any remaining tokens are the image's arguments (kept as a single
         * whitespace-separated string, re-split at spawn). */
        tok = strtok_r(NULL, "", &save);
        if (tok) {
            while (*tok && isspace((unsigned char)*tok)) tok++;
            strncpy(s.args, tok, sizeof(s.args) - 1);
        }

        svcs[n++] = s;
    }
    return n;
}

/* Bind + listen the service's port over the executive BGn: seam, atop the
 * proven veneer. Returns the listening OVMX socket handle (>= 0), or -1 with
 * errno preserved (ENODEV = no /dev/vms -> the auxiliary server fails honestly,
 * never a per-process fake). INADDR_ANY so the well-known port is reachable on
 * every interface, exactly as the auxiliary server binds. */
static inline int tcpip_inetd_listen(const struct tcpip_service *svc)
{
    int s;
    struct sockaddr_in la;
    int one = 1;

    if (!svc) { errno = EINVAL; return -1; }

    s = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;                              /* errno (ENODEV) preserved */

    /* Best-effort SO_REUSEADDR so a restart of the auxiliary server can re-bind
     * a port still in TIME_WAIT; an executive that does not honor it is not an
     * error here. */
    (void)ovmx_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_port = htons(svc->port);
    la.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ovmx_bind(s, (struct sockaddr *)&la, sizeof(la)) < 0) {
        int e = errno; ovmx_socket_close(s); errno = e; return -1;
    }
    if (ovmx_listen(s, 5) < 0) {
        int e = errno; ovmx_socket_close(s); errno = e; return -1;
    }
    return s;
}

/* Spawn the configured service image on an ALREADY-ACCEPTED connection handle:
 * materialize the accepted BG channel as a real executive-backed fd, then
 * fork()+execv() the service image with that fd as its SYS$INPUT (stdin) and
 * SYS$OUTPUT (stdout) -- the inetd contract. The parent closes its copy of the
 * accepted handle (the connection stays alive on the child's materialized fd,
 * whose last-reference $DASSGN drives the FIN, vms-0cd) and returns the child
 * pid (> 0), or -1 with errno on failure. */
static inline pid_t tcpip_inetd_spawn(int accepted_h, const struct tcpip_service *svc)
{
    int rfd;
    pid_t pid;
    char argbuf[TCPIP_INETD_ARGS_MAX];
    char *argv[16];
    int argc = 0;

    if (!svc) { errno = EINVAL; return -1; }

    /* Materialize the accepted executive socket as a REAL dup2-able fd whose
     * read/write route to that socket through vms.ko (Rule 9: the bytes transit
     * the executive, never a host socketpair). */
    rfd = ovmx_materialize_fd(accepted_h);
    if (rfd < 0)
        return -1;                              /* errno (ENODEV) preserved */

    /* Build argv = { image, args... } for the service image. */
    argv[argc++] = (char *)svc->image;
    argbuf[0] = '\0';
    if (svc->args[0] != '\0') {
        char *tok, *save = NULL;
        strncpy(argbuf, svc->args, sizeof(argbuf) - 1);
        argbuf[sizeof(argbuf) - 1] = '\0';
        for (tok = strtok_r(argbuf, " \t", &save);
             tok && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1;
             tok = strtok_r(NULL, " \t", &save))
            argv[argc++] = tok;
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        int e = errno; close(rfd); errno = e; return -1;
    }
    if (pid == 0) {
        /* Child: the accepted connection becomes the service image's SYS$INPUT
         * and SYS$OUTPUT. Both fds are the materialized [bgconn] fd, so the
         * service's read()/write() transit the executive socket. */
        dup2(rfd, STDIN_FILENO);
        if (dup2(rfd, STDOUT_FILENO) != STDOUT_FILENO) _exit(126); /* NEGCTL tcpip-inetd-reply-not-connected */
        if (rfd > STDERR_FILENO)
            close(rfd);
        execv(svc->image, argv);
        _exit(127);                             /* execv failed */
    }

    /* Parent: drop our copies. Closing the materialized fd and the accepted BG
     * handle here does NOT tear the connection down -- the child holds the live
     * reference (ovmx_socket_close drops a ref, the FIN is at the last one). */
    close(rfd);
    ovmx_socket_close(accepted_h);
    return pid;
}

/* Accept one inbound connection on a listening handle and dispatch it to the
 * configured service image. Blocks in ovmx_accept() until a client connects.
 * On success returns the spawned service's pid (> 0) and, if `peer` is
 * non-NULL, fills it with the client's address. Returns -1 with errno on
 * failure (ENODEV = no /dev/vms). */
static inline pid_t tcpip_inetd_accept_dispatch(int listen_h,
                                                const struct tcpip_service *svc,
                                                struct sockaddr_in *peer)
{
    int a;
    struct sockaddr_in pa;
    socklen_t pl = sizeof(pa);

    if (!svc) { errno = EINVAL; return -1; }

    memset(&pa, 0, sizeof(pa));
    a = ovmx_accept(listen_h, (struct sockaddr *)&pa, &pl);
    if (a < 0)
        return -1;                              /* errno (ENODEV) preserved */
    if (peer)
        *peer = pa;
    return tcpip_inetd_spawn(a, svc);
}

#endif /* _OVMX_TCPIP_INETD_H */
