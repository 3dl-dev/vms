// SPDX-License-Identifier: GPL-2.0
/*
 * vms_mscp_srv_io.c - the MSCP disk server's SERVED-I/O WORKER half (FC-P6.6).
 *
 * READ vms_mscp_srv_io.h FIRST: it carries the contract, the execution context
 * and -- the point of the whole file -- why the executive's block seam is
 * reachable from HERE and from nowhere else in the cluster stack.
 *
 * THE ONE RULE THIS FILE EXISTS TO KEEP (design §3.2.6):
 *
 *      the cluster fork thread never calls exec_blockdev_*
 *
 * Everything below runs on the SERVED-I/O WORKER THREAD -- process context, no
 * queue lock, no fork mutex, no protocol state -- which is exactly the context
 * exec_kbackend.h §8 requires ("MAY SLEEP; call only from process context with
 * no exec_lock held") and exactly the context the fork thread is not.
 *
 * IT DECIDES NOTHING ABOUT MSCP. The unit's backing device, the LBN, the block
 * count and the buffer all arrive in the copied struct cf_io, composed by the
 * pure server FSM from the host's own command and by vms_mscp_srv.c from the
 * executive's own mounted-volume table. What goes back is 0 or the honest
 * non-zero the block layer returned -- never a defaulted success (INV-6).
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header.
 */

#include "vms_internal.h"      /* the host's fixed-width types                */
#include "exec_kbackend.h"     /* §8: exec_blockdev_read_block / _write_block */
#include "vms_cluster_fork.h"  /* FC-P0.5/P6.6: struct cf_io, cf_io_handler_t */
#include "vms_mscp_srv_fsm.h"  /* MSCP_SRV_BLOCK_SIZE, enum mscp_srv_io_op    */
#include "vms_mscp_srv.h"      /* MSCP_SRV_XFER_BLOCKS -- the slot's ceiling  */
#include "vms_mscp_srv_io.h"

/*
 * ONE SET OF OPERATION CODES, TWO SPELLINGS, ASSERTED. The submitter names them
 * enum mscp_srv_io_op (a pure TU that must not include this header); the worker
 * names them VMS_MSCP_SRV_IO_OP_*. They are one set or the server would write
 * where it meant to read -- so the compiler, not a reader's memory, keeps them
 * together.
 */
_Static_assert((unsigned)MSCP_SRV_IO_READ == VMS_MSCP_SRV_IO_OP_READ,
	       "the worker's READ opcode must be the server FSM's");
_Static_assert((unsigned)MSCP_SRV_IO_WRITE == VMS_MSCP_SRV_IO_OP_WRITE,
	       "the worker's WRITE opcode must be the server FSM's");

void vms_mscp_srv_io_pack(struct cf_io *out, uint16_t op, uint32_t tag,
			  uint32_t major, uint32_t minor, uint32_t lbn,
			  uint32_t nblocks, uint8_t *buf)
{
	if (out == NULL)
		return;
	out->owner = (uint16_t)CF_OWNER_MSCP;
	out->op    = op;
	out->tag   = tag;
	out->arg0  = major;
	out->arg1  = minor;
	out->arg2  = lbn;
	out->arg3  = nblocks;
	out->ptr   = buf;
}

/*
 * Is this a request the worker can actually attempt? A malformed one is refused
 * rather than half-run: `nblocks` past MSCP_SRV_XFER_BLOCKS would run off the
 * end of the staging slot the submitter promised, and a NULL buffer has nowhere
 * to put the bytes. Both are impossible from the shipping submitter (the FSM's
 * own P.BCNT gate gets there first) -- which is why they are answered as a
 * refusal here and not left to be discovered by a memory overwrite.
 */
static int srvio_request_ok(const struct cf_io *io)
{
	return io != NULL && io->ptr != NULL && io->arg3 != 0u &&
	       io->arg3 <= MSCP_SRV_XFER_BLOCKS &&
	       (io->op == VMS_MSCP_SRV_IO_OP_READ ||
		io->op == VMS_MSCP_SRV_IO_OP_WRITE);
}

/* THE BLOCKING READ. One 512-byte block at a time, which is the seam's own
 * unit (§8); the first failure stops the loop, because a partially filled
 * buffer is not the data the host asked for. */
static uint32_t srvio_read(const struct cf_io *io)
{
	uint8_t *buf = (uint8_t *)io->ptr;
	uint32_t i;

	for (i = 0; i < io->arg3; i++) {
		if (exec_blockdev_read_block(io->arg0, io->arg1,
					     (uint64_t)io->arg2 + (uint64_t)i,
					     buf + (i * MSCP_SRV_BLOCK_SIZE),
					     MSCP_SRV_BLOCK_SIZE) != 0)
			return (uint32_t)(i + 1u);   /* the block that failed */
	}
	return 0u;
}

/* THE BLOCKING WRITE, the same shape. */
static uint32_t srvio_write(const struct cf_io *io)
{
	const uint8_t *buf = (const uint8_t *)io->ptr;
	uint32_t i;

	for (i = 0; i < io->arg3; i++) {
		if (exec_blockdev_write_block(io->arg0, io->arg1,
					      (uint64_t)io->arg2 + (uint64_t)i,
					      buf + (i * MSCP_SRV_BLOCK_SIZE),
					      MSCP_SRV_BLOCK_SIZE) != 0)
			return (uint32_t)(i + 1u);
	}
	return 0u;
}

uint32_t vms_mscp_srv_io_handler(void *ctx, const struct cf_io *io)
{
	(void)ctx;   /* the request carries everything; no server state is read
		      * here, which is what makes this context safe */

	if (!srvio_request_ok(io))
		return VMS_MSCP_SRV_IO_BADREQ;
	return (io->op == VMS_MSCP_SRV_IO_OP_READ) ? srvio_read(io)
						   : srvio_write(io);
}
