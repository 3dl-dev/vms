/*
 * vms_systime.h - the system clock as the substrate kernel keeps it (vms-593).
 */
#ifndef VMS_SYSTIME_H
#define VMS_SYSTIME_H

#include <stdint.h>

/* vms_substrate_systime -- the system time as a VMS quadword (100 ns ticks since
 * 17-NOV-1858), the one clock SYS$GETTIM/SYS$GETUTC report (vms-593). Read
 * from the substrate kernel's realtime clock through this layer's own kernel
 * call -- never through the C RTL, whose clock_gettime()/time() are themselves
 * served by $GETUTC (src/vmsrms/crtl_time.c). The executive keeps no
 * system-time cell yet (rd vms-642); when it does, this is the one place that
 * changes. SS$_NORMAL, or SS$_ABORT when the substrate clock cannot be read. */
uint32_t vms_substrate_systime(uint64_t *vmstime);

#endif /* VMS_SYSTIME_H */
