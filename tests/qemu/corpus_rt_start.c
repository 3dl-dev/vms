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

__attribute__((constructor))
static void corpus_rt_image_context(void)
{
    if (vms_pcb_get())
        return;
    struct vms_procinfo self;
    memset(&self, 0, sizeof self);
    if (!(vms_kif_getjpi_self(&self) & 1))
        return;                 /* no executive row: no context, services refuse honestly */
    struct vms_pcb *pcb = vms_pcb_init(self.cur_privs);
    if (pcb)
        vms_pcb_set_identity(self.vms_pid, self.uic, self.username, self.prcnam);
}
