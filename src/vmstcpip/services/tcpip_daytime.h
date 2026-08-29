/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tcpip_daytime.h - TCP/IP Services for OVMX: the DAYTIME service (RFC 867),
 * the first GENUINE standalone service image the auxiliary server (TCPIP$INETD,
 * vms-cdb9) launches end to end over the executive (rd vms-477).
 *
 * WHAT THIS IS. RFC 867 defines the Daytime Protocol: a TCP server listening on
 * well-known port 13 that, on an inbound connection, sends the current date and
 * time as a single human-readable ASCII line and closes the connection. Any data
 * the client sends is thrown away. It is the classic minimal inetd service -- the
 * one that proves the auxiliary server can bind a well-known port, accept a
 * connection, and hand it to a SEPARATELY-BUILT service image whose reply travels
 * back to the client over the executive-materialized connection fd.
 *
 * WHERE IT RUNS. The shipped image is TCPIP$DAYTIME.EXE (tools/
 * vms_tcpip_daytime.c): a standalone image the auxiliary server spawns with the
 * accepted connection as its SYS$INPUT/SYS$OUTPUT (fd 0/1). The image touches NO
 * executive API of its own -- it is an ordinary program handed a connection on
 * fd 0/1, exactly as an inetd service is, and it just writes its one line to
 * stdout and exits. The whole NETWORK path (bind/listen/accept + the
 * materialized [bgconn] fd) is the auxiliary server's, proven against a real
 * /dev/vms by test_syssvc_tcpip_daytime.c.
 *
 * WHY A HEADER-RESIDENT ENGINE. Same reason tcpip_ping.h / tcpip_inetd.h are:
 * the ONE daytime-string formatter backs the shipped image, and its output SHAPE
 * is what the QEMU proof asserts on the wire. Every function is `static inline`,
 * so a TU using it draws no unused-function diagnostic and adds no compiled
 * object to any native link graph.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The daytime BEHAVIOUR -- send the date and time
 * as a one-line ASCII string on connect, then close -- is from the public IETF
 * standard RFC 867 (no clean-room constraint on an open standard). RFC 867
 * deliberately does NOT mandate a byte-exact syntax ("There is no specific syntax
 * for the daytime"), only that it be a human-readable one-line date/time. The
 * exact field layout OVMX emits (the strftime template below, a ctime-style
 * "Www Mmm dd hh:mm:ss yyyy" line terminated CR LF) is therefore an OVMX DESIGN
 * CHOICE within RFC 867's latitude -- LABELED here, never presented as a
 * VMS-authentic wire format. No VSI/HPE source or binary was read.
 */

#ifndef _OVMX_TCPIP_DAYTIME_H
#define _OVMX_TCPIP_DAYTIME_H

#include <stddef.h>
#include <stdio.h>      /* snprintf -- used by the negctl mutation of the formatter */
#include <time.h>

/* OVMX DESIGN CHOICE (Rule 8): the one-line daytime layout OVMX emits, a
 * ctime-style "Www Mmm dd hh:mm:ss yyyy" line. RFC 867 leaves the syntax open
 * ("a human-readable one-line date/time"), so this template is an OVMX choice
 * within that latitude, not a VMS-authentic format. `%e` (space-padded day of
 * month) keeps the field width fixed. */
#define TCPIP_DAYTIME_RFC867_FMT   "%a %b %e %H:%M:%S %Y"

/* The CR LF line terminator RFC 867's examples use. */
#define TCPIP_DAYTIME_EOL          "\r\n"

/* Longest line the template above can produce, plus the CR LF and a NUL. A
 * fixed-width ctime-style line is ~24 chars; 64 is comfortable headroom. */
#define TCPIP_DAYTIME_MAX          64

/*
 * Format the RFC 867 daytime line for `when` into `out` (`outlen` bytes),
 * NUL-terminated, ending in CR LF. Returns the number of bytes written (not
 * counting the NUL), or 0 if the buffer is too small or the arguments are bad.
 * Uses localtime() so the line is in the server's current time zone, exactly as
 * RFC 867 specifies ("in the current time zone").
 */
static inline size_t tcpip_daytime_format(char *out, size_t outlen, time_t when)
{
    struct tm tmv;
    size_t n;

    if (!out || outlen < 8)
        return 0;

    /* localtime_r keeps the engine reentrant; fall back to localtime() only if
     * the platform lacks it (it does not, on any OVMX target). */
    if (!localtime_r(&when, &tmv))
        return 0;

    n = strftime(out, outlen, TCPIP_DAYTIME_RFC867_FMT, &tmv); /* NEGCTL tcpip-daytime-reply-not-formatted */
    if (n == 0)
        return 0;

    /* Append CR LF if it fits; RFC 867's examples terminate the line that way. */
    if (n + 2 < outlen) {
        out[n++] = '\r';
        out[n++] = '\n';
        out[n] = '\0';
    }
    return n;
}

#endif /* _OVMX_TCPIP_DAYTIME_H */
