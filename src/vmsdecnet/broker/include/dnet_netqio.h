/*
 * dnet_netqio.h -- one logical-link $QIOW on a _NET: channel, as a VMS
 * program issues it (rd vms-dda / vms-d01).
 *
 * $QIOW's own return value is the status of QUEUING the request; how the I/O
 * ENDED is in the IOSB. On a _NET: channel the ending is the answer NETACP
 * gave: SS$_INVLOGIN for a connect the remote refused, SS$_ENDOFFILE for an
 * IO$M_NOW read with no message buffered, SS$_FILNOTACC / SS$_ABORT for a link
 * that does not exist or has gone. A client that reads only the service status
 * takes every one of those for a success -- a bad-password COPY "opens", then
 * polls a link that never existed for ever (the booted refused-OPEN hang).
 *
 * Every DECnet client image (DECNETD's COPY / SET HOST over NETACP) issues its
 * _NET: $QIOWs through this, and test_syssvc_net_qio_status proves it on the
 * real executive.
 */
#ifndef DNET_NETQIO_H
#define DNET_NETQIO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "starlet.h"
#include "iosbdef.h"

/* Returns how the I/O ended (the IOSB status) when the request was queued, or
 * the refusal when it was not; *xfer is the IOSB byte count on success. */
static inline uint32_t dnet_net_qiow(uint16_t chan, uint32_t func, void *p1, uint32_t p2,
                                     size_t *xfer)
{
    struct _iosb iosb;
    memset(&iosb, 0, sizeof iosb);
    uint32_t st = sys$qiow(0, chan, func, &iosb, NULL, 0, p1, p2, 0, 0, 0, 0);
    if (st & 1)
        st = iosb.iosb$w_status;
    if (xfer)
        *xfer = (st & 1) ? iosb.iosb$w_bcnt : 0;
    return st;
}

#endif /* DNET_NETQIO_H */
