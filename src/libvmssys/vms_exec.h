/*
 * vms_exec.h - Executive privilege / access-mode gateway
 *
 * OVMX privilege state is owned by the EXECUTIVE (the vms.ko kernel module,
 * reached through /dev/vms), not by any per-process userspace variable. This
 * header is the single userspace door to it: $SETPRV, privilege checks and
 * access-mode transitions all go through here, so there is exactly one
 * translation unit that speaks the privilege ioctls.
 *
 * NO SILENT FALLBACK (CLAUDE.md rule 9). If /dev/vms is absent there is no
 * executive, so there is no authoritative privilege state: every entry point
 * returns SS$_NOSUCHDEV and grants nothing. A caller that cannot reach the
 * executive is UNPRIVILEGED, never "privileged because it said so".
 *
 * Status values are the public ssdef.h SS$_ constants. They are spelled as
 * literals here because libvmssys is freestanding and cannot include the
 * libvms headers; see the PROVENANCE comment in vms_exec.c.
 */

#ifndef _VMS_EXEC_H
#define _VMS_EXEC_H

#include <stdint.h>

/* Public SS$_ values used by this interface (see vms_exec.c PROVENANCE). */
#define VMS_EXEC_SS_NORMAL      1
#define VMS_EXEC_SS_NOPRIV      36
#define VMS_EXEC_SS_BADPARAM    20
#define VMS_EXEC_SS_NOTALLPRIV  1664
#define VMS_EXEC_SS_NOSUCHDEV   2312

/*
 * Attach this thread to the executive and establish its privilege set.
 *
 * `request` is a REQUEST, not an assertion: the executive decides what is
 * actually granted (it clamps to what the caller's credentials authorize).
 * `granted` receives the mask the executive actually holds for this process,
 * which is the only mask a caller may believe.
 *
 * The first attach in a process fixes the process-wide granted ceiling;
 * later threads attach with that ceiling rather than a fresh request, so a
 * thread cannot ask the executive for more than the process was granted.
 *
 * Returns SS$_NORMAL, or SS$_NOSUCHDEV when there is no executive.
 */
uint32_t vms_exec_attach(uint32_t vms_pid, uint64_t request, uint64_t *granted);

/* Is this thread attached to a live executive? 1 = yes, 0 = no. */
int vms_exec_attached(void);

/*
 * $SETPRV against the executive. `prev` receives the previous CURRENT mask.
 * Returns SS$_NORMAL, SS$_NOTALLPRIV (only the authorized subset was
 * enabled), or SS$_NOSUCHDEV.
 */
uint32_t vms_exec_setprv(uint64_t mask, int enable, int permanent,
                         uint64_t *prev);

/*
 * Ask the executive whether ALL of `mask` is currently held.
 * Returns SS$_NORMAL if held, SS$_NOPRIV if not, SS$_NOSUCHDEV if there is
 * no executive (i.e. absence denies -- it never grants).
 */
uint32_t vms_exec_chkpriv(uint64_t mask);

/*
 * Read the executive's view of this process: current privileges, permanent
 * (authorized) privileges, and current access mode. Any pointer may be NULL.
 * On SS$_NOSUCHDEV the outputs are zeroed (no privileges, kernel mode is
 * never reported for an unreachable executive).
 */
uint32_t vms_exec_getprv(uint64_t *cur, uint64_t *perm, uint8_t *mode);

/* Access-mode transition through the executive ($SETMOD equivalent). */
uint32_t vms_exec_setmode(uint8_t mode);

#endif /* _VMS_EXEC_H */
