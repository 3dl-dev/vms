/*
 * corpus_rt_start.c - image start-up context for the corpus runtime column
 * (vms-44a, R2.3).
 *
 * A VMS image always runs inside a process that has a control block: the system
 * services it calls ($ASSIGN, $CREMBX, $DCLEXH, $GETUAI, ...) find the caller's
 * channels, exit handlers, quotas and privileges there. OVMX keeps that state in
 * a per-process PCB (src/vmsprocess/vms_pcb.c) which today only DCL and a
 * $CREPRC child establish -- nothing establishes it for an arbitrary activated
 * image, so such an image's first PCB-backed service answers SS$_BADPARAM.
 *
 * That gap is the PRODUCT's (rd vms-44a/vms-fe3d: the image start-up path --
 * IMGACT / the C RTL's decc$main -- must do what this does); until it lands,
 * this object stands in for it in the corpus guest, and only here. It copies
 * what the EXECUTIVE holds for this process (privileges, pid, UIC, user name,
 * process name) into the PCB -- the same seeding dcl_main.c performs -- and never
 * declares an identity of its own.
 *
 * Linked into every corpus_rt_* binary and run before main().
 */
#include <string.h>
#include <stdint.h>
#include "vms/pcb.h"
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
static void corpus_rt_image_context(void)
{
    if (vms_pcb_get())
        return;
    /* The corpus guest has no login: every program runs as the process STARTUP
     * runs under, SYSTEM [1,4] with the executive's own full privilege mask --
     * vms_kif_establish_system() asks the executive for exactly that (it refuses
     * without CAP_SYS_ADMIN, which init.sh's root has). Programs that look their
     * own user name up in SYSUAF ($GETUAI, $CREPRC) need a name that is in it. */
    (void)vms_kif_establish_system();
    struct vms_procinfo self;
    memset(&self, 0, sizeof self);
    if (!(vms_kif_getjpi_self(&self) & 1))
        return;                 /* no executive row: no context, services refuse honestly */
    struct vms_pcb *pcb = vms_pcb_init(self.cur_privs);
    if (pcb)
        vms_pcb_set_identity(self.vms_pid, self.uic, self.username, self.prcnam);
}
