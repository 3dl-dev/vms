/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bg_core.h - the BGn: core<->Linux-rind boundary (vms-9951).
 *
 * The BGn: facility (vms_bg.c) is substrate-agnostic and lives in the executive
 * core; the readiness poll fd (VMS_IOCTL_BG_POLLFD) is pure Linux fd machinery
 * (anon_inode + ->poll, no NetBSD analogue -- NetBSD is kqueue) and stays a
 * Linux rind in src/kernel/vms_bg_pollfd.c. This one declaration is the only
 * surface the rind needs from the core: it must obtain a REFERENCED host socket
 * for a channel without reaching into vms_bg.c's private channel list.
 */
#ifndef _VMS_BG_CORE_H
#define _VMS_BG_CORE_H

#include "exec_kbackend.h"   /* exec_socket_t (the opaque, refcounted host socket) */

struct vms_proc;

/*
 * Look up proc's BG channel `chan`, take a reference on its socket holder, and
 * return it. Returns NULL if there is no such channel or no socket yet
 * (IO$_SETMODE never issued). On a non-NULL return the CALLER owns the reference
 * and must release it with exec_socket_release() -- for the poll fd, that is the
 * fd's ->release. Takes proc->chan_lock internally.
 */
exec_socket_t vms_bg_ref_socket(struct vms_proc *proc, uint32_t chan);

struct list_head;

/*
 * BG channel inheritance boundary (vms-0cd, rd vms-2a5). vms_bg_inherit captures a
 * creator PCB's BG channels while the creator holds the child it is making
 * (vms_proc_create_for), and adopts them onto the child's new PCB. The channel
 * struct is private to vms_bg.c, so these three work through an opaque
 * caller-owned list:
 *
 *   capture: kref'd snapshot of `parent`'s channels onto `out` (must start empty);
 *            0 on success (or empty), -ENOMEM on OOM (out left empty). Caller holds
 *            whatever keeps `parent` alive (vms_proc_hash_lock). GFP_ATOMIC.
 *   adopt:   splice a captured list onto `child`'s table (consumes the list).
 *   drop:    free a captured list without adopting, releasing each socket ref.
 */
int  vms_bg_capture_channels(struct vms_proc *parent, struct list_head *out,
                             uint32_t *out_next_chan);
void vms_bg_adopt_channels(struct vms_proc *child, struct list_head *captured,
                           uint32_t next_chan);
void vms_bg_drop_captured(struct list_head *captured);

#endif /* _VMS_BG_CORE_H */
