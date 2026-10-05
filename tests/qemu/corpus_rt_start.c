/*
 * corpus_rt_start.c - what the corpus guest does BEFORE main() (vms-44a, R2.3).
 *
 * 1. Run as SYSTEM. The guest has no login: every program runs as the process
 *    STARTUP runs under, SYSTEM [1,4] with the executive's own full privilege mask
 *    (vms_kif_establish_system() asks the executive for exactly that; it refuses
 *    without CAP_SYS_ADMIN, which init.sh's root has). Programs that look their own
 *    user name up in SYSUAF ($GETUAI, $CREPRC) need a name that is in it.
 * 2. The RMS force-bind anchor below.
 *
 * The process control block is NOT made here any more: libvms establishes it from
 * the executive's row on the first PCB-backed service (ovmx_pcb_ctx.h), as VMS gives
 * every image a process context. (This file used to stand in for that.)
 */
#include <string.h>
#include <stdint.h>
#include "vms_kif.h"

/*
 * RMS FORCE-BIND ANCHOR. libvms reaches SYSUAF.DAT/RIGHTSLIST.DAT through RMS
 * WEAKLY (rtl/rms_textfile.c, rtl/sysuaf.c -- libvms must not depend on
 * libvmsrms), and a weak reference does not pull an archive member out of
 * libvmsrms.a: without a strong reference somewhere in the link the reader is
 * silently unlinked and $GETUAI/$CREPRC see "no such user". DCL, LOGINOUT and
 * PROVISION each carry such an anchor (src/vmslink/*_rms_bind.c); this is the
 * corpus image's.
 */
extern unsigned int sys$open(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$close(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$connect(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$get(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$put(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$create(void *, void (*)(void *), void (*)(void *));
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

__attribute__((used, noinline))
unsigned int corpus_rt_rms_bind_never(void)
{
    static volatile int never = 0;      /* zero at run time; keeps the strong refs */
    if (!never)
        return 0;
    return sys$open(0, 0, 0)  | sys$close(0, 0, 0) | sys$connect(0, 0, 0)
         | sys$get(0, 0, 0)   | sys$put(0, 0, 0)   | sys$create(0, 0, 0)
         | ovmx_sysuaf_read_user(0, 0) | ovmx_sysuaf_read_uic(0, 0);
}

__attribute__((constructor))
static void corpus_rt_run_as_system(void)
{
    (void)vms_kif_establish_system();
}
