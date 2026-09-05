/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_srv.h - the MSCP disk server's EXECUTIVE GLUE (plan item FC-P6.3).
 *
 * vms_mscp_srv_fsm.h is the server itself: the UQB/HQB/HRB model, the
 * controller-state ladder and the answer to every command, all pure. THIS is
 * the half that gives it a world, and it is the exact counterpart of what
 * vms_scs.c is for the SCS FSM and vms_cnxman.c is for the connection manager:
 *
 *   - it OWNS the object (`struct vms_mscp_srv`, private to vms_mscp_srv.c):
 *     the FSM, its injected ops and the one staging buffer transfers pass
 *     through. The pure layer still allocates nothing.
 *   - it BINDS the server DOWNWARD to the executive: `unit_at` to the ODS-2
 *     ACP's mounted-volume table (vms_acp_serve.h), `io_submit` to FC-P0.5's
 *     SERVED-I/O WORKER (cf_io_post -- the block seam itself is reached only
 *     from vms_mscp_srv_io.c, on the worker thread, NEVER from the fork
 *     thread; design §3.2.6 / FC-P6.6), `send_end` to scs_send_msg, and the two
 *     transfer entries to the port's THIRD service (pe_send_block_read_end /
 *     pe_buf_register).
 *   - it BINDS the server UPWARD from SCS: the `MSCP$DISK` SYSAP callbacks,
 *     plus the ONE block-transfer consumer registration (vms_scs.h SS9).
 *   - it runs the server's beat on FC-P0.5's cf_timer_* under CF_OWNER_MSCP --
 *     never a raw substrate timer, and never protocol at timer level.
 *
 * ------------------------------------------------------------------------
 * `MSCP$DISK` IS REGISTERED ONLY WHILE A SERVEABLE UNIT EXISTS
 * ------------------------------------------------------------------------
 * The plan row's own clause, and design P6's "never a listener that
 * black-holes". A registered SYSAP is an ADVERTISEMENT: a member that finds
 * `MSCP$DISK` present and connects has been told this node serves disks. So
 * the registration is not a configuration flag -- it is a function of REAL
 * executive state, re-evaluated on this layer's own beat:
 *
 *     MSCP_LOAD == 0                      -> the server is not started at all
 *     MSCP_SERVE_ALL == 0                 -> loaded, serving nothing
 *     no volume mounted                   -> no UQB, so no registration
 *     the first volume appears            -> LISTEN
 *     the last volume goes away           -> UNLISTEN
 *
 * That is why this file has a beat at all. A node that mounts its system disk
 * after CLUSTER_START (which is the normal boot order) starts serving when the
 * volume really appears, and a node whose last volume is dismounted stops
 * advertising a service it can no longer perform.
 *
 * ------------------------------------------------------------------------
 * MSCP_LOAD / MSCP_SERVE_ALL, AND WHAT THIS FILE DOES *NOT* DECODE (Rule 8)
 * ------------------------------------------------------------------------
 * Both are real SYSGEN parameters this executive already loads
 * (vms_cluster.h SS2, FC-P0.10) and both are read here from
 * `cl->params`, never from a module parameter or a default. What is honoured is
 * the reading this project can defend: MSCP_LOAD selects whether the disk
 * server is loaded, MSCP_SERVE_ALL selects whether it serves the node's
 * volumes, and ZERO means no in both cases. MSCP_SERVE_ALL's finer BIT
 * semantics on OpenVMS (which classes of device a particular non-zero value
 * selects) are NOT reproduced: this tree has no published source for them, and
 * inventing a bit map for an operator-visible parameter would be exactly the
 * guess CLAUDE.md Rule 8 forbids. Any non-zero value therefore means "serve
 * this node's mounted volumes", which is what the lab's own golden
 * configuration (MSCP_LOAD=1 / MSCP_SERVE_ALL=1,
 * docs/design-mscp-direction.md) sets and what a real MOUNT was captured
 * against.
 *
 * ------------------------------------------------------------------------
 * THE TWO IDENTITIES THIS FILE MINTS, AND WHAT THEY ARE MADE OF (INV-6)
 * ------------------------------------------------------------------------
 * AA-L619A-TK makes both opaque and merely requires them to be UNIQUE, so
 * their composition is OVMX's own -- labelled as such, exactly as the SCS
 * Con.ID allocator's is, and made ENTIRELY of values the executive really
 * holds:
 *
 *   P.CNTI (controller identifier, SCC end): this node's SCSSYSTEMID. One
 *          MSCP server per node, one identifier per server. A node with no
 *          SCSSYSTEMID does not start a server at all rather than mint from a
 *          zero.
 *   P.UNTI (unit identifier, GUS/ONLINE end): (SCSSYSTEMID << 16) | unit
 *          number -- unique across the cluster because SCSSYSTEMID is, and
 *          unique within the node because the unit number is. NEVER zero,
 *          which sec 6.12 reserves for "virtually no characteristics are
 *          valid".
 *
 * The unit NUMBER itself is not minted: it is the trailing decimal of the
 * executive's own device name for the volume (DKA0: -> 0, VDA12: -> 12), so
 * OVMX's served `$<ALLOCLASS>$DUAn` names the same unit the node's own
 * SHOW DEVICE does.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_MSCP_SRV_H
#define OVMX_VMS_MSCP_SRV_H

#include "vms_cluster.h"

struct vms_mscp_srv;

/*
 * The PER-REQUEST transfer ceiling, in 512-byte blocks. Lives in the header
 * because two translation units need the SAME number and must not each carry
 * their own: vms_mscp_srv.c sizes the staging buffer from it (one slot per HRB)
 * and vms_mscp_srv_io.c bounds a worker request against it, so a request the
 * worker is asked to run can never be larger than the slot it was promised.
 *
 * AA-L619A-TK bounds a single transfer only by what the host's own buffer
 * descriptor names, so this ceiling is OVMX's own: 8 blocks (4 KiB), which
 * covers the mount-verification sequence (home block, SCB, the INDEXF/BITMAP
 * extents a class driver reads a few blocks at a time) and keeps the whole
 * server one modest allocation on a VAX. A command asking for more is refused
 * with a REAL "Invalid Byte Count" naming P.BCNT, never truncated.
 */
#define MSCP_SRV_XFER_BLOCKS 8u

/* ==========================================================================
 * 1. Lifecycle
 *
 * Called from VMS_IOCTL_CLUSTER_START's chain, AFTER vms_cnxman_start(): the
 * server needs SCS to register a SYSAP with and the port to move blocks over.
 *
 * SS$_NORMAL when the server is running OR when this node has nothing to serve
 * and honestly did not start one -- "no disks to serve" is a legitimate cluster
 * configuration (the published description makes serving a ROLE, not a
 * membership requirement), so it must not fail a boot. A real failure (no SCS,
 * no port, no memory, no SCSSYSTEMID) returns its own SS$_ status.
 * ========================================================================== */
int  vms_mscp_srv_start(struct vms_cluster *cl);
void vms_mscp_srv_stop(struct vms_cluster *cl);

/* ==========================================================================
 * 2. Readback (INV-6: a projection of real objects, never a composed answer)
 * ========================================================================== */

/* How many units this node is serving right now, and whether `MSCP$DISK` is
 * registered. Both read off the live server; 0/0 when none is running. */
void vms_mscp_srv_status(struct vms_cluster *cl, uint32_t *out_units,
			 uint32_t *out_registered);

#endif /* OVMX_VMS_MSCP_SRV_H */
