#ifndef OVMX_PCB_CTX_H
#define OVMX_PCB_CTX_H
/*
 * ovmx_pcb_ctx.h - the calling process's PCB, established on first use (libvms
 * internal; include AFTER vms/pcb.h).
 *
 * On VMS every image runs in a process that already has its control block; the
 * services find the caller's channels, exit handlers, quotas and privileges
 * there. OVMX keeps that state in a per-process struct vms_pcb that only DCL,
 * LOGINOUT and a $CREPRC child created explicitly -- so an arbitrary activated
 * image's first PCB-backed service ($ASSIGN, $CREMBX, $DCLEXH, ...) used to answer
 * SS$_BADPARAM. Inside libvms, vms_pcb_get() therefore means vms$$pcb_for_service():
 * the PCB if the process has one, else one SEEDED FROM THE EXECUTIVE'S ROW for this
 * process (privileges, pid, UIC, user name, process name -- never a self-declared
 * identity), the same seeding DCL performs. No executive, or a thread other than
 * the process's main thread: NULL, exactly as before.
 */
struct vms_pcb;
struct vms_pcb *vms$$pcb_for_service(void);
#define vms_pcb_get() vms$$pcb_for_service()
#endif
