/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/mscp_write.c - FC-P6.5's rung-R2 leg: TWO SIMULATED NODES COMPLETE
 * AN MSCP WRITE, end to end, over the virtual LAN.
 *
 * ===========================================================================
 * WHAT ACTUALLY CROSSES THE WIRE HERE
 * ===========================================================================
 *
 * Two `sim_node`s -- each the SHIPPING src/kernel-core/vms_pe_fsm.c with the
 * simulator's ops injected -- form their circuits by the real HELLO/START/ACK
 * ladder on the virtual LAN and virtual clock. Then:
 *
 *   OVMXA runs the SHIPPING `struct mscp_cl_fsm`   (the disk class driver)
 *   OVMXB runs the SHIPPING `struct mscp_srv_fsm`  (the MSCP disk server)
 *
 * and every byte between them is carried by the two real ports:
 *
 *   1. the class driver's SCC/GUS/ONLINE/WRITE COMMANDS and the server's END
 *      MESSAGES go out through `pe_vc_send_msg_var` -- the port's own sequenced
 *      message service -- and arrive through `pe_fsm_rx` and the port's own
 *      `pe_upper_ops.message` upcall;
 *   2. the server's REQUEST DATA is `pe_blk_send_request` (FC-P6.5) and
 *      really traverses the LAN;
 *   3. the client's ANSWER is the PORT'S OWN automatic responder inside
 *      `pe_blk_rx_try` -- nothing in this file transmits the WRITE data;
 *   4. those data frames arrive at the server's port, which bounds-checks them
 *      against ITS registration and upcalls `block_data`.
 *
 * So the WRITE choreography design §3.2.6's E41 ruled -- server-sent REQUEST
 * DATA, host port answers automatically -- is exercised by the shipping code at
 * BOTH ends, across a wire that can lose frames.
 *
 * ===========================================================================
 * WHAT THE HARNESS STANDS IN FOR, AND SAID PLAINLY
 * ===========================================================================
 *
 *   SCS. There is no `struct scs_fsm` in the simulator (sim_msg.h says the same
 *   thing for its own stand-in). This file builds the 16-byte abs 56-71
 *   envelope through the SCS CODEC and carries the Con.ID pair as its own test
 *   identity, because no CDT exists here to own a real one -- exactly the
 *   labelled shortcut sim_msg.h already takes. Nothing about the MSCP protocol,
 *   the block transfer or the port is stood in for.
 *
 *   THE SERVED-I/O WORKER. `io_submit` QUEUES the transfer and returns; the
 *   scenario then drains the queue explicitly (`worker_drain`). That is not a
 *   convenience -- it is the point: design §3.2.6 forbids the cluster fork
 *   thread from touching storage, so this scenario ASSERTS that the volume is
 *   still untouched at the instant the data-arrival dispatch returns, and only
 *   the drain writes it.
 *
 *   THE VOLUME. A byte array standing in for `exec_blockdev_*`, as every rung-1
 *   MSCP test uses.
 *
 * ===========================================================================
 * A FINDING THIS SCENARIO MADE -- E48 -- AND ITS RESOLUTION (FC-P2.7)
 * ===========================================================================
 *
 * This was the first test in the tree to push MSCP END MESSAGES through a
 * real port, and it measured that SOME OF THEM COULD NOT ARRIVE. `vc_deliver`
 * hands a received sequenced frame to a SYSAP only when the classifier grounds
 * a Con.ID pair for the class it falls in; of the five MSCP end lengths FC-P6.2
 * measured -- SCA contents 86 (SCC), 90 (READ), 94 (WRITE), 102 (ONLINE), 110
 * (GUS) -- only 94 was such a class (110/102 also failed because the
 * connection-control rule that owns those lengths EXCLUDES the application
 * marker 10 an MSCP message carries at abs 60). The rest were counted
 * `vc_rx_undelivered` and dropped. FC-P6.3 gave the SEND side a length-generic
 * entry (`pe_vc_send_msg_var`) and the RECEIVE side had never grown the
 * matching half; this run counted BOTH halves so the size of the gap was
 * visible.
 *
 * This scenario did NOT itself widen a codec class rule to make itself pass:
 * §4(d) says the other length classes "do not reliably match this layout and
 * are therefore left undecoded", and asserting abs 64/68 for them would have
 * been a wire claim nothing measured (Rule 8). It instead ASKED THE CODEC per
 * message (`port_can_route`), carried what the port could carry, handed the
 * rest across itself, and COUNTED both -- so the gap was a number in the run
 * rather than a comment.
 *
 * **RESOLVED by FC-P2.7** (design docs/design-faithful-cluster-executive.md
 * §3.2.7, item's own ruling): SCS dispatches an application message on MTYPE
 * 10 to the CDT its Con.ID names, at ANY length -- the length-keyed classes
 * were a capture-census convenience, not the real dispatch rule. The frozen
 * classify table gained a length-generic `VMS_FCLS_SCS_APPLMSG` class keyed
 * on the SAME §4(h)(1b) envelope (the format word, MTYPE 10, the
 * self-consistent inner length) at any content length, so all five MSCP END
 * lengths now grant `VMS_FCAP_CONID` and `port_can_route` answers yes for
 * every one of them. `e->ends_bridged` is now asserted `== 0` and both
 * ports' own `vc_rx_undelivered` counters are asserted `== 0` below -- the
 * gap this scenario measured is closed, not merely narrower.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim.h"
#include "sim_lan.h"

#include "vms_pe_fsm.h"
#include "vms_cluster_codec_scs.h"
#include "vms_cluster_codec_blk.h"
#include "vms_cluster_codec_mscp.h"
#include "vms_mscp_cl_io_fsm.h"
#include "vms_mscp_srv_fsm.h"

/* ==========================================================================
 * 0. The two nodes, and the one connection between them
 * ========================================================================== */
#define CL_SYSID  1030u
#define SRV_SYSID 1031u

/* The two ends of the one `MSCP$DISK` connection. Both are values a real SCS
 * Con.ID allocator would mint; with no CDT in the simulator this scenario plays
 * that allocator, and says so (see the file header). */
#define CL_CONID  0x00020007u
#define SRV_CONID 0x00010005u

#define VOL_BLOCKS   32u
#define WRITE_LBN     7u
#define WRITE_BLOCKS  2u
#define WRITE_BYTES   (WRITE_BLOCKS * MSCP_SRV_BLOCK_SIZE)

/* The serving node's own advertised SCSNODE, as its CSB would carry it. */
static const uint8_t srv_scsnode[6] = { 'O', 'V', 'M', 'X', 'B', ' ' };

/* ==========================================================================
 * 1. The served volume and the served-I/O WORKER queue
 * ========================================================================== */
struct fake_vol {
	uint16_t unit;
	uint32_t blocks;
	uint8_t  data[VOL_BLOCKS * MSCP_SRV_BLOCK_SIZE];
};

/* One transfer the server handed to the worker. Held until the scenario drains
 * it, so "the fork thread did not touch the disk" is an assertion. */
struct worker_slot {
	uint8_t  busy;
	uint8_t  op;
	uint16_t unit;
	uint32_t tag;
	uint32_t lbn;
	uint32_t nblocks;
	uint8_t *buf;
};

/* ==========================================================================
 * 2. The whole two-node environment
 * ========================================================================== */
struct env {
	struct sim        sim;
	struct sim_node  *cl_node;
	struct sim_node  *srv_node;

	/* the SHIPPING FSMs */
	struct mscp_cl_ops  cl_ops;
	struct mscp_cl_fsm  cl;
	struct mscp_srv_ops srv_ops;
	struct mscp_srv_fsm srv;
	uint8_t srv_xferbuf[MSCP_SRV_MAX_REQS * 8u * MSCP_SRV_BLOCK_SIZE];

	/* the two ports' upper layers, replacing the harness recorder */
	struct pe_upper_ops cl_upper;
	struct pe_upper_ops srv_upper;

	struct fake_vol     vol;
	struct worker_slot  worker;

	/* the caller's own I/O buffer -- the class driver names THIS memory */
	uint8_t  iobuf[WRITE_BYTES];

	/* what really happened, counted where it happened */
	uint32_t cmds_to_srv, ends_to_cl;
	uint32_t cmds_unroutable;   /* a command class the port cannot route  */
	uint32_t ends_over_wire;    /* ends the PORT really carried           */
	uint32_t last_wire_end_len; /* ...and the body length of the last one */
	uint32_t ends_bridged;      /* ends the harness carried (E48)         */
	uint32_t send_failures;
	uint32_t requests_issued;
	uint32_t worker_submits, worker_completions;
	uint32_t units_ready;
	uint32_t done_calls;
	uint32_t done_handle;
	uint16_t done_status;
	uint32_t done_bytes;

	/* one scratch body: abs 56 onward (SCS's 16 bytes + the SYSAP body) */
	uint8_t  body[VMS_SCS_HDR_LEN + VMS_MSCP_CMD_BODY_LEN + 64u];
};

static struct env g_env;

/* ==========================================================================
 * 3. The message carrier -- one function each way, both through the port
 * ========================================================================== */

/*
 * Wrap a SYSAP body in SCS's own abs 56-71 envelope. The envelope is built by
 * the SCS CODEC, never by hand; the Con.ID pair is this scenario's test
 * identity (file header). Returns the abs-56-onward content length, or 0.
 */
static uint32_t build_scs_content(struct env *e, vms_conid_t remote_conid,
				  vms_conid_t local_conid, const uint8_t *body,
				  uint32_t len)
{
	struct vms_scs_hdr sh;
	uint32_t content = VMS_SCS_HDR_LEN + len;

	if (content > (uint32_t)sizeof(e->body))
		return 0u;
	memset(e->body, 0, sizeof(e->body));
	memset(&sh, 0, sizeof(sh));
	/*
	 * The inner length names the SCA content past its own 44-byte prefix --
	 * the codec's own arithmetic, inverted here exactly once so a receiver
	 * bounding the frame by it sees the whole message and no trailer.
	 */
	sh.inner_len = (uint16_t)((VMS_OFF_SYSAP_BODY + len) -
				  (VMS_ETH_HDR_LEN + 44u));
	/*
	 * FC-P2.7 (design sec3.2.7, E48) fix: this is the SCS HEADER's own
	 * mtype/ctrl_type field (content[46:48], abs 60) -- the application
	 * marker vms_scs_fsm.c's shipping send path (msg_transmit_var(),
	 * msg_transmit_long(), ctrl_prepare()) ALWAYS stamps
	 * VMS_SCS_CTRL_APPLICATION (10) into, never the abs-30 SCS message-
	 * TYPE BYTE (VMS_SCS_MT_MSG, 0x4b -- a DIFFERENT field, already set by
	 * vms_scs_seq_envelope_build() below). The two constants share no
	 * value by coincidence; writing the wrong one here silently defeated
	 * this scenario's own port_can_route() check for every END length
	 * this item grounds (86/90/102/110) until corrected.
	 */
	sh.mtype = (uint16_t)VMS_SCS_CTRL_APPLICATION;
	sh.conid_remote = remote_conid;
	sh.conid_local = local_conid;
	if (vms_scs_msg_body_build(&sh, body, len, e->body, content) !=
	    VMS_CODEC_OK)
		return 0u;
	return content;
}

/*
 * ***  WILL THE FAR PORT BE ABLE TO ROUTE THIS FRAME TO A CONNECTION?  ***
 *
 * ASKED OF THE CODEC, never decided here. `vc_deliver` hands a received
 * sequenced frame to a SYSAP only when the classifier grounds a Con.ID pair for
 * the class it falls in -- no Con.ID, no delivery, and the frame is counted
 * `vc_rx_undelivered`. So this ASSEMBLES the frame `pe_vc_send_msg_var` would
 * assemble, byte for byte in every field the classifier reads (the length, the
 * message type, and the application marker at abs 60 that the connection-control
 * rule excludes on), and asks `vms_frame_classify` for its capabilities.
 *
 * THE ANSWER IS NOT ALWAYS YES, AND THAT IS A REAL FINDING -- E48, raised by
 * this scenario. See the file header. This function does not widen anything: it
 * reports what the shipping codec says, and the caller acts honestly on it.
 */
static int port_can_route(const uint8_t *content, uint32_t content_len)
{
	uint8_t frame[PE_VC_FRAME_MAX];
	struct vms_scs_seq_envelope env;
	struct vms_frame_info fi;
	uint32_t total = PE_SEND_BODY_OFF + content_len;

	if (content_len == 0u || total > (uint32_t)sizeof(frame))
		return 0;
	memset(frame, 0, sizeof(frame));
	memset(&env, 0, sizeof(env));
	env.msgtype = VMS_SCS_MT_MSG;
	if (vms_scs_seq_envelope_build(&env, frame, (uint32_t)sizeof(frame),
				       NULL) != VMS_CODEC_OK)
		return 0;
	memcpy(frame + PE_SEND_BODY_OFF, content, content_len);
	if (vms_scs_seq_envelope_fixup_len(frame, (uint32_t)sizeof(frame),
					   total) != VMS_CODEC_OK)
		return 0;
	if (vms_frame_classify(frame, total, &fi) != VMS_CODEC_OK)
		return 0;
	return (fi.caps & VMS_FCAP_CONID) != 0u;
}

static int cl_op_send_cmd(void *ctx, vms_conid_t conid, const uint8_t *body,
			  uint32_t len)
{
	struct env *e = (struct env *)ctx;
	uint32_t content;

	(void)conid;
	e->cmds_to_srv++;
	content = build_scs_content(e, SRV_CONID, CL_CONID, body, len);
	/* Every MSCP command is the 94-content class §4(h)(1b) grounds, so a
	 * command ALWAYS goes over the wire. Asserted, not assumed. */
	if (!port_can_route(e->body, content)) {
		e->cmds_unroutable++;
		return -1;
	}
	if (pe_vc_send_msg_var(&e->cl_node->fsm, SRV_SYSID, SRV_CONID, e->body,
			       content) != PE_VC_SEND_OK) {
		e->send_failures++;
		return -1;
	}
	return 0;
}

static int srv_op_send_end(void *ctx, vms_conid_t conid, const uint8_t *body,
			   uint32_t len)
{
	struct env *e = (struct env *)ctx;
	uint32_t content;

	(void)conid;
	e->ends_to_cl++;
	content = build_scs_content(e, CL_CONID, SRV_CONID, body, len);
	if (port_can_route(e->body, content)) {
		e->ends_over_wire++;
		e->last_wire_end_len = len;
		if (pe_vc_send_msg_var(&e->srv_node->fsm, CL_SYSID, CL_CONID,
				       e->body, content) != PE_VC_SEND_OK) {
			e->send_failures++;
			return -1;
		}
		return 0;
	}
	/* E48: this class has no grounded Con.ID, so the far port would count
	 * it undelivered. The harness carries it, and SAYS SO. */
	e->ends_bridged++;
	return mscp_cl_fsm_end_msg(&e->cl, CL_CONID, body, len) == 0 ? 0 : -1;
}

/* ==========================================================================
 * 4. The two ports' upper layers
 *
 * The port hands up the WHOLE frame; the SYSAP body starts at the ONE position
 * the codec publishes. Neither of these interprets a byte.
 * ========================================================================== */

static void up_message_to_srv(void *ctx, vms_scs_sysid_t from,
			      vms_conid_t conid, const uint8_t *frame,
			      uint32_t len)
{
	struct env *e = (struct env *)ctx;

	(void)from; (void)conid;
	if (len <= VMS_OFF_SYSAP_BODY)
		return;
	(void)mscp_srv_fsm_command(&e->srv, SRV_CONID,
				   frame + VMS_OFF_SYSAP_BODY,
				   len - VMS_OFF_SYSAP_BODY);
}

static void up_message_to_cl(void *ctx, vms_scs_sysid_t from,
			     vms_conid_t conid, const uint8_t *frame,
			     uint32_t len)
{
	struct env *e = (struct env *)ctx;

	(void)from; (void)conid;
	if (len <= VMS_OFF_SYSAP_BODY)
		return;
	(void)mscp_cl_fsm_end_msg(&e->cl, CL_CONID,
				  frame + VMS_OFF_SYSAP_BODY,
				  len - VMS_OFF_SYSAP_BODY);
}

/* The block-transfer consumer, one dereference into the server -- the same
 * one-liner vms_mscp_srv.c's own `srv_block_data` makes. */
static void up_block_to_srv(void *ctx, vms_scs_sysid_t from, uint32_t name,
			    uint32_t offset, uint32_t len, uint32_t remaining)
{
	struct env *e = (struct env *)ctx;

	(void)from;
	mscp_srv_fsm_block_data(&e->srv, name, offset, len, remaining);
}

static void up_block_to_cl(void *ctx, vms_scs_sysid_t from, uint32_t name,
			   uint32_t offset, uint32_t len, uint32_t remaining)
{
	struct env *e = (struct env *)ctx;

	(void)from;
	mscp_cl_fsm_block_data(&e->cl, name, offset, len, remaining);
}

static void up_nothing_dg(void *ctx, vms_scs_sysid_t from, const uint8_t *body,
			  uint32_t len)
{
	(void)ctx; (void)from; (void)body; (void)len;
}

static void up_nothing_up(void *ctx, vms_scs_sysid_t peer)
{
	(void)ctx; (void)peer;
}

static void up_nothing_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	(void)ctx; (void)peer; (void)reason;
}

/* ==========================================================================
 * 5. The server's executive: units, named buffers, REQUEST DATA, the worker
 * ========================================================================== */

static int srv_op_unit_at(void *ctx, uint32_t index,
			  struct mscp_srv_unit_info *out)
{
	struct env *e = (struct env *)ctx;

	if (index != 0u)
		return -1;
	memset(out, 0, sizeof(*out));
	out->unit = e->vol.unit;
	out->unit_size = e->vol.blocks;
	out->unit_id = ((uint64_t)SRV_SYSID << 16) | e->vol.unit;
	out->media_id = 0x25400000u;
	out->media_valid = 1u;
	return 0;
}

/* WRITE's first half: name the staging slot as a DESTINATION on the SERVER's
 * own port. */
static int srv_op_recv_write_data(void *ctx, vms_conid_t conid,
				  vms_scs_sysid_t peer,
				  const struct mscp_srv_bufdesc *desc,
				  uint8_t *buf, uint32_t len,
				  uint32_t *name_out)
{
	struct env *e = (struct env *)ctx;

	(void)conid; (void)peer; (void)desc;
	return pe_blk_buf_register(&e->srv_node->fsm, buf, len,
				   (uint8_t)PE_BLK_ACC_DST, name_out) ==
			       PE_BLK_OK ? 0 : -1;
}

/*
 * WRITE's second half (FC-P6.5): the REQUEST DATA, through the SERVER's own
 * port. Every remote-side field comes out of `desc`, which the server read off
 * the client's own command; the local side is the name it just minted.
 */
static int srv_op_request_write_data(void *ctx, vms_conid_t conid,
				     vms_scs_sysid_t peer,
				     const struct mscp_srv_bufdesc *desc,
				     uint32_t local_name, uint32_t len)
{
	struct env *e = (struct env *)ctx;
	struct pe_blk_xfer x;

	(void)conid;
	memset(&x, 0, sizeof(x));
	x.peer = peer;
	x.dest_conid = desc->conid;
	x.local_name = local_name;
	x.local_offset = 0u;
	x.remote_name = desc->name;
	x.remote_offset = desc->offset;
	x.length = len;
	e->requests_issued++;
	return pe_blk_send_request(&e->srv_node->fsm, &x) == PE_BLK_OK ? 0 : -1;
}

static void srv_op_release_buffer(void *ctx, uint32_t name)
{
	struct env *e = (struct env *)ctx;

	(void)pe_blk_buf_release(&e->srv_node->fsm, name);
}

/*
 * READ's answer is NOT exercised by this scenario, and this refuses rather than
 * pretending: a READ that ran here would fail loudly instead of quietly looking
 * like it worked. (READ end to end is tests/cluster/host/test_mscp_cl.c's.)
 */
static int srv_op_send_read_data(void *ctx, vms_conid_t conid,
				 vms_scs_sysid_t peer,
				 const struct mscp_srv_bufdesc *desc,
				 const uint8_t *data, uint32_t len,
				 const uint8_t *end_body, uint32_t end_len)
{
	(void)ctx; (void)conid; (void)peer; (void)desc; (void)data; (void)len;
	(void)end_body; (void)end_len;
	return -1;
}

/* THE SERVED-I/O WORKER, QUEUED. This is the fork thread; it must not touch
 * storage (design §3.2.6). It records the request and returns. */
static int srv_op_io_submit(void *ctx, const struct mscp_srv_io_req *req)
{
	struct env *e = (struct env *)ctx;

	if (e->worker.busy)
		return -1;   /* one slot: an honest refusal, never an overwrite */
	e->worker.busy = 1u;
	e->worker.op = req->op;
	e->worker.unit = req->unit;
	e->worker.tag = req->tag;
	e->worker.lbn = req->lbn;
	e->worker.nblocks = req->nblocks;
	e->worker.buf = req->buf;
	e->worker_submits++;
	return 0;
}

/* ...and the worker itself, run explicitly by the scenario. */
static void worker_drain(struct env *e)
{
	uint32_t status = 0u;
	uint32_t off;

	if (!e->worker.busy)
		return;
	e->worker.busy = 0u;
	off = e->worker.lbn * MSCP_SRV_BLOCK_SIZE;

	if (e->worker.unit != e->vol.unit ||
	    e->worker.lbn + e->worker.nblocks > e->vol.blocks)
		status = 1u;
	else if (e->worker.op == (uint8_t)MSCP_SRV_IO_WRITE)
		memcpy(e->vol.data + off, e->worker.buf,
		       e->worker.nblocks * MSCP_SRV_BLOCK_SIZE);
	else
		memcpy(e->worker.buf, e->vol.data + off,
		       e->worker.nblocks * MSCP_SRV_BLOCK_SIZE);

	e->worker_completions++;
	mscp_srv_fsm_io_done(&e->srv, e->worker.tag, status);
}

/* ==========================================================================
 * 6. The client's executive
 * ========================================================================== */

static uint8_t cl_access_bits(uint8_t access)
{
	uint8_t bits = 0u;

	if ((access & MSCP_CL_BUF_IN) != 0u)
		bits |= (uint8_t)PE_BLK_ACC_DST;
	if ((access & MSCP_CL_BUF_OUT) != 0u)
		bits |= (uint8_t)PE_BLK_ACC_SRC;
	return bits;
}

static int cl_op_buf_register(void *ctx, uint8_t *base, uint32_t len,
			      uint8_t access, uint32_t *name_out)
{
	struct env *e = (struct env *)ctx;

	return pe_blk_buf_register(&e->cl_node->fsm, base, len,
				   cl_access_bits(access), name_out) ==
			       PE_BLK_OK ? 0 : -1;
}

static void cl_op_buf_release(void *ctx, uint32_t name)
{
	struct env *e = (struct env *)ctx;

	(void)pe_blk_buf_release(&e->cl_node->fsm, name);
}

static void cl_op_unit_ready(void *ctx, const struct mscp_cl_ucb *u)
{
	struct env *e = (struct env *)ctx;

	(void)u;
	e->units_ready++;
}

static void cl_op_unit_gone(void *ctx, const struct mscp_cl_ucb *u)
{
	(void)ctx; (void)u;
}

static void cl_op_io_done(void *ctx, uint32_t handle, uint16_t status,
			  uint32_t bytes)
{
	struct env *e = (struct env *)ctx;

	e->done_calls++;
	e->done_handle = handle;
	e->done_status = status;
	e->done_bytes = bytes;
}

/* ==========================================================================
 * 7. Shared ops: time comes from the VIRTUAL CLOCK, never from the host
 * ========================================================================== */

static uint32_t op_now_ms(void *ctx)
{
	struct env *e = (struct env *)ctx;

	return (uint32_t)sim_now_ms(&e->sim);
}

static uint64_t op_time_now(void *ctx)
{
	struct env *e = (struct env *)ctx;

	/* The VMS 100 ns epoch offset the simulator's own nodes boot with, plus
	 * the virtual clock -- one clock in this process, not two. */
	return 0x00bc055269000000ull + (uint64_t)sim_now_ms(&e->sim) * 10000ull;
}

static void op_log(void *ctx, const char *msg)
{
	(void)ctx; (void)msg;
}

/* ==========================================================================
 * 8. Wiring
 * ========================================================================== */

static void bind_srv_ops(struct env *e)
{
	memset(&e->srv_ops, 0, sizeof(e->srv_ops));
	e->srv_ops.unit_at = srv_op_unit_at;
	e->srv_ops.io_submit = srv_op_io_submit;
	e->srv_ops.send_end = srv_op_send_end;
	e->srv_ops.send_read_data = srv_op_send_read_data;
	e->srv_ops.recv_write_data = srv_op_recv_write_data;
	e->srv_ops.request_write_data = srv_op_request_write_data;
	e->srv_ops.release_buffer = srv_op_release_buffer;
	e->srv_ops.now_ms = op_now_ms;
	e->srv_ops.log = op_log;
	e->srv_ops.ctx = e;
}

static void bind_cl_ops(struct env *e)
{
	memset(&e->cl_ops, 0, sizeof(e->cl_ops));
	e->cl_ops.send_cmd = cl_op_send_cmd;
	e->cl_ops.buf_register = cl_op_buf_register;
	e->cl_ops.buf_release = cl_op_buf_release;
	e->cl_ops.unit_ready = cl_op_unit_ready;
	e->cl_ops.unit_gone = cl_op_unit_gone;
	e->cl_ops.io_done = cl_op_io_done;
	e->cl_ops.time_now = op_time_now;
	e->cl_ops.now_ms = op_now_ms;
	e->cl_ops.log = op_log;
	e->cl_ops.ctx = e;
}

/* Replace the harness's recorder with this scenario's SYSAP wiring. Done AFTER
 * the circuits form, because formation is HELLO/START/ACK and never reaches the
 * upper layer -- and done BEFORE any MSCP traffic, because from here on every
 * upcall is a real message or a real block transfer. */
static void bind_uppers(struct env *e)
{
	memset(&e->cl_upper, 0, sizeof(e->cl_upper));
	e->cl_upper.message = up_message_to_cl;
	e->cl_upper.datagram = up_nothing_dg;
	e->cl_upper.vc_up = up_nothing_up;
	e->cl_upper.vc_down = up_nothing_down;
	e->cl_upper.block_data = up_block_to_cl;
	e->cl_upper.ctx = e;
	pe_fsm_set_upper(&e->cl_node->fsm, &e->cl_upper);

	memset(&e->srv_upper, 0, sizeof(e->srv_upper));
	e->srv_upper.message = up_message_to_srv;
	e->srv_upper.datagram = up_nothing_dg;
	e->srv_upper.vc_up = up_nothing_up;
	e->srv_upper.vc_down = up_nothing_down;
	e->srv_upper.block_data = up_block_to_srv;
	e->srv_upper.ctx = e;
	pe_fsm_set_upper(&e->srv_node->fsm, &e->srv_upper);
}

static void vol_init(struct env *e)
{
	uint32_t i;

	e->vol.unit = 1u;
	e->vol.blocks = VOL_BLOCKS;
	for (i = 0; i < sizeof(e->vol.data); i++)
		e->vol.data[i] = (uint8_t)(i * 11u + 5u);
}

/*
 * Bring both nodes up on the virtual LAN and wait for the two circuits. Returns
 * 1 when both are OPEN.
 */
static int env_form(struct env *e, uint64_t seed, uint32_t loss_pct)
{
	struct sim_node_cfg a, b;
	struct sim_link link;

	memset(e, 0, sizeof(*e));
	sim_init(&e->sim, seed);

	sim_node_cfg_default(&a, "OVMXA", CL_SYSID, 0u);
	sim_node_cfg_default(&b, "OVMXB", SRV_SYSID, 1u);
	if (sim_add_node(&e->sim, &a) < 0 || sim_add_node(&e->sim, &b) < 0)
		return 0;

	memset(&link, 0, sizeof(link));
	link.loss_pct = (uint8_t)loss_pct;
	sim_lan_set_link_all(&e->sim.lan, &link);

	if (sim_boot_all(&e->sim) < 0)
		return 0;
	e->cl_node = sim_node_by_name(&e->sim, "OVMXA");
	e->srv_node = sim_node_by_name(&e->sim, "OVMXB");
	if (e->cl_node == NULL || e->srv_node == NULL)
		return 0;

	if (!sim_run_until(&e->sim, sim_all_vcs_open, NULL, 120000u))
		return 0;

	bind_uppers(e);
	bind_srv_ops(e);
	bind_cl_ops(e);
	vol_init(e);

	mscp_srv_fsm_init(&e->srv, &e->srv_ops);
	mscp_srv_fsm_set_ctlr_id(&e->srv, (uint64_t)SRV_SYSID);
	mscp_srv_fsm_bind_xferbuf(&e->srv, e->srv_xferbuf,
				  (uint32_t)sizeof(e->srv_xferbuf));
	(void)mscp_srv_fsm_refresh_units(&e->srv);
	mscp_cl_fsm_init(&e->cl, &e->cl_ops);
	return 1;
}

/* The class driver's discovery walk really runs across the wire: SCC x2, the
 * GUS enumeration and its terminator. Each step is a frame each way. */
static int cl_online(struct sim *s, void *ctx)
{
	struct env *e = (struct env *)ctx;

	(void)s;
	/* The walk is DONE when the controller reached Controller-Online AND at
	 * least one unit came out of its own GUS answers -- not at the first
	 * unit, which would stop the clock mid-enumeration. */
	return e->cl.cddb[0].state == (uint8_t)MSCP_CL_ST_ONLINE &&
	       mscp_cl_fsm_unit_count(&e->cl) > 0u;
}

static int env_connect(struct env *e)
{
	mscp_srv_fsm_conn_open(&e->srv, SRV_CONID, (vms_scs_sysid_t)CL_SYSID);
	(void)mscp_cl_fsm_conn_open(&e->cl, CL_CONID,
				    (vms_scs_sysid_t)SRV_SYSID, srv_scsnode,
				    (uint32_t)sizeof(srv_scsnode));
	return sim_run_until(&e->sim, cl_online, e, 60000u);
}

/* ==========================================================================
 * 9. THE SCENARIO
 * ========================================================================== */

static void test_two_nodes_complete_a_write(uint64_t seed, uint32_t loss_pct)
{
	struct env *e = &g_env;
	uint32_t i;
	char devnam[MSCP_CL_NAME_MAX];

	printf("-- 2 simulated nodes complete an MSCP WRITE (seed %llu, "
	       "%u %% loss)\n", (unsigned long long)seed, loss_pct);

	ct_check(env_form(e, seed, loss_pct),
		 "both circuits reached OPEN on the virtual LAN");
	ct_check(env_connect(e),
		 "the class driver's SCC/GUS walk ran to a served unit");
	ct_check_eq_u32(mscp_cl_fsm_unit_count(&e->cl), 1u,
			"the server's ONE unit was enumerated -- from its own "
			"GUS answers, not from this file");
	ct_check_eq_u32(e->units_ready, 1u, "and it became a device");

	memcpy(devnam, mscp_cl_fsm_ucb_at(&e->cl, 0)->devnam, sizeof(devnam));

	/* A pattern nothing else in this process could have produced. */
	for (i = 0; i < WRITE_BYTES; i++)
		e->iobuf[i] = (uint8_t)(0x3cu ^ (i * 5u + 17u));

	ct_check_eq_u32((unsigned long)mscp_cl_fsm_write(&e->cl, devnam,
							 WRITE_LBN,
							 WRITE_BLOCKS,
							 e->iobuf, WRITE_BYTES,
							 0x4242u),
			0, "the WRITE command was taken");

	/* The command crosses the wire, the server asks for the data, the
	 * client's PORT answers, and the answer crosses back. */
	(void)sim_run_ms(&e->sim, 30000u);

	ct_check_eq_u32(e->requests_issued, 1u,
			"the SERVER issued a REQUEST DATA -- it asks, it does "
			"not wait for data nobody requested (E41)");
	ct_check_eq_u32(e->cl_node->fsm.blk_req_answered, 1u,
			"and the CLIENT's PORT answered it, by itself");
	ct_check_eq_u32(e->cl_node->fsm.blk_req_unknown_buffer, 0u,
			"...from a buffer it really held");
	ct_check_eq_u32(e->srv.write_requests_issued, 1u, "counted by the server");

	/* DESIGN §3.2.6: the fork thread never touches storage. */
	ct_check_eq_u32(e->worker_submits, 1u,
			"the commit was HANDED to the served-I/O worker");
	ct_check_eq_u32(e->worker_completions, 0u,
			"and NOTHING was written while the fork context ran");
	ct_check_eq_u32(e->done_calls, 0u,
			"...so nothing completed on a write that has not happened");

	worker_drain(e);
	(void)sim_run_ms(&e->sim, 30000u);

	/* THE PROOF: the caller's real bytes are on the served volume, at the
	 * LBN the caller named -- carried there by the two shipping ports. */
	ct_check_eq_u32(e->worker_completions, 1u, "the worker wrote once");
	ct_check(memcmp(e->vol.data + WRITE_LBN * MSCP_SRV_BLOCK_SIZE,
			e->iobuf, WRITE_BYTES) == 0,
		 "and the served volume holds the CALLER'S OWN bytes at its own "
		 "LBN -- moved by the port's REQUEST DATA responder");

	/* ...and the caller is told, by the SERVER's own end message. */
	ct_check_eq_u32(e->done_calls, 1u, "the caller was completed");
	ct_check_eq_u32(e->done_handle, 0x4242u, "...on its own handle");
	ct_check_eq_u32(vms_mscp_status_major(e->done_status),
			(unsigned)VMS_MSCP_ST_SUCCESS, "with Success");
	ct_check_eq_u32(e->done_bytes, WRITE_BYTES,
			"and the SERVER's own byte count");
	ct_check_eq_u32(e->cl.writes_completed, 1u, "counted");
	ct_check_eq_u32(e->cl.writes_undelivered, 0u,
			"and NOTHING was left undelivered");
	ct_check_eq_u32(e->srv.writes_served, 1u, "the server served it");
	ct_check_eq_u32(e->send_failures, 0u, "no message was refused the wire");

	/* What really crossed the wire (E48 RESOLVED by FC-P2.7 -- see the
	 * file header). Every end message this run sends -- SCC/ONLINE/GUS
	 * during discovery, WRITE at completion -- is now a class the port
	 * routes, so NOTHING is bridged and NOTHING is undelivered. */
	ct_check_eq_u32(e->cmds_unroutable, 0u,
			"every MSCP COMMAND is a class the port routes, so all "
			"of them crossed the wire");
	ct_check(e->ends_over_wire > 0u,
		 "some end messages ARE a class the port routes, and those "
		 "really crossed the wire");
	ct_check_eq_u32(e->last_wire_end_len, VMS_MSCP_WRITE_END_LEN,
			"...and it is the WRITE END -- the message that "
			"completes this transfer really crossed the wire");
	ct_check_eq_u32(e->ends_bridged, 0u,
			 "E48 RESOLVED (FC-P2.7, design sec3.2.7): the "
			 "discovery-phase end messages (SCC/ONLINE/GUS) are "
			 "now ALSO a class the port routes -- the harness's "
			 "own bridge fallback never fires");
	ct_check_eq_u32(e->cl_node->fsm.vc_rx_undelivered, 0u,
			 "the CLIENT port's own vc_rx_undelivered counter is "
			 "zero -- every frame the server sent it was delivered");
	ct_check_eq_u32(e->srv_node->fsm.vc_rx_undelivered, 0u,
			 "the SERVER port's own vc_rx_undelivered counter is "
			 "zero -- every frame the client sent it was delivered");
}

/*
 * The same WRITE across a LOSSY wire. The port does not retransmit block frames
 * (vms_pe_fsm.h §8d "NO RING"), so this asserts the honest disjunction the
 * stack really promises: either the write completed with the caller's bytes on
 * the volume, or it did not complete at all and was reaped -- never a success
 * over a volume that did not receive them.
 */
static void test_write_under_loss_is_never_dishonest(uint64_t seed)
{
	struct env *e = &g_env;
	uint32_t i;
	char devnam[MSCP_CL_NAME_MAX];
	int landed;

	printf("-- WRITE across a lossy wire: complete-and-correct, or not "
	       "complete at all (seed %llu)\n", (unsigned long long)seed);

	if (!env_form(e, seed, 10u) || !env_connect(e)) {
		printf("     (formation/discovery did not finish on this seed; "
		       "nothing to assert)\n");
		return;
	}
	memcpy(devnam, mscp_cl_fsm_ucb_at(&e->cl, 0)->devnam, sizeof(devnam));
	for (i = 0; i < WRITE_BYTES; i++)
		e->iobuf[i] = (uint8_t)(0x91u ^ (i * 7u + 3u));

	if (mscp_cl_fsm_write(&e->cl, devnam, WRITE_LBN, WRITE_BLOCKS,
			      e->iobuf, WRITE_BYTES, 0x77u) != 0)
		return;

	(void)sim_run_ms(&e->sim, 30000u);
	worker_drain(e);
	(void)sim_run_ms(&e->sim, 30000u);

	landed = memcmp(e->vol.data + WRITE_LBN * MSCP_SRV_BLOCK_SIZE,
			e->iobuf, WRITE_BYTES) == 0;

	if (e->done_calls != 0u &&
	    vms_mscp_status_major(e->done_status) ==
		    (unsigned)VMS_MSCP_ST_SUCCESS) {
		ct_check(landed,
			 "a SUCCESS completion is only ever reported over a "
			 "volume that really received the bytes");
		ct_check_eq_u32(e->done_bytes, WRITE_BYTES,
				"...for the whole span");
	} else {
		ct_check(!landed || e->done_calls == 0u,
			 "no success was claimed; the request is outstanding or "
			 "was answered with a real failure");
	}
	ct_check_eq_u32(e->cl.short_transfers, 0u,
			"and no short transfer was ever completed as one");
}

int main(void)
{
	test_two_nodes_complete_a_write(1u, 0u);
	test_two_nodes_complete_a_write(7u, 0u);
	test_write_under_loss_is_never_dishonest(3u);
	test_write_under_loss_is_never_dishonest(11u);
	return ct_summary("sim_mscp_write");
}
