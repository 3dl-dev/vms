/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_tcpip_daytime.c - TCPIP$DAYTIME.EXE (rd vms-477): the RFC 867 Daytime
 * service, the first GENUINE standalone service image the TCP/IP auxiliary
 * server (TCPIP$INETD.EXE, vms-cdb9) launches end to end over the executive.
 *
 * WHAT IT DOES. The auxiliary server binds DAYTIME's well-known port over the
 * executive BGn: seam, accepts an inbound connection, materializes it as a real
 * executive-backed fd, and fork()+execv()s THIS image with that fd as its
 * SYS$INPUT/SYS$OUTPUT (fd 0/1). Per RFC 867 this image then writes the current
 * date and time as a single human-readable ASCII line to stdout and exits;
 * closing stdout drives the connection FIN. Any client input is thrown away
 * (RFC 867: "any data received is thrown away"), so this image never reads --
 * it is a pure "write one line on connect and close" server.
 *
 * NO EXECUTIVE API HERE. Unlike TCPIP$INETD.EXE (the auxiliary server), this
 * service image touches no /dev/vms path of its own: it is an ordinary program
 * handed an already-connected fd on 0/1, exactly as a Unix inetd service is. The
 * entire network path -- and its Rule 9 honest-failure when /dev/vms is absent
 * -- lives in the auxiliary server, not here. That is precisely the point of
 * vms-477: proving the auxiliary server hands a real accepted connection to a
 * REAL separately-built shipped image whose reply transits the executive fd.
 *
 * Rule 8: the daytime string is formatted by the shared RFC 867 engine
 * (src/vmstcpip/services/tcpip_daytime.h) -- behaviour from the open IETF
 * standard, the exact one-line layout a LABELED OVMX design choice within RFC
 * 867's open syntax.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "tcpip_daytime.h"

int main(void)
{
    char line[TCPIP_DAYTIME_MAX];
    size_t n, off;

    /* RFC 867: the current date/time as a single human-readable ASCII line. */
    n = tcpip_daytime_format(line, sizeof(line), time(NULL));
    if (n == 0) {
        /* Formatting failed (impossible on a sane clock) -- emit nothing and
         * exit non-zero rather than send a fabricated line. */
        fprintf(stderr, "%%TCPIP-F-DAYTIME, cannot format the RFC 867 daytime line\n");
        return 1;
    }

    /* Write the whole line to SYS$OUTPUT (fd 1 = the accepted connection),
     * handling short writes so the client sees the complete line. */
    off = 0;
    while (off < n) {
        ssize_t w = write(STDOUT_FILENO, line + off, n - off);
        if (w < 0)
            return 1;            /* connection gone -- honest non-zero exit */
        off += (size_t)w;
    }

    return 0;
}
