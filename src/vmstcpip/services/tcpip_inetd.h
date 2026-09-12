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
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <fcntl.h>           /* access(2) X_OK for the stage-once guard */
#include <sys/stat.h>        /* mkdir(2) for the stage dir */
#include "rms/rms.h"         /* rms_stage_over_acp: materialize a SYS$SYSTEM: image
                              * off the ODS-2 ACP into a Linux-execve-able tmpfs
                              * path (the shared stager DCL RUN + PID1 use) */
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

/* Concurrency ceiling for spawned service children (rd vms-bb4, R4 G2). Real
 * TCPIP$INETD caps concurrent service processes; without a cap, a hostile client
 * that opens connections faster than a reading/slow service exits accumulates
 * children unboundedly -- a fork-bomb-via-network the moment a reading service
 * (the SSH rung) is enabled. The control loop counts live children and applies
 * accept BACK-PRESSURE at the cap (stops selecting the listeners for POLLIN, so
 * new SYNs queue in the listen backlog) until a child exits. Faithful: a
 * concurrency limit, not a Linux sandbox. */
#define TCPIP_INETD_MAXCHILD     64

struct tcpip_service {
    char     name[TCPIP_INETD_NAME_MAX];
    uint16_t port;
    char     user[TCPIP_INETD_NAME_MAX];    /* per-service run-as account (SYSUAF
                                             * username, R4 G1); "" = none configured */
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

        /* Next token is either a per-service run-as USERNAME (R4 G1) or the
         * image-filespec. Disambiguate structurally: an image filespec always
         * carries device/path punctuation (a VMS "SYS$SYSTEM:..." has ':', a
         * Linux "/path" has '/'), a VMS username never does. So a token with
         * neither ':' nor '/' is the run-as account and the image is the token
         * after it; a token with either is the image and no account is
         * configured. This keeps the older "name port image [args]" format
         * parsing unchanged (user stays ""). */
        tok = strtok_r(NULL, " \t", &save);
        if (!tok) continue;
        if (strchr(tok, ':') == NULL && strchr(tok, '/') == NULL) {
            strncpy(s.user, tok, sizeof(s.user) - 1);
            tok = strtok_r(NULL, " \t", &save);     /* image-filespec */
            if (!tok) continue;
        }
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

/* Resolve a service's image-filespec to an execve-able Linux path in
 * out[0..out_sz). A VMS filespec (e.g. "SYS$SYSTEM:TCPIP$DAYTIME.EXE") names a
 * file on the ACP-mounted ODS-2 system disk -- NOT on the boot initramfs Linux
 * VFS -- so its GENUINE bytes are staged off the ACP into a tmpfs path
 * (rms_stage_over_acp -- the same materialize-then-exec the executive already
 * does for DCL RUN and for PID1's boot images), staged once per image (X_OK
 * guard on the staged copy). A leading '/' is an already-Linux-reachable path
 * (the in-guest test harness, or a future initramfs image) -- copied through,
 * no staging. Returns 0 with the resolved path in `out`; -1 with errno on a
 * name too long (ENAMETOOLONG) or a stage failure (ENOENT -- image not on the
 * ACP volume). No fabrication (INV-6): a stageable image or an honest failure. */
static inline int tcpip_inetd_resolve_image(const struct tcpip_service *svc,
                                            char *out, size_t out_sz)
{
    if (!svc || !out || out_sz == 0) { errno = EINVAL; return -1; }

    if (svc->image[0] == '/') {                 /* already Linux-reachable */
        if ((size_t)snprintf(out, out_sz, "%s", svc->image) >= out_sz) {
            errno = ENAMETOOLONG; return -1;
        }
        return 0;
    }

    {
        const char *colon = strrchr(svc->image, ':');
        const char *base  = colon ? colon + 1 : svc->image;
        (void)mkdir("/tmp/ovmx_inetd", 0755);
        if ((size_t)snprintf(out, out_sz, "/tmp/ovmx_inetd/%s", base) >= out_sz) {
            errno = ENAMETOOLONG; return -1;
        }
        if (access(out, X_OK) != 0) {
            uint32_t st = rms_stage_over_acp(svc->image, out);
            if (!(st & 1u)) {                   /* VMS status: low bit set == success */
                errno = ENOENT; return -1;      /* image not on the ACP volume */
            }
        }
    }
    return 0;
}

/* Bind-time PRE-FLIGHT: can this service's image actually be staged and
 * executed? The auxiliary server must NOT bind a well-known port for a service
 * it cannot deliver -- a "bound but unserviceable" facade in which the operator
 * sees the service listening while every client connect is silently dropped
 * when spawn's staging/execv fails (INV-6). Real TCPIP$INETD validates a
 * service's image when the service is enabled; this is the OVMX analogue. The
 * control loop calls this after a successful listen and refuses to advertise a
 * service that fails it. Returns 0 if the image resolves to an executable Linux
 * path; -1 with errno otherwise. Side effect: stages the image once (X_OK
 * guard), so the first real connection is not slowed by staging. */
static inline int tcpip_inetd_preflight(const struct tcpip_service *svc)
{
    char path[TCPIP_INETD_PATH_MAX];
    if (tcpip_inetd_resolve_image(svc, path, sizeof(path)) < 0)
        return -1;                              /* errno preserved */
    if (access(path, X_OK) != 0)
        return -1;                              /* errno (EACCES/ENOENT) preserved */
    return 0;
}

/* Per-service identity establishment, run in the forked child before execv
 * (rd vms-8bd, R4 G1). It must drop the child from INETD's SYSTEM/all-privs
 * identity to the service's configured run-as account and return 0, or return
 * <0 to FAIL-CLOSED (the child then _exit()s WITHOUT execv -- the service is not
 * launched, and NEVER inherits SYSTEM). The production image passes the real
 * establisher (tcpip_inetd_establish_service_identity, tcpip_inetd_ident.c); a
 * transport test may pass NULL to skip the drop (identity is proven separately by
 * the ident unit test + the booted-runtime e2e, not the transport round-trip). */
typedef int (*tcpip_inetd_identity_fn)(const struct tcpip_service *svc);

/* Spawn the configured service image on an ALREADY-ACCEPTED connection handle:
 * materialize the accepted BG channel as a real executive-backed fd, then
 * fork()+execv() the service image with that fd as its SYS$INPUT (stdin) and
 * SYS$OUTPUT (stdout) -- the inetd contract. In the child, before execv, drop to
 * the service's run-as identity via `identity_fn` (fail-closed: if it returns <0
 * the service is NOT launched, R4 G1); NULL skips the drop (transport tests only).
 * The parent closes its copy of the accepted handle (the connection stays alive on
 * the child's materialized fd, whose last-reference $DASSGN drives the FIN,
 * vms-0cd) and returns the child pid (> 0), or -1 with errno on failure. */
static inline pid_t tcpip_inetd_spawn(int accepted_h, const struct tcpip_service *svc,
                                      tcpip_inetd_identity_fn identity_fn)
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

    /* Resolve the service image to an execve-able Linux path (staging its bytes
     * off the ACP-mounted ODS-2 disk for a VMS filespec, or passing a leading-'/'
     * path through). execve's argv[0] stays the VMS filespec so the service sees
     * its faithful name. Resolution fails honestly (ENOENT/ENAMETOOLONG) rather
     * than fabricating a launch (INV-6) -- the SAME check the control loop's
     * pre-flight runs before it agrees to advertise the service. */
    char staged[TCPIP_INETD_PATH_MAX];
    if (tcpip_inetd_resolve_image(svc, staged, sizeof(staged)) < 0) {
        int e = errno; close(rfd); ovmx_socket_close(accepted_h); errno = e; return -1;
    }
    const char *exec_path = staged;

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
        /* R4 G1: drop from INETD's SYSTEM/all-privs identity to the service's
         * configured run-as account BEFORE execv. FAIL-CLOSED -- if the identity
         * cannot be established (no account, unknown account, executive refused,
         * or the credential drop failed) the service is NOT launched; it never
         * runs as SYSTEM. (identity_fn NULL = transport test, no drop.) */
        if (identity_fn != NULL && identity_fn(svc) != 0)
            _exit(125);                         /* fail-closed: identity not established */
        execv(exec_path, argv);
        /* execv returned -> it FAILED. Two sinks, asymmetric (R4 posture, #1168):
         * the FULL detail (which image, errno) to stderr -> SYS$MANAGER:TCPIP$INETD.LOG
         * (an operator record), and a GENERIC "service unavailable" to the accepted
         * socket (STDOUT, dup2'd above) -- an unauthenticated client is not told the
         * image path or errno. Distinguishes an execv failure from an identity
         * refusal in the operator log without leaking internals to the client
         * (rd vms-8bd). */
        {
            char eb[256];
            int en = snprintf(eb, sizeof(eb),
                              "%%OVMX-F-NOSTART, service image %s could not be "
                              "launched: %s\n", exec_path, strerror(errno));
            if (en > 0) {
                size_t el = (en < (int)sizeof(eb)) ? (size_t)en : sizeof(eb) - 1;
                (void)!write(STDERR_FILENO, eb, el);
            }
            static const char generic[] = "%OVMX-F-NOSVC, service unavailable\n";
            (void)!write(STDOUT_FILENO, generic, sizeof(generic) - 1);
        }
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
 * configured service image, dropping to the service's run-as identity in the
 * spawned child via `identity_fn` (fail-closed, R4 G1; NULL = transport test).
 * Blocks in ovmx_accept() until a client connects. On success returns the
 * spawned service's pid (> 0) and, if `peer` is non-NULL, fills it with the
 * client's address. Returns -1 with errno on failure (ENODEV = no /dev/vms). */
static inline pid_t tcpip_inetd_accept_dispatch(int listen_h,
                                                const struct tcpip_service *svc,
                                                struct sockaddr_in *peer,
                                                tcpip_inetd_identity_fn identity_fn)
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
    return tcpip_inetd_spawn(a, svc, identity_fn);
}

/* The fork-flood back-pressure gate (rd vms-bb4, R4 G2): may the auxiliary server
 * accept another inbound connection, given `live_children` service processes
 * currently running? Returns 0 at/above TCPIP_INETD_MAXCHILD -- the control loop
 * then stops selecting its listeners for POLLIN, so new connections queue in the
 * listen backlog instead of forking an unbounded number of children, until a
 * child exits and the count drops back below the cap. */
static inline int tcpip_inetd_may_accept(int live_children)
{
    return live_children < TCPIP_INETD_MAXCHILD;
}

#endif /* _OVMX_TCPIP_INETD_H */
