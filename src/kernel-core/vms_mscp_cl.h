/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl.h - the MSCP DISK CLASS DRIVER's EXECUTIVE GLUE (plan item
 * FC-P7.1).
 *
 * vms_mscp_cl_io_fsm.h is the driver itself: the CDDB/UCB/CDRP model, the
 * `handlers[state][event]` table, the device-naming rule and the request
 * deadline, all pure. THIS is the half that gives it a world, and it is the
 * exact counterpart of what vms_mscp_srv.c is for the server:
 *
 *   - it OWNS the object (`struct vms_mscp_cl`, private to vms_mscp_cl.c);
 *   - it BINDS the driver DOWNWARD to the executive: `send_cmd` to
 *     scs_send_msg, `buf_register`/`buf_release` to the port's THIRD service
 *     (pe_buf_register / pe_buf_release), `unit_ready`/`unit_gone` to
 *     vms_devtab's served-disk rows, and the clock to the seam;
 *   - it BINDS the driver UPWARD from SCS: the `VMS$DISK_CL_DRVR` connections
 *     (through CNXMAN's ONE registration of that name, vms_cnxman.h SS5b) and
 *     the block-transfer consumer registration (vms_scs.h SS9);
 *   - it runs the driver's beat on FC-P0.5's cf_timer_* under
 *     CF_OWNER_MSCP_CL -- never a raw substrate timer, and never protocol at
 *     timer level.
 *
 * ------------------------------------------------------------------------
 * WHO IT CONNECTS TO, AND WHY A REFUSAL IS NOT AN ERROR
 * ------------------------------------------------------------------------
 * On its beat this layer sweeps the systems SCS has an OPEN circuit to
 * (vms_scs_peer_at, vms_scs.h SS8) and opens one `VMS$DISK_CL_DRVR` ->
 * `MSCP$DISK` connection to each one it does not already hold. A member that
 * serves no disks has no `MSCP$DISK` SYSAP registered and REJECTS that connect
 * -- which is a real, common configuration (MSCP_LOAD=0, or simply nothing
 * mounted), not a failure. It is counted (`connect_refusals`) and retried no
 * sooner than MSCP_CL_RETRY_MS, so a node that serves nothing is asked again
 * occasionally rather than hammered, and a node that starts serving later is
 * still found.
 *
 * ------------------------------------------------------------------------
 * THE SERVED DEVICE ROW
 * ------------------------------------------------------------------------
 * A unit the discovery walk found becomes a real `vms_devtab` row with
 * `mscp_served` set -- the executive state DVI$_MSCP_SERVED projects -- and NO
 * local backing device, because the bytes are on another node. When the
 * connection to the serving member closes, the row is REMOVED: a device that
 * cannot be reached is not a device this executive will keep advertising.
 *
 * ------------------------------------------------------------------------
 * WHAT THIS LAYER DOES NOT DO, AND WHY IT IS SAID HERE
 * ------------------------------------------------------------------------
 * It does NOT plug served units into `exec_blockdev_read_block` /
 * `_write_block`. That seam is SYNCHRONOUS by contract (exec_kbackend.h SS8),
 * and an MSCP transfer completes asynchronously on a peer's end message; a
 * synchronous wrapper would have to block the cluster fork thread that also
 * has to run the completion, which deadlocks by construction. So this layer
 * exposes the driver's own ASYNCHRONOUS service (SS3 below) and the ACP bridge
 * is left to the item that owns the ACP's own waiting discipline (FC-P7.2).
 * Nothing here pretends to a synchronous read.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_MSCP_CL_H
#define OVMX_VMS_MSCP_CL_H

#include "vms_cluster.h"

struct vms_mscp_cl;

/* ==========================================================================
 * 1. Lifecycle
 *
 * Called from VMS_IOCTL_CLUSTER_START's chain, AFTER vms_cnxman_start(): the
 * class driver needs CNXMAN's `VMS$DISK_CL_DRVR` registration to connect under
 * and the port to move blocks over.
 *
 * SS$_NORMAL when the driver is running. A node with no member serving disks
 * simply finds nothing -- that is not a failure and must not fail a boot. A
 * real failure (no fork, no port, no SCS, no connection manager, no memory)
 * returns its own SS$_ status (Rule 9: no layer beneath, no service).
 * ========================================================================== */
int  vms_mscp_cl_start(struct vms_cluster *cl);
void vms_mscp_cl_stop(struct vms_cluster *cl);

/* ==========================================================================
 * 2. Readback (INV-6: a projection of real objects, never a composed answer)
 * ========================================================================== */

/* How many served units this node currently holds devices for, and how many
 * serving controllers it is connected to. Both read off the live driver; 0/0
 * when none is running. */
void vms_mscp_cl_status(struct vms_cluster *cl, uint32_t *out_units,
			uint32_t *out_controllers);

/* ==========================================================================
 * 3. The asynchronous block service (see the file header's last section)
 *
 * `devnam` is a served unit's own device name as this driver entered it in
 * vms_devtab. `buf` is the CALLER's memory and must stay valid until the
 * completion runs: the port names it for the peer and the bytes land in it
 * directly, with no copy.
 *
 * Returns SS$_NORMAL when the request was really taken (a command went out, or
 * it is queued behind a real ONLINE in flight) -- and then `done` WILL be
 * called exactly once, with the server's own MSCP status or a real Command
 * Aborted from the deadline. Any other status is a synchronous refusal and
 * `done` is NOT called.
 * ========================================================================== */
typedef void (*vms_mscp_cl_done_cb)(void *ctx, uint32_t handle,
				    uint16_t mscp_status, uint32_t bytes);

int vms_mscp_cl_read(struct vms_cluster *cl, const char *devnam, uint32_t lbn,
		     uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		     uint32_t handle, vms_mscp_cl_done_cb done, void *done_ctx);
int vms_mscp_cl_write(struct vms_cluster *cl, const char *devnam, uint32_t lbn,
		      uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		      uint32_t handle, vms_mscp_cl_done_cb done,
		      void *done_ctx);

#endif /* OVMX_VMS_MSCP_CL_H */
