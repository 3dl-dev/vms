/*
 * vms_systime.h - the system time, which the executive keeps (rd vms-642).
 */
#ifndef VMS_SYSTIME_H
#define VMS_SYSTIME_H

#include <stdint.h>

/* vms_systime_get -- the system time as a VMS quadword (100 ns ticks since
 * 17-NOV-1858): the executive's system-time cell (src/kernel-core/
 * vms_systime.c), the one clock SYS$GETTIM/SYS$GETUTC report and $SETIME
 * changes for every process. Never the C RTL's clock_gettime()/time(), which
 * are themselves $GETUTC (src/vmsrms/crtl_time.c). SS$_NORMAL; SS$_NOSUCHDEV
 * with no executive -- there is no substrate-clock fallback. */
uint32_t vms_systime_get(uint64_t *vmstime);

/* vms_systime_set -- $SETIME: make *vmstime the system time, or with NULL
 * recalibrate it from the time-of-year clock (the substrate's realtime clock).
 * The executive requires OPER and LOG_IO (SS$_NOPRIV) and refuses a delta time
 * (SS$_IVTIME); the substrate's own clock is never changed. SS$_NOSUCHDEV with
 * no executive. */
uint32_t vms_systime_set(const uint64_t *vmstime);

#endif /* VMS_SYSTIME_H */
