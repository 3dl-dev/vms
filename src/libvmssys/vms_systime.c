/*
 * vms_systime.c - the system clock as the substrate kernel keeps it (vms-593).
 *
 * SYS$GETTIM/SYS$GETUTC report this clock (src/libvms/syssvc/sys_time.c). It is
 * read with libvmssys's own kernel call, not the C RTL's clock_gettime(),
 * because on OVMX the C RTL's clock is itself SYS$GETUTC (src/vmsrms/
 * crtl_time.c). Not a /dev/vms entry point: the executive keeps no system-time
 * cell yet (rd vms-642), and this is the one place that changes when it does.
 */

#include "vms_systime.h"
#include "vms_syscall.h"
#include "vms_errno.h"

/* see vms_systime.h. 0x007C95674BEB4000 = 17-NOV-1858 ->
 * 1-JAN-1970 in 100 ns ticks. */
uint32_t vms_substrate_systime(uint64_t *vmstime)
{
    struct vms_timespec ts;

    if (!vmstime)
        return SS$_BADPARAM;
    if (vms_sys_clock_gettime(VMS_CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0)
        return SS$_ABORT;
    *vmstime = (uint64_t)ts.tv_sec * 10000000ULL + (uint64_t)ts.tv_nsec / 100u
               + 0x007C95674BEB4000ULL;
    return SS$_NORMAL;
}
