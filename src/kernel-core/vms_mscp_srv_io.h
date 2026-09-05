/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_srv_io.h - the MSCP disk server's SERVED-I/O WORKER half (FC-P6.6;
 * design docs/design-faithful-cluster-executive.md §3.2.6, the E42 corollary).
 *
 * ------------------------------------------------------------------------
 * WHY THIS IS A TRANSLATION UNIT OF ITS OWN
 * ------------------------------------------------------------------------
 * exec_kbackend.h §8's block ops are synchronous and "MAY SLEEP; call only from
 * process context with no exec_lock held". The MSCP server has to make them --
 * a served READ is a real read of a real volume -- and FC-P6.3 made them from
 * the fork work handler, i.e. on the CLUSTER FORK THREAD: the one context that
 * also carries the HELLO cadence, the VC retransmit ladder and every barrier
 * step. Design §3.2.6 rules that out in one line:
 *
 *      "the cluster fork thread never calls exec_blockdev_*"
 *
 * A rule stated in a comment is a rule that comes back. So the calls live in
 * THIS FILE AND NOWHERE ELSE in the cluster stack, and
 * tools/ci/cluster_core_includes_gate.sh RULE 5 enforces exactly that as a
 * file-scoped fact: the symbol `exec_blockdev_` may appear in
 * vms_mscp_srv_io.c and in no other kernel-core cluster file. A reviewer does
 * not have to trace a call graph to know the fork thread is clean; the fork
 * context's translation units cannot even name the symbol.
 *
 * ------------------------------------------------------------------------
 * WHAT RUNS HERE, AND IN WHICH CONTEXT
 * ------------------------------------------------------------------------
 * ONE function, vms_mscp_srv_io_handler(), registered with the fork module's
 * served-I/O worker (cf_set_io_handler, vms_cluster_fork.h §7a) by
 * vms_mscp_srv_start(). It runs on the WORKER THREAD -- process context, no
 * queue lock, no fork mutex, no protocol state -- and may sleep for as long as
 * the disk takes. Its answer travels back to the fork thread as an ordinary
 * CF_WORK_IO_DONE work item, and the MSCP end message is built there.
 *
 * It decides NOTHING about MSCP: the unit, the LBN, the block count and the
 * buffer all arrive in the copied struct cf_io, composed by the pure server FSM
 * from the host's own command and the executive's own device table. All this
 * file adds is the loop over 512-byte blocks and the honest 0 / non-zero the
 * block layer actually returned (INV-6).
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header.
 */
#ifndef OVMX_VMS_MSCP_SRV_IO_H
#define OVMX_VMS_MSCP_SRV_IO_H

#include "vms_cluster_fork.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The two operations the MSCP server submits, carried in cf_io.op. They are the
 * server FSM's own enum mscp_srv_io_op values; the _Static_asserts in
 * vms_mscp_srv_io.c hold the two spellings to one set, so the worker cannot
 * drift from the submitter.
 */
#define VMS_MSCP_SRV_IO_OP_READ  0u
#define VMS_MSCP_SRV_IO_OP_WRITE 1u

/*
 * The status this worker returns for a request it could not even attempt --
 * a malformed request, or a block count that would run past the staging slot
 * the submitter promised. Non-zero, like any real block-layer failure, so the
 * server answers a REAL error rather than a success it cannot back up. Distinct
 * from 1 only so a diagnostic can tell "the worker refused" from "the device
 * failed"; the server treats every non-zero the same.
 */
#define VMS_MSCP_SRV_IO_BADREQ 0x80000001u

/*
 * The cf_io_handler_t the MSCP server registers. WORKER CONTEXT ONLY: it
 * blocks. Returns 0 iff every block moved, non-zero otherwise -- the block
 * layer's own answer, never a default.
 */
uint32_t vms_mscp_srv_io_handler(void *ctx, const struct cf_io *io);

/*
 * How the server packs a request into a struct cf_io, in ONE place so the
 * submitter and the worker cannot disagree about which arg is which. `major`
 * and `minor` are the backing block device the executive's own mounted-volume
 * table holds; `buf` is the submitting HRB's exclusive staging slot, which the
 * worker owns until its completion is posted.
 */
void vms_mscp_srv_io_pack(struct cf_io *out, uint16_t op, uint32_t tag,
			  uint32_t major, uint32_t minor, uint32_t lbn,
			  uint32_t nblocks, uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_MSCP_SRV_IO_H */
