/*
 * vms_systime.c - the system time, which the executive keeps (rd vms-642).
 *
 * SYS$GETTIM/SYS$GETUTC/SYS$SETIME (src/libvms/syssvc/sys_time.c) and through
 * them DECC$SHR's clock (src/vmsrms/crtl_time.c, vms-593) reach the system
 * time here: the executive's cell (src/kernel-core/vms_systime.c, the
 * EXE$GQ_SYSTIME of VMS), so a $SETIME in one process is the time every other
 * process reads. No executive -> SS$_NOSUCHDEV, never the substrate clock.
 */

#include "vms_systime.h"
#include "vms_kif.h"
#include "vms_string.h"
#include "vms_errno.h"

uint32_t vms_systime_get(uint64_t *vmstime)
{
    struct vms_systime_args a;
    uint32_t st;

    if (!vmstime)
        return SS$_BADPARAM;
    vms_memset(&a, 0, sizeof a);
    a.op = VMS_SYSTIME_GET;
    st = vms_kif_systime(&a);
    if (st & 1)
        *vmstime = a.time;
    return st;
}

uint32_t vms_systime_set(const uint64_t *vmstime)
{
    struct vms_systime_args a;

    vms_memset(&a, 0, sizeof a);
    a.op = vmstime ? VMS_SYSTIME_SET : VMS_SYSTIME_RECAL;
    a.time = vmstime ? *vmstime : 0;
    return vms_kif_systime(&a);
}
