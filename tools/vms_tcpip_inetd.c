/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_tcpip_inetd.c - the OVMX auxiliary-server image (TCPIP$INETD.EXE, rd
 * vms-cdb9): the master listener that reads SYS$SYSTEM:TCPIP$SERVICE.DAT, binds
 * each enabled service's well-known port over the executive BGn: seam, and
 * spawns the configured service image on an inbound connection.
 *
 * This is a THIN image over the shared engine (src/vmstcpip/services/
 * tcpip_inetd.h): the same header the QEMU proof (test_syssvc_tcpip_inetd)
 * drives against a real /dev/vms. All the network path is the proven server
 * veneer (ovmx_bind/listen/accept + ovmx_materialize_fd, vms-698 / vms-0cd).
 *
 * Rule 9 (INV-6): every listener is executive-resident over /dev/vms. With no
 * executive the veneer fails honestly (SS$_NOSUCHDEV -> ENODEV) and this image
 * reports that and exits non-zero -- it never fabricates a per-process listener.
 * Rule 8: the auxiliary-server behaviour is from public VSI OpenVMS TCP/IP
 * Services docs; the TCPIP$SERVICE.DAT text layout is an OVMX design choice,
 * labelled in tcpip_inetd.h.
 *
 * SCOPE (this rung): multiplex the configured services' listeners with poll()
 * over each listener's executive readiness fd, and dispatch one connection at a
 * time per ready listener (the classic inetd wait model). Deferred honestly:
 * the persistent binary service DB (#878), UDP services, the concurrent nowait
 * model, per-service identity, and ACLs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include "vms/pcb.h"
#include "tcpip_inetd.h"

/* Default service DB path when none is given on the command line. The running
 * OVMX system resolves SYS$SYSTEM: through the Files-11 ACP; this literal is the
 * rootfs staging path so the image is runnable in a plain build/test shell too. */
#define DEFAULT_SERVICE_DB \
    "/vms/SYS0/SYSCOMMON/SYSEXE/TCPIP$SERVICE.DAT"

static volatile sig_atomic_t g_stop = 0;

static void on_term(int sig) { (void)sig; g_stop = 1; }
static void on_child(int sig) { (void)sig; /* reaped in the loop */ }

/* Read the whole service DB file into a heap buffer (caller frees). */
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t got;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)sz, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char *argv[])
{
    const char *db_path = (argc > 1) ? argv[1] : DEFAULT_SERVICE_DB;
    char *db_text;
    struct tcpip_service svcs[TCPIP_INETD_MAX_SERVICES];
    int listen_h[TCPIP_INETD_MAX_SERVICES];
    struct pollfd pfd[TCPIP_INETD_MAX_SERVICES];
    int nsvc, i, nbound = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGCHLD, on_child);

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        fprintf(stderr, "%%TCPIP-F-NOPCB, cannot initialise process control block\n");
        return 1;
    }

    db_text = slurp(db_path);
    if (!db_text) {
        fprintf(stderr, "%%TCPIP-F-NOSERVICEDB, cannot read service database %s: %s\n",
                db_path, strerror(errno));
        return 1;
    }

    nsvc = tcpip_inetd_parse_db(db_text, svcs, TCPIP_INETD_MAX_SERVICES);
    free(db_text);
    if (nsvc <= 0) {
        fprintf(stderr, "%%TCPIP-W-NOSERVICES, no enabled services in %s\n", db_path);
        return 1;
    }

    /* Bind + listen each enabled service over the executive BGn: seam. */
    for (i = 0; i < nsvc; i++) {
        listen_h[i] = tcpip_inetd_listen(&svcs[i]);
        if (listen_h[i] < 0) {
            if (errno == ENODEV) {
                fprintf(stderr, "%%TCPIP-F-NOSUCHDEV, no executive (/dev/vms) -- cannot bind %s port %u\n",
                        svcs[i].name, (unsigned)svcs[i].port);
                return 1;               /* honest failure, Rule 9 */
            }
            fprintf(stderr, "%%TCPIP-W-BINDFAIL, service %s port %u: %s\n",
                    svcs[i].name, (unsigned)svcs[i].port, strerror(errno));
            pfd[i].fd = -1;
            continue;
        }
        pfd[i].fd = ovmx_readyfd(listen_h[i]);  /* executive readiness fd for poll() */
        pfd[i].events = POLLIN;
        printf("%%TCPIP-I-BOUND, service %s listening on port %u (image %s)\n",
               svcs[i].name, (unsigned)svcs[i].port, svcs[i].image);
        nbound++;
    }
    if (nbound == 0) {
        fprintf(stderr, "%%TCPIP-F-NOBOUND, no service could be bound\n");
        return 1;
    }

    /* The auxiliary-server accept loop: poll the listeners, dispatch the ready
     * ones to their configured service image, reap exited services. */
    while (!g_stop) {
        int r, st;
        pid_t w;

        r = poll(pfd, (nfds_t)nsvc, 1000);
        if (r < 0) {
            if (errno == EINTR) { /* reap and re-poll */ }
            else break;
        }
        for (i = 0; i < nsvc && r > 0; i++) {
            if (pfd[i].fd < 0 || !(pfd[i].revents & POLLIN))
                continue;
            (void)tcpip_inetd_accept_dispatch(listen_h[i], &svcs[i], NULL);
        }
        /* Reap any finished service images (non-blocking). */
        while ((w = waitpid(-1, &st, WNOHANG)) > 0)
            ;
    }

    for (i = 0; i < nsvc; i++)
        if (pfd[i].fd >= 0)
            (void)ovmx_socket_close(listen_h[i]);
    return 0;
}
