/*
 * tcpip_qio.h -- one $QIOW on a BGn: channel, as a VMS program issues it.
 *
 * $QIOW's own return value is the status of QUEUING the request; how the I/O
 * ENDED (a refused connect, a reset, a short read) is in the IOSB (rd vms-d01,
 * as OpenVMS answers it). A client that reads only the service status takes a
 * refused connect for an open connection and a failed read for end of file.
 * Every TCP/IP Services client in this tree issues its $QIOWs through this.
 */
#ifndef TCPIP_QIO_H
#define TCPIP_QIO_H

#include <stdint.h>

#include "starlet.h"
#include "iosbdef.h"

/* The IOSB status when the request was queued, the refusal when it was not. */
static inline uint32_t tcpip_qiow(uint16_t chan, uint32_t func, struct _iosb *iosb,
                                  void *p1, uint32_t p2, uint32_t p3)
{
    uint32_t st = sys$qiow(0, chan, func, iosb, NULL, 0, p1, p2, p3, 0, 0, 0);
    if (st & 1)
        st = iosb->iosb$w_status;
    return st;
}

#endif /* TCPIP_QIO_H */
