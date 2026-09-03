/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_mscp_cl.c - the R1 rung for FC-P7.1, the executive-resident MSCP DISK
 * CLASS DRIVER (src/kernel-core/vms_mscp_cl_io_fsm.{c,h} + the glue's own
 * bindings).
 *
 * ***  WHAT MAKES THIS AN INTEROP PROOF AND NOT A FIXTURE REPLAY  ***
 *
 * There is no stub server and no canned end message anywhere in this file.
 * THREE REAL, SHIPPING OBJECTS talk to each other in one host process:
 *
 *   the CLIENT  - FC-P7.1's `struct mscp_cl_fsm`, driving FC-P3.4's real
 *                 discovery FSM;
 *   the SERVER  - FC-P6.3's `struct mscp_srv_fsm`, answering from its own
 *                 state over a fake executive volume;
 *   the CLIENT's PORT
 *               - FC-P0.8/P1.2/P6.1's real `struct pe_fsm`, with a real
 *                 circuit, real named buffers (pe_blk_buf_register) and the
 *                 real receive path (pe_blk_rx_try / pe_blk_rx_trailer_try).
 *
 * So a READ here is end to end in the only sense that matters: the client asks
 * for a buffer name and gets one its OWN port minted; that name travels in the
 * command's Table A-6 descriptor and the SERVER reads it back off the wire; the
 * server's answer is carried in REAL block-transfer frames built through the
 * FC-P6.1 codec and pushed into the client's REAL port, which bounds-checks
 * them against its own registration and copies the bytes into the caller's
 * buffer; and the assertion is that those bytes are the fake block device's
 * own. Nothing is copied around the mechanism.
 *
 * The only thing simulated is the SERVER's port -- its frames are built here
 * through the same FC-P6.1 codec `pe_blk_send`/`pe_blk_send_read_end` build
 * through, which is exactly the discipline pe_fake_vc.h established for every
 * peer in this directory ("every stimulus frame is built through the SAME codec
 * the code under test builds through").
 *
 * ***  WHAT IS DELIBERATELY NOT PROVEN, AND WHY  ***
 *
 * WRITE's DATA does not move here, because nothing in this project grounds
 * WHICH SIDE initiates a WRITE block transfer (docs/cluster-integration-notes.md
 * E39's own lab-ask; the capture records only that the two 28-byte headers are
 * byte-identical). So the WRITE leg asserts exactly what IS grounded -- that a
 * real WRITE command goes out carrying a REAL named buffer and a REAL Con.ID,
 * that the server reads both off the wire, that NO completion is fabricated for
 * it, and that the class driver's own deadline reaps it honestly -- and it
 * asserts the ungrounded half as an OPEN FACT rather than papering over it.
 *
 * The R4 leg -- a booted OVMX node discovering and reading a volume another
 * cluster member really serves -- is tests/lab/tools/run_mscp_client_mount_gate.sh,
 * lab-deferred (exit 77); that file records the five questions only a real run
 * can settle, the allocation class among them.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "pe_fake_vc.h"
#include "vms_mscp_srv_fsm.h"
#include "vms_mscp_cl_io_fsm.h"
#include "vms_cluster_codec_blk.h"

/* ------------------------------------------------------------------ *
 * Identities. The client is OVMX; the server plays a VAX (the §3 decoder
 * ring's own address pair, as every sibling test in this directory uses).
 * ------------------------------------------------------------------ */
#define OVMX_SYSID 1030u
#define VAX1_SYSID 1025u

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

#define LAB_CREDITS 10u
#define OVMX_BOOT_TIME 0x00bc0552690a0000ull

/* The two ends of the one `MSCP$DISK` connection. Both are values a real
 * allocator would mint; the test plays SCS for them. */
#define CL_CONID  0x00020007u   /* OUR local Con.ID -- goes in P.BUFF   */
#define SRV_CONID 0x00010005u   /* the server's own local Con.ID        */

/* The serving node's own advertised SCSNODE, as the connection manager records
 * it on that node's CSB. The client's glue reads it there; this test hands the
 * same six bytes straight in. */
static const uint8_t vax1_scsnode[6] = { 'V', 'A', 'X', '1', ' ', ' ' };

#define SRV_BLOCKS 64u

/*
 * pe_fake_vc.h ships a whole peer toolkit. This test uses its addressing, its
 * HELLO/START/ACK builders and the FC-P1.1 envelope entries, but not its
 * fixed-length 190-content application-message builder or its emitted-frame
 * decoder -- both of which this file's traffic is the wrong shape for. Naming
 * them keeps -Werror=unused-function honest without editing a header five
 * other tests share.
 */
static void fake_vc_toolkit_unused(void)
{
	(void)fake_peer_seqmsg;
	(void)fake_vc_last;
}

/* ================================================================== *
 * 1. The SERVER side: FC-P6.3's real FSM over a fake executive volume
 * ================================================================== */

struct fake_unit {
	uint16_t unit;
	uint32_t size;
	uint8_t  write_protect;
	uint8_t  data[SRV_BLOCKS * MSCP_SRV_BLOCK_SIZE];
};

struct fake_exec {
	struct fake_unit units[MSCP_SRV_MAX_UNITS];
	uint32_t         n_units;
	uint32_t         reads;
	uint32_t         writes;

	/* what the server's WRITE half read off the client's own command --
	 * the fields the interop assertion is about. */
	uint32_t write_desc_name;
	uint32_t write_desc_conid;
	uint32_t write_desc_offset;
	uint32_t write_reqs;
};

/* ================================================================== *
 * 2. The CLIENT side: FC-P7.1's real FSM over a REAL port
 * ================================================================== */

struct cl_done {
	uint32_t calls;
	uint32_t handle;
	uint16_t status;
	uint32_t bytes;
};

/* The whole two-node environment, so every wiring function has one context. */
struct env {
	/* server */
	struct fake_exec     fake;
	struct mscp_srv_ops  srv_ops;
	struct mscp_srv_fsm  srv;
	uint8_t              srv_xferbuf[MSCP_SRV_MAX_REQS * 8u *
					 MSCP_SRV_BLOCK_SIZE];

	/* client */
	struct mscp_cl_ops   cl_ops;
	struct mscp_cl_fsm   cl;
	struct cl_done       done;

	/* the client's REAL port */
	struct pe_fsm        port;
	struct pe_ops        port_ops;
	struct fake_pe       port_fake;
	struct pe_upper_ops  upper;
	struct pe_vc         vcs[2];
	struct fake_peer     peer;      /* the server node, as the port sees it */
	uint8_t              wire[VMS_HELLO_PADDED_MAX_FRAME];
	uint16_t             peer_seq;  /* the server port's own send sequence  */

	/* test knobs */
	uint32_t now_ms;
	int      drop_ends;     /* swallow every end message the server sends  */
	int      drop_read_data;/* swallow the READ data AND its end message   */
	uint32_t deliver_end_without_data; /* deliver the end, drop the bytes  */

	/* the caller's own I/O buffer -- the class driver names THIS memory */
	uint8_t  iobuf[8u * MSCP_CL_BLOCK_SIZE];
};

/* ------------------------------------------------------------------ *
 * 3. The server's injected ops (its executive)
 * ------------------------------------------------------------------ */

static struct fake_unit *fake_find(struct fake_exec *e, uint16_t unit)
{
	uint32_t i;

	for (i = 0; i < e->n_units; i++) {
		if (e->units[i].unit == unit)
			return &e->units[i];
	}
	return NULL;
}

static int fake_unit_at(void *ctx, uint32_t index,
			struct mscp_srv_unit_info *out)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	if (index >= e->n_units)
		return -1;
	memset(out, 0, sizeof(*out));
	out->unit = e->units[index].unit;
	out->unit_size = e->units[index].size;
	out->unit_id = ((uint64_t)VAX1_SYSID << 16) | e->units[index].unit;
	out->media_id = 0x25400000u;
	out->media_valid = 1u;
	out->write_protect = e->units[index].write_protect;
	if (out->write_protect)
		out->unit_flags = VMS_MSCP_UF_WRITE_PROT_HW;
	return 0;
}

static uint32_t fake_do_read(struct fake_exec *e, uint16_t unit, uint32_t lbn,
			     uint32_t nblocks, uint8_t *buf)
{
	struct fake_unit *u = fake_find(e, unit);

	if (u == NULL || lbn + nblocks > u->size)
		return 1u;
	memcpy(buf, u->data + lbn * MSCP_SRV_BLOCK_SIZE,
	       nblocks * MSCP_SRV_BLOCK_SIZE);
	e->reads++;
	return 0u;
}

static uint32_t fake_do_write(struct fake_exec *e, uint16_t unit, uint32_t lbn,
			      uint32_t nblocks, const uint8_t *buf)
{
	struct fake_unit *u = fake_find(e, unit);

	if (u == NULL || lbn + nblocks > u->size)
		return 1u;
	memcpy(u->data + lbn * MSCP_SRV_BLOCK_SIZE, buf,
	       nblocks * MSCP_SRV_BLOCK_SIZE);
	e->writes++;
	return 0u;
}

/*
 * THE SERVER'S SERVED-I/O WORKER, INLINE (FC-P6.6). On the shipping stack a
 * served transfer is handed to a worker KTHREAD and answered later, on the fork
 * thread. THIS file is the CLIENT's proof -- it drives two real FSMs across a
 * real port -- so its fake worker performs the transfer and delivers the
 * completion immediately, which keeps the two-node exchange a straight line.
 *
 * That is a deliberate simplification OF THE TEST, not of the server: the
 * asynchrony itself (no block read on the command dispatch, the deferred abort,
 * the abandoned request, the stale completion) is proved in
 * tests/cluster/host/test_mscp_srv.c against a fake worker the test steps by
 * hand. Completing inline here is legal by the FSM's own contract -- see
 * srv_io_submit's note on publishing the HRB before the hand-over.
 */
static int fake_io_submit(void *ctx, const struct mscp_srv_io_req *req)
{
	/* srv_ops.ctx is the whole env, whose FIRST member is the fake
	 * executive -- the same one-object convention every op here uses. */
	struct env *env = (struct env *)ctx;
	uint32_t status;

	if (req->op == (uint8_t)MSCP_SRV_IO_READ)
		status = fake_do_read(&env->fake, req->unit, req->lbn,
				      req->nblocks, req->buf);
	else
		status = fake_do_write(&env->fake, req->unit, req->lbn,
				       req->nblocks, req->buf);
	mscp_srv_fsm_io_done(&env->srv, req->tag, status);
	return 0;
}

/* ------------------------------------------------------------------ *
 * 4. The wire between them
 *
 * Each direction is ONE function, and neither copies a field: the client's
 * command body is handed to the server verbatim, and the server's end body is
 * handed to the client verbatim, exactly as SCS delivers a SYSAP body.
 * ------------------------------------------------------------------ */

static struct env *g_env;   /* the ops carry ctx; this is only for the two
			     * bridges, which are the "network" and belong to
			     * no object */

/* CLIENT -> SERVER: one MSCP command body. */
static int bridge_client_cmd(void *ctx, vms_conid_t conid, const uint8_t *body,
			     uint32_t len)
{
	struct env *e = (struct env *)ctx;

	ct_check_eq_u32(conid, CL_CONID, "the command went out on OUR Con.ID");
	return mscp_srv_fsm_command(&e->srv, SRV_CONID, body, len);
}

/* SERVER -> CLIENT: one MSCP end-message body (the non-READ classes). */
static int bridge_server_end(void *ctx, vms_conid_t conid, const uint8_t *body,
			     uint32_t len)
{
	struct env *e = (struct env *)ctx;

	(void)conid;
	if (e->drop_ends)
		return 0;   /* the answer never arrives -- the timeout leg */
	return mscp_cl_fsm_end_msg(&e->cl, CL_CONID, body, len) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ *
 * 5. The SERVER's port, simulated -- but every frame built through the
 * FC-P6.1 codec, and pushed into the client's REAL receive path.
 * ------------------------------------------------------------------ */

static void ovmx_lavc(uint8_t out[6])
{
	vms_cluster_lavc_addr_build(OVMX_SYSID, out);
}

static void rx_frame(struct env *e, uint32_t len)
{
	if (len != 0)
		(void)pe_fsm_rx(&e->port, e->wire, len);
}

/* One standalone block-transfer frame, server -> client. */
static uint32_t build_block_frame(struct env *e, const struct vms_blk_hdr *h,
				  const uint8_t *data, uint32_t data_len)
{
	struct vms_scs_seq_envelope env;
	struct vms_frame_info fi;
	uint8_t dst_lavc[6];
	uint32_t total = VMS_BLK_DATA_OFF + data_len;
	uint16_t seq = ++e->peer_seq;

	if (total > sizeof(e->wire))
		return 0;
	memset(e->wire, 0, total);
	ovmx_lavc(dst_lavc);

	memset(&env, 0, sizeof(env));
	fake_vc_addr(&env.addr, &e->peer, ovmx_hw, dst_lavc);
	env.msgtype = VMS_SCS_MT_MSG;
	env.send_seq = seq;
	if (vms_scs_seq_envelope_build(&env, e->wire, sizeof(e->wire), NULL) !=
	    VMS_CODEC_OK)
		return 0;
	if (vms_blk_hdr_build(h, e->wire, sizeof(e->wire)) != VMS_CODEC_OK)
		return 0;
	if (data_len != 0)
		memcpy(e->wire + VMS_BLK_DATA_OFF, data, data_len);
	if (vms_scs_seq_envelope_fixup_len(e->wire, sizeof(e->wire), total) !=
	    VMS_CODEC_OK)
		return 0;
	if (vms_frame_classify(e->wire, total, &fi) != VMS_CODEC_OK)
		return 0;
	if (vms_scs_seq_stamp(e->wire, total, &fi, 0u, seq) != VMS_CODEC_OK)
		return 0;
	return total;
}

/*
 * The inner SCS message's own declared length, the ONE place this file does the
 * `content - 44` arithmetic. It is the exact inverse of the codec's
 * vms_scs_inner_frame_len(): a message whose SYSAP body is `body_len` stops at
 * abs 72 + body_len, and inner_len names the SCA content past its own 44-byte
 * prefix.
 */
#define INNERLEN_BIAS 44u
static uint16_t inner_len_for(uint32_t body_len)
{
	return (uint16_t)((VMS_OFF_SYSAP_BODY + body_len) -
			  (VMS_ETH_HDR_LEN + INNERLEN_BIAS));
}

/*
 * TRAP 1: the READ end message with the transfer's FINAL chunk piggybacked past
 * the length that end message's own inner header declares -- the exact shape
 * pe_blk_send_read_end() emits.
 */
static uint32_t build_read_end_frame(struct env *e, const struct vms_blk_hdr *h,
				     const uint8_t *tail, uint32_t tail_len,
				     const uint8_t *end_body, uint32_t end_len)
{
	struct vms_scs_seq_envelope env;
	struct vms_scs_hdr sh;
	struct vms_frame_info fi;
	uint8_t dst_lavc[6];
	uint32_t inner = VMS_OFF_SYSAP_BODY + end_len;
	uint32_t total = 0u;
	uint16_t seq = ++e->peer_seq;

	memset(e->wire, 0, sizeof(e->wire));
	ovmx_lavc(dst_lavc);

	memset(&env, 0, sizeof(env));
	fake_vc_addr(&env.addr, &e->peer, ovmx_hw, dst_lavc);
	env.msgtype = VMS_SCS_MT_MSG;
	env.send_seq = seq;
	if (vms_scs_seq_envelope_build(&env, e->wire, sizeof(e->wire), NULL) !=
	    VMS_CODEC_OK)
		return 0;

	/* abs 56..71 + the SYSAP body, through the codec's own body builder. */
	memset(&sh, 0, sizeof(sh));
	sh.inner_len = inner_len_for(end_len);
	sh.mtype = VMS_SCS_MT_MSG;
	sh.conid_remote = CL_CONID;
	sh.conid_local = SRV_CONID;
	if (vms_scs_msg_body_build(&sh, end_body, end_len,
				   e->wire + VMS_OFF_SCSCTRL_INNERLEN,
				   VMS_SCS_HDR_LEN + end_len) != VMS_CODEC_OK)
		return 0;

	if (vms_blk_trailer_build(h, tail, tail_len, e->wire, sizeof(e->wire),
				  inner, &total) != VMS_CODEC_OK)
		return 0;
	if (vms_scs_seq_envelope_fixup_len(e->wire, sizeof(e->wire), total) !=
	    VMS_CODEC_OK)
		return 0;
	if (vms_frame_classify(e->wire, total, &fi) != VMS_CODEC_OK)
		return 0;
	if (vms_scs_seq_stamp(e->wire, total, &fi, 0u, seq) != VMS_CODEC_OK)
		return 0;
	return total;
}

/* One transfer header, filled the way a real server's port fills it: every
 * remote-side field READ off the client's own command descriptor. */
static void blk_hdr_init(struct vms_blk_hdr *h,
			 const struct mscp_srv_bufdesc *desc, uint32_t done,
			 uint32_t remaining)
{
	memset(h, 0, sizeof(*h));
	h->dest_conid = desc->conid;
	h->bytes_remaining = remaining;
	h->src_name = 0x0abc0001u;    /* the server's own staging buffer name */
	h->src_offset = done;
	h->dst_name = desc->name;     /* the CLIENT's name, off its command   */
	h->dst_offset = desc->offset + done;
}

/*
 * READ's whole answer, in the order the vms291 capture has it: standalone
 * block frames for everything but the final chunk, then the end message with
 * that chunk piggybacked on it.
 */
static int bridge_server_read_data(void *ctx, vms_conid_t conid,
				   vms_scs_sysid_t peer,
				   const struct mscp_srv_bufdesc *desc,
				   const uint8_t *data, uint32_t len,
				   const uint8_t *end_body, uint32_t end_len)
{
	struct env *e = (struct env *)ctx;
	struct vms_blk_hdr h;
	uint32_t tail = len % 512u ? len % 512u : (len < 512u ? len : 512u);
	uint32_t done = 0u;

	(void)conid; (void)peer;
	if (e->drop_read_data)
		return 0;   /* the bytes never move -- the timeout leg */

	while (len - done > tail) {
		uint32_t n = len - done - tail;

		if (n > 512u)
			n = 512u;
		blk_hdr_init(&h, desc, done, len - done);
		rx_frame(e, build_block_frame(e, &h, data + done, n));
		done += n;
	}

	if (e->deliver_end_without_data) {
		/* The dishonest shape this test exists to catch: a successful
		 * end message on top of a transfer whose bytes did not all
		 * arrive. Delivered deliberately, to prove the client REFUSES
		 * it rather than reporting the server's success. */
		return mscp_cl_fsm_end_msg(&e->cl, CL_CONID, end_body,
					   end_len) == 0 ? 0 : -1;
	}

	blk_hdr_init(&h, desc, done, len - done);
	rx_frame(e, build_read_end_frame(e, &h, data + done, len - done,
					 end_body, end_len));
	return mscp_cl_fsm_end_msg(&e->cl, CL_CONID, end_body, end_len) == 0
		       ? 0 : -1;
}

/*
 * WRITE's half. The server names its own staging buffer as a destination and
 * WAITS -- and nothing here moves the client's bytes, because which side
 * initiates a WRITE block transfer is not grounded (see the file header).
 * What IS asserted is that the server got the client's REAL descriptor.
 */
static int bridge_server_recv_write(void *ctx, vms_conid_t conid,
				    vms_scs_sysid_t peer,
				    const struct mscp_srv_bufdesc *desc,
				    uint8_t *buf, uint32_t len,
				    uint32_t *name_out)
{
	struct env *e = (struct env *)ctx;

	(void)conid; (void)peer; (void)buf; (void)len;
	e->fake.write_desc_name = desc->name;
	e->fake.write_desc_conid = desc->conid;
	e->fake.write_desc_offset = desc->offset;
	e->fake.write_reqs++;
	*name_out = 0x0abc0002u;   /* the server port's own destination name */
	return 0;
}

static void bridge_release(void *ctx, uint32_t name)
{
	(void)ctx; (void)name;
}

static uint32_t bridge_now(void *ctx)
{
	return ((struct env *)ctx)->now_ms;
}

static void bridge_log(void *ctx, const char *msg)
{
	(void)ctx; (void)msg;
}

/* ------------------------------------------------------------------ *
 * 6. The client's injected ops
 * ------------------------------------------------------------------ */

static int cl_buf_register(void *ctx, uint8_t *base, uint32_t len,
			   uint8_t access, uint32_t *name_out)
{
	struct env *e = (struct env *)ctx;
	uint8_t bits = 0u;

	/* The SAME mapping vms_mscp_cl.c's cl_access_bits() makes. */
	if ((access & MSCP_CL_BUF_IN) != 0u)
		bits |= (uint8_t)PE_BLK_ACC_DST;
	if ((access & MSCP_CL_BUF_OUT) != 0u)
		bits |= (uint8_t)PE_BLK_ACC_SRC;
	return pe_blk_buf_register(&e->port, base, len, bits, name_out) ==
			       PE_BLK_OK ? 0 : -1;
}

static void cl_buf_release(void *ctx, uint32_t name)
{
	struct env *e = (struct env *)ctx;

	(void)pe_blk_buf_release(&e->port, name);
}

struct unit_note {
	uint32_t added;
	uint32_t removed;
	char     last_added[MSCP_CL_NAME_MAX];
	char     last_removed[MSCP_CL_NAME_MAX];
};

static struct unit_note g_units;

static void cl_unit_ready(void *ctx, const struct mscp_cl_ucb *u)
{
	(void)ctx;
	g_units.added++;
	memcpy(g_units.last_added, u->devnam, sizeof(g_units.last_added));
}

static void cl_unit_gone(void *ctx, const struct mscp_cl_ucb *u)
{
	(void)ctx;
	g_units.removed++;
	memcpy(g_units.last_removed, u->devnam, sizeof(g_units.last_removed));
}

static void cl_io_done(void *ctx, uint32_t handle, uint16_t status,
		       uint32_t bytes)
{
	struct env *e = (struct env *)ctx;

	e->done.calls++;
	e->done.handle = handle;
	e->done.status = status;
	e->done.bytes = bytes;
}

static uint64_t cl_time_now(void *ctx)
{
	(void)ctx;
	return 0x00bc055269000000ull;
}

/* ------------------------------------------------------------------ *
 * 7. The client's REAL port, and its block-completion forwarder
 * ------------------------------------------------------------------ */

static void up_message(void *ctx, vms_scs_sysid_t from, vms_conid_t conid,
		       const uint8_t *body, uint32_t len)
{
	(void)ctx; (void)from; (void)conid; (void)body; (void)len;
}

static void up_datagram(void *ctx, vms_scs_sysid_t from, const uint8_t *body,
			uint32_t len)
{
	(void)ctx; (void)from; (void)body; (void)len;
}

static void up_vc_up(void *ctx, vms_scs_sysid_t peer)
{
	(void)ctx; (void)peer;
}

static void up_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	(void)ctx; (void)peer; (void)reason;
}

/* The SAME one-line forwarder vms_scs.c's block consumer makes. */
static void up_block(void *ctx, vms_scs_sysid_t from, uint32_t name,
		     uint32_t offset, uint32_t len, uint32_t remaining)
{
	struct env *e = (struct env *)ctx;

	(void)from;
	mscp_cl_fsm_block_data(&e->cl, name, offset, len, remaining);
}

static uint64_t fake_now_vms(void *ctx)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	return 0x00bc055269000000ull + (uint64_t)f->now_ms * 10000ull;
}

static void port_init(struct env *e)
{
	struct pe_identity id;

	fake_pe_ops_init(&e->port_ops, &e->port_fake);
	e->port_ops.now_vms = fake_now_vms;

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = 1500;
	memcpy(id.sw_version, "VMX V0.6", 8);
	id.sw_version_valid = 1;
	memcpy(id.hw_type, "X86 ", 4);
	id.hw_type_valid = 1;
	id.cluster_credits = LAB_CREDITS;
	id.cluster_credits_valid = 1;
	id.incarnation_time = OVMX_BOOT_TIME;
	id.incarnation_time_valid = 1;
	id.timvcfail_ms = 16000;
	id.vc_retransmit_ms = 2000;

	(void)pe_fsm_init(&e->port, &id, OVMX_SYSID, &e->port_ops);
	pe_fsm_bind_vcs(&e->port, e->vcs, 2);

	e->upper.message = up_message;
	e->upper.datagram = up_datagram;
	e->upper.vc_up = up_vc_up;
	e->upper.vc_down = up_vc_down;
	e->upper.block_data = up_block;
	e->upper.ctx = e;
	pe_fsm_set_upper(&e->port, &e->upper);
	pe_fsm_start(&e->port);
	fake_peer_init(&e->peer, VAX1_SYSID, vax1_hw, "VAX1");
}

/* Bring the client's circuit to the server node OPEN, the way test_pe_block.c
 * does: real HELLOs, a real START and a real ACK, all through the codec. */
static void port_open_circuit(struct env *e)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_hello(&e->peer, ovmx_hw, dst_lavc,
				    PE_PFW_VERIFY_B2, 1, 0, e->wire,
				    sizeof(e->wire)));
	rx_frame(e, fake_peer_hello(&e->peer, ovmx_hw, dst_lavc,
				    PE_PFW_VERIFY_B4, 1, 0, e->wire,
				    sizeof(e->wire)));
	rx_frame(e, fake_peer_start(&e->peer, VAX1_SYSID, ovmx_hw, dst_lavc, 0,
				    1, 0, LAB_CREDITS, e->wire,
				    sizeof(e->wire)));
	rx_frame(e, fake_peer_vc_ack(&e->peer, ovmx_hw, dst_lavc, 1, 0, e->wire,
				     sizeof(e->wire)));
	/* The circuit's recv_seq is 0 once formation completes, so the peer's
	 * NEXT sequenced frame is 1. Off by one here and every block frame
	 * this file injects would be a gap the port correctly discards. */
	e->peer_seq = 0u;
	fake_pe_clear_frames(&e->port_fake);
}

/* ------------------------------------------------------------------ *
 * 8. Environment setup
 * ------------------------------------------------------------------ */

static void env_add_unit(struct env *e, uint16_t unit, uint32_t nblocks,
			 int write_protect)
{
	struct fake_unit *u = &e->fake.units[e->fake.n_units++];
	uint32_t i;

	memset(u, 0, sizeof(*u));
	u->unit = unit;
	u->size = nblocks;
	u->write_protect = (uint8_t)write_protect;
	for (i = 0; i < nblocks * MSCP_SRV_BLOCK_SIZE; i++)
		u->data[i] = (uint8_t)(i * 7u + unit + 3u);
}

static void env_bind(struct env *e)
{
	memset(&e->srv_ops, 0, sizeof(e->srv_ops));
	e->srv_ops.unit_at = fake_unit_at;
	e->srv_ops.io_submit = fake_io_submit;
	e->srv_ops.send_end = bridge_server_end;
	e->srv_ops.send_read_data = bridge_server_read_data;
	e->srv_ops.recv_write_data = bridge_server_recv_write;
	e->srv_ops.release_buffer = bridge_release;
	e->srv_ops.now_ms = bridge_now;
	e->srv_ops.log = bridge_log;
	e->srv_ops.ctx = e;

	memset(&e->cl_ops, 0, sizeof(e->cl_ops));
	e->cl_ops.send_cmd = bridge_client_cmd;
	e->cl_ops.buf_register = cl_buf_register;
	e->cl_ops.buf_release = cl_buf_release;
	e->cl_ops.unit_ready = cl_unit_ready;
	e->cl_ops.unit_gone = cl_unit_gone;
	e->cl_ops.io_done = cl_io_done;
	e->cl_ops.time_now = cl_time_now;
	e->cl_ops.now_ms = bridge_now;
	e->cl_ops.log = bridge_log;
	e->cl_ops.ctx = e;
}

/*
 * Bring both nodes up and let the class driver run its whole discovery walk
 * against the real server. On return the client is Controller-Online and every
 * served unit is a device.
 *
 * `scsnode_len == 0` is the honest-absence case: the connection manager holds
 * no name for the serving node.
 */
static void env_start(struct env *e, uint32_t scsnode_len)
{
	memset(e, 0, sizeof(*e));
	memset(&g_units, 0, sizeof(g_units));
	g_env = e;
	env_bind(e);
	port_init(e);
	port_open_circuit(e);

	mscp_srv_fsm_init(&e->srv, &e->srv_ops);
	mscp_srv_fsm_set_ctlr_id(&e->srv, (uint64_t)VAX1_SYSID);
	mscp_srv_fsm_bind_xferbuf(&e->srv, e->srv_xferbuf,
				  (uint32_t)sizeof(e->srv_xferbuf));

	mscp_cl_fsm_init(&e->cl, &e->cl_ops);
	(void)scsnode_len;
}

/* The units the server serves, then the open that starts the walk. */
static void env_go(struct env *e, uint32_t scsnode_len)
{
	(void)mscp_srv_fsm_refresh_units(&e->srv);
	mscp_srv_fsm_conn_open(&e->srv, SRV_CONID, (vms_scs_sysid_t)OVMX_SYSID);
	(void)mscp_cl_fsm_conn_open(&e->cl, CL_CONID,
				    (vms_scs_sysid_t)VAX1_SYSID, vax1_scsnode,
				    scsnode_len);
}

/* ================================================================== *
 * 9. THE TESTS
 * ================================================================== */

/* ------------------------------------------------------------------ *
 * 9.1 Discovery: a served unit becomes a real `<SCSNODE>$DUAn:` device
 * ------------------------------------------------------------------ */
static void test_discovery_makes_devices(void)
{
	static struct env e;

	printf("-- discovery: the peer's OWN GUS answers become real devices\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_add_unit(&e, 3u, 16u, 0);
	env_go(&e, 6u);

	ct_check_eq_u32(e.cl.cddb[0].state, MSCP_CL_ST_ONLINE,
			"the whole SCC x2 + GUS walk ran to its terminator");
	ct_check_eq_u32(mscp_cl_fsm_unit_count(&e.cl), 2u,
			"BOTH served units were enumerated -- no more, no fewer");
	ct_check_eq_u32(g_units.added, 2u, "...and both became devices");
	ct_check(strcmp(g_units.last_added, "VAX1$DUA3:") == 0,
		 "the device name is composed of the peer's OWN advertised "
		 "SCSNODE and the unit number its OWN GUS end returned");
	ct_check_eq_u32(e.cl.units_registered, 2u, "counted where it happened");
	ct_check_eq_u32(e.cl.alloclass_absent, 2u,
			"and the missing ALLOCLASS is COUNTED, not guessed "
			"(vms_mscp_cl_io_fsm.h: OVMX holds no peer ALLOCLASS)");

	/* Every value on the device came off the wire, not out of this file. */
	ct_check(e.cl.ucb[0].unit.unit_id != 0u,
		 "the unit identifier is the peer's own, and non-zero");
	ct_check_eq_u32(e.cl.ucb[0].unit_size_valid, 0u,
			"and no unit SIZE is claimed before an ONLINE end "
			"carries one -- a GUS end does not have it");
}

/* ------------------------------------------------------------------ *
 * 9.2 No SCSNODE -> no device. The unit is still enumerated.
 * ------------------------------------------------------------------ */
static void test_no_scsnode_means_no_device(void)
{
	static struct env e;

	printf("-- a unit whose name cannot be composed gets NO device\n");
	env_start(&e, 0u);
	env_add_unit(&e, 1u, 32u, 0);
	env_go(&e, 0u);   /* the connection manager holds no name for the peer */

	ct_check_eq_u32(mscp_cl_fsm_unit_count(&e.cl), 1u,
			"the unit is still enumerated -- it really is served");
	ct_check_eq_u32(e.cl.units_unnamed, 1u,
			"...but it could not be named, and that is COUNTED");
	ct_check_eq_u32(e.cl.units_registered, 0u,
			"...so NO device was created (INV-6: a served disk "
			"under a made-up name is a fabricated device)");
	ct_check_eq_u32(g_units.added, 0u, "and the glue was never asked to");
	ct_check_eq_u32(e.cl.ucb[0].registered, 0u, "the UCB says so too");
}

/* ------------------------------------------------------------------ *
 * 9.3 THE READ, END TO END: the bytes come from the server's block device
 * ------------------------------------------------------------------ */
static void test_read_end_to_end(void)
{
	static struct env e;
	uint32_t nblocks = 3u;
	uint32_t bytes = nblocks * MSCP_CL_BLOCK_SIZE;

	printf("-- READ: real named buffer, real block frames, REAL bytes\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_go(&e, 6u);

	ct_check_eq_u32((unsigned long)mscp_cl_fsm_read(&e.cl, "VAX1$DUA1:",
							4u, nblocks, e.iobuf,
							(uint32_t)sizeof(e.iobuf),
							0x1234u),
			0, "the READ was taken");
	/* The unit was not online yet, so the driver ONLINEd it first and the
	 * request rode out behind the real ONLINE end. */
	ct_check_eq_u32(e.cl.onlines_sent, 1u,
			"sec 4.3: the unit was brought ONLINE first");
	ct_check_eq_u32(e.cl.ucb[0].online, 1u, "...by a REAL ONLINE end");
	ct_check_eq_u32(e.cl.ucb[0].unit_size_valid, 1u,
			"...which is where the unit SIZE finally comes from");
	ct_check_eq_u32(e.cl.ucb[0].unit_size, 32u,
			"...and it is the volume's real block count");
	ct_check_eq_u32(e.cl.reads_issued, 1u, "then the READ command went out");

	ct_check_eq_u32(e.fake.reads, 1u,
			"the server really read its block device");
	ct_check_eq_u32(e.done.calls, 1u, "the request completed exactly once");
	ct_check_eq_u32(e.done.handle, 0x1234u, "with the caller's own handle");
	ct_check_eq_u32(vms_mscp_status_major(e.done.status),
			(unsigned)VMS_MSCP_ST_SUCCESS,
			"and the SERVER's own success status");
	ct_check_eq_u32(e.done.bytes, bytes,
			"and the SERVER's own byte count");

	/* THE ASSERTION THIS WHOLE FILE EXISTS FOR. */
	ct_check(memcmp(e.iobuf,
			e.fake.units[0].data + 4u * MSCP_SRV_BLOCK_SIZE,
			bytes) == 0,
		 "the caller's buffer holds the SERVER's own block-device "
		 "bytes, moved by real named-buffer transfers");

	ct_check_eq_u32(e.cl.reads_completed, 1u, "counted");
	ct_check_eq_u32(e.cl.short_transfers, 0u, "and nothing was short");
	ct_check(e.cl.block_bytes_rx == bytes,
		 "every byte was accounted through the port's completions");
	ct_check(e.port.blk_rx_trailer == 1u,
		 "the FINAL chunk arrived PIGGYBACKED on the end message "
		 "(TRAP 1's receive arm) -- not as a standalone frame");
}

/* ------------------------------------------------------------------ *
 * 9.4 A short READ is REFUSED, not reported as the server's success
 * ------------------------------------------------------------------ */
static void test_short_read_is_refused(void)
{
	static struct env e;

	printf("-- a successful end message on a short transfer is REFUSED\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_go(&e, 6u);
	e.deliver_end_without_data = 1u;

	ct_check_eq_u32((unsigned long)mscp_cl_fsm_read(&e.cl, "VAX1$DUA1:", 0u,
							2u, e.iobuf,
							(uint32_t)sizeof(e.iobuf),
							7u),
			0, "the READ was taken");
	ct_check_eq_u32(e.done.calls, 1u, "it completed");
	ct_check_eq_u32(vms_mscp_status_major(e.done.status),
			(unsigned)VMS_MSCP_ST_HOST_BUF_ERR,
			"with Host Buffer Access Error -- NOT the server's "
			"success on a buffer that never got the bytes");
	/* Half the transfer DID land (one standalone frame); the piggybacked
	 * tail did not. The completion reports what really arrived -- 512 --
	 * not the 1024 the server's end message claimed. */
	ct_check_eq_u32(e.done.bytes, 512u, "and the bytes that really landed");
	ct_check_eq_u32(e.cl.short_transfers, 1u, "counted where it happened");
	ct_check_eq_u32(e.cl.reads_completed, 0u, "no read was completed");
}

/* ------------------------------------------------------------------ *
 * 9.5 CONTROLLER TIMEOUT: a dropped answer is reaped, never hung
 * ------------------------------------------------------------------ */
static void test_controller_timeout(void)
{
	static struct env e;
	uint32_t reaped;

	printf("-- the controller timeout reaps a request whose answer never "
	       "came\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_go(&e, 6u);

	/* Bring the unit online normally, THEN start dropping. */
	ct_check_eq_u32((unsigned long)mscp_cl_fsm_read(&e.cl, "VAX1$DUA1:", 0u,
							1u, e.iobuf,
							(uint32_t)sizeof(e.iobuf),
							1u),
			0, "a first READ works");
	ct_check_eq_u32(e.done.calls, 1u, "and completes");

	/* Two deadlines were armed for that one READ: one while it waited on the
	 * real ONLINE, one when the transfer itself went out. BOTH came from
	 * the server's own P.CTMO. */
	ct_check_eq_u32(e.cl.deadline_from_ctmo, 2u,
			"the deadline came from the SERVER's own declared "
			"controller timeout, not a number this client chose");
	ct_check_eq_u32(e.cl.deadline_from_own_htmo, 0u, "...so not the fallback");

	e.drop_read_data = 1;
	ct_check_eq_u32((unsigned long)mscp_cl_fsm_read(&e.cl, "VAX1$DUA1:", 1u,
							1u, e.iobuf,
							(uint32_t)sizeof(e.iobuf),
							2u),
			0, "a second READ goes out");
	ct_check_eq_u32(e.done.calls, 1u,
			"...and does NOT complete: nothing came back");

	/* One tick short of the controller's own timeout: still outstanding. */
	e.now_ms = MSCP_SRV_CTLR_TIMEOUT_SECS * 1000u - 1u;
	ct_check_eq_u32(mscp_cl_fsm_tick(&e.cl), 0u,
			"before the deadline it is still in flight");
	ct_check_eq_u32(e.done.calls, 1u, "...and still not completed");

	e.now_ms = MSCP_SRV_CTLR_TIMEOUT_SECS * 1000u;
	reaped = mscp_cl_fsm_tick(&e.cl);
	ct_check_eq_u32(reaped, 1u, "at the deadline it is reaped");
	ct_check_eq_u32(e.done.calls, 2u, "the caller IS told");
	ct_check_eq_u32(e.done.handle, 2u, "...about the right request");
	ct_check_eq_u32(vms_mscp_status_major(e.done.status),
			(unsigned)VMS_MSCP_ST_ABORTED,
			"with a REAL Command Aborted (Table B-1 ST.ABO)");
	ct_check_eq_u32(e.done.bytes, 0u, "and no bytes claimed");
	ct_check_eq_u32(e.cl.reqs_aborted, 1u, "counted");
	ct_check_eq_u32(e.cl.reads_completed, 1u,
			"the aborted read is NOT counted as completed");
}

/* ------------------------------------------------------------------ *
 * 9.6 WRITE, as far as the direction is GROUNDED (E39)
 * ------------------------------------------------------------------ */
static void test_write_as_far_as_grounded(void)
{
	static struct env e;
	uint32_t buf_name;

	printf("-- WRITE: a real command with a REAL buffer name; the data "
	       "direction stays ungrounded and is reaped honestly\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_go(&e, 6u);

	memset(e.iobuf, 0xa5, sizeof(e.iobuf));
	ct_check_eq_u32((unsigned long)mscp_cl_fsm_write(&e.cl, "VAX1$DUA1:",
							 2u, 1u, e.iobuf,
							 (uint32_t)sizeof(e.iobuf),
							 0x99u),
			0, "the WRITE was taken");
	ct_check_eq_u32(e.cl.writes_issued, 1u, "a real WRITE command went out");
	ct_check_eq_u32(e.fake.write_reqs, 1u, "the server received it");

	/* The interop assertion: the name the SERVER read off the wire is the
	 * one OUR port really minted for the caller's buffer. */
	buf_name = e.cl.cdrp[0].buf_name;
	ct_check(buf_name != 0u, "our port really named the caller's buffer");
	ct_check_eq_u32(e.fake.write_desc_name, buf_name,
			"and THAT is the name the server read out of the "
			"command's Table A-6 descriptor");
	ct_check_eq_u32(e.fake.write_desc_conid, CL_CONID,
			"...beside OUR OWN local Con.ID, from the connection");
	ct_check(pe_blk_buf_lookup(&e.port, buf_name) != NULL,
		 "and the buffer is really registered with the port");
	ct_check_eq_u32(pe_blk_buf_lookup(&e.port, buf_name)->access,
			(unsigned)PE_BLK_ACC_SRC,
			"...as a SOURCE the peer's port may read from");

	/* The ungrounded half, asserted as an OPEN FACT so it cannot drift. */
	ct_check_eq_u32(e.done.calls, 0u,
			"RECORDED (integration note E39): nothing completes -- "
			"which side initiates a WRITE block transfer is not "
			"grounded, and this driver invents no initiation");
	ct_check_eq_u32(e.fake.writes, 0u, "and not a block was written");

	/* ...and the honest end: the deadline, not a hang. */
	e.now_ms = MSCP_SRV_CTLR_TIMEOUT_SECS * 1000u;
	ct_check_eq_u32(mscp_cl_fsm_tick(&e.cl), 1u, "the deadline reaps it");
	ct_check_eq_u32(e.done.calls, 1u, "and the caller is told");
	ct_check_eq_u32(vms_mscp_status_major(e.done.status),
			(unsigned)VMS_MSCP_ST_ABORTED, "Command Aborted");
	ct_check_eq_u32(e.cl.writes_undelivered, 1u,
			"and the gap is a MEASURED number, not a comment");
	ct_check(pe_blk_buf_lookup(&e.port, buf_name) == NULL,
		 "the buffer name was released with the request -- a name that "
		 "outlived its transfer is a landing zone for a stale frame");
}

/* ------------------------------------------------------------------ *
 * 9.7 The path goes away: the devices go with it, the requests are ended
 * ------------------------------------------------------------------ */
static void test_close_withdraws_devices(void)
{
	static struct env e;

	printf("-- a closed connection withdraws its devices and ends its "
	       "requests\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_add_unit(&e, 2u, 32u, 0);
	env_go(&e, 6u);
	ct_check_eq_u32(g_units.added, 2u, "two devices exist");

	e.drop_read_data = 1;
	ct_check_eq_u32((unsigned long)mscp_cl_fsm_read(&e.cl, "VAX1$DUA1:", 0u,
							1u, e.iobuf,
							(uint32_t)sizeof(e.iobuf),
							0x55u),
			0, "a READ is in flight");
	ct_check_eq_u32(e.done.calls, 0u, "outstanding");

	mscp_cl_fsm_conn_closed(&e.cl, CL_CONID);

	ct_check_eq_u32(g_units.removed, 2u,
			"BOTH devices were withdrawn -- a disk that cannot be "
			"reached is not one this executive keeps advertising");
	ct_check_eq_u32(mscp_cl_fsm_unit_count(&e.cl), 0u, "no UCB survives");
	ct_check_eq_u32(e.done.calls, 1u, "the in-flight request was ended");
	ct_check_eq_u32(vms_mscp_status_major(e.done.status),
			(unsigned)VMS_MSCP_ST_ABORTED,
			"with a real Command Aborted, not a hang");
	ct_check_eq_u32(e.cl.cddb[0].in_use, 0u, "and the CDDB is gone");
}

/* ------------------------------------------------------------------ *
 * 9.8 An end message that matches nothing outstanding is never applied
 * ------------------------------------------------------------------ */
static void test_unmatched_end_is_not_applied(void)
{
	static struct env e;
	uint8_t body[VMS_MSCP_END_BODY_MAX];
	uint8_t frame[VMS_OFF_SYSAP_BODY + VMS_MSCP_END_BODY_MAX];
	struct vms_mscp_xfer_end end;
	uint32_t len = 0u;

	printf("-- an end message whose P.CRF matches nothing is DROPPED\n");
	env_start(&e, 6u);
	env_add_unit(&e, 1u, 32u, 0);
	env_go(&e, 6u);

	memset(&end, 0, sizeof(end));
	memset(frame, 0, sizeof(frame));
	end.eh.hdr.cmd_ref = 0xdeadbeefu;   /* no request carries this */
	end.eh.hdr.unit = 1u;
	end.eh.hdr.opcode = VMS_MSCP_OP_READ;
	end.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0u);
	end.byte_count = 512u;
	ct_check_eq_u32((unsigned long)vms_mscp_read_end_build(&end, frame,
							       (uint32_t)sizeof(frame),
							       &len),
			VMS_CODEC_OK, "the stimulus builds through the codec");
	memcpy(body, frame + VMS_OFF_SYSAP_BODY, len);

	ct_check_eq_u32((unsigned long)mscp_cl_fsm_end_msg(&e.cl, CL_CONID,
							   body, len),
			0, "the driver took the frame");
	ct_check_eq_u32(e.cl.ends_unmatched, 1u, "...and COUNTED the mismatch");
	ct_check_eq_u32(e.done.calls, 0u, "nothing was completed by it");
}

/* ------------------------------------------------------------------ *
 * 9.9 The device NAME, as a pure derivation
 * ------------------------------------------------------------------ */
static void test_unit_naming(void)
{
	char out[MSCP_CL_NAME_MAX];
	static const uint8_t node[6] = { 'V', 'A', 'X', '1', ' ', ' ' };
	static const uint8_t blank[6] = { ' ', ' ', ' ', ' ', ' ', ' ' };

	printf("-- the served device's name is a pure derivation of real "
	       "values\n");

	ct_check_eq_u32((unsigned long)mscp_cl_unit_name(node, 6u, 0u, 0, 0u,
							 out, sizeof(out)),
			0, "a node-qualified name composes");
	ct_check(strcmp(out, "VAX1$DUA0:") == 0,
		 "trailing blanks are not part of the node name");

	ct_check_eq_u32((unsigned long)mscp_cl_unit_name(node, 6u, 0u, 0, 255u,
							 out, sizeof(out)),
			0, "so does a three-digit unit");
	ct_check(strcmp(out, "VAX1$DUA255:") == 0, "...spelled in decimal");

	/* The `$n$` spelling design P7 names, reachable the day a grounded
	 * transport for the peer's ALLOCLASS lands. */
	ct_check_eq_u32((unsigned long)mscp_cl_unit_name(NULL, 0u, 2u, 1, 0u,
							 out, sizeof(out)),
			0, "the allocation-class form composes without a node");
	ct_check(strcmp(out, "$2$DUA0:") == 0, "...as design P7 spells it");

	ct_check(mscp_cl_unit_name(blank, 6u, 0u, 0, 0u, out,
				   sizeof(out)) != 0,
		 "an all-blank SCSNODE is NO name (and so no device)");
	ct_check(out[0] == '\0', "...and leaves nothing behind");
	ct_check(mscp_cl_unit_name(NULL, 0u, 0u, 0, 0u, out, sizeof(out)) != 0,
		 "neither is no node at all");
	ct_check(mscp_cl_unit_name(node, 6u, 0u, 0, 0u, out, 6u) != 0,
		 "a name that will not fit is refused, never truncated");
}

/* ------------------------------------------------------------------ *
 * 9.10 The empty [state][event] cell is COUNTED, not guessed
 * ------------------------------------------------------------------ */
static void test_empty_cell_is_counted(void)
{
	static struct env e;
	uint8_t frame[VMS_OFF_SYSAP_BODY + VMS_MSCP_END_BODY_MAX];
	struct vms_mscp_xfer_end end;
	uint32_t len = 0u;

	printf("-- an event a state has no edge for is counted, not guessed\n");
	env_start(&e, 6u);
	/* No units and no walk: the CDDB stays in DISCOVER, where a READ end
	 * has no cell (this driver has issued no transfer). */
	(void)mscp_srv_fsm_refresh_units(&e.srv);
	mscp_srv_fsm_conn_open(&e.srv, SRV_CONID, (vms_scs_sysid_t)OVMX_SYSID);
	e.drop_ends = 1;
	(void)mscp_cl_fsm_conn_open(&e.cl, CL_CONID,
				    (vms_scs_sysid_t)VAX1_SYSID, vax1_scsnode,
				    6u);
	e.drop_ends = 0;
	ct_check_eq_u32(e.cl.cddb[0].state, MSCP_CL_ST_DISCOVER,
			"the controller is still discovering");

	memset(&end, 0, sizeof(end));
	memset(frame, 0, sizeof(frame));
	end.eh.hdr.cmd_ref = 0x11223344u;
	end.eh.hdr.opcode = VMS_MSCP_OP_READ;
	(void)vms_mscp_read_end_build(&end, frame, (uint32_t)sizeof(frame),
				      &len);

	ct_check_eq_u32((unsigned long)mscp_cl_fsm_end_msg(&e.cl, CL_CONID,
							   frame + VMS_OFF_SYSAP_BODY,
							   len),
			0, "the frame was taken");
	ct_check_eq_u32(e.cl.ignored_events, 1u,
			"and the empty cell was COUNTED");
	ct_check_eq_u32(e.done.calls, 0u, "nothing acted on it");
}

/* ------------------------------------------------------------------ *
 * 9.11 The GLUE, source-scanned (the two-proof shape test_cnxman_glue.c
 * established for glue that names exec_kbackend.h and cannot be host-linked)
 * ------------------------------------------------------------------ */
static char *slurp(const char *path)
{
	static char buf[262144];
	FILE *fp = fopen(path, "rb");
	size_t n;

	if (fp == NULL)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

static void test_glue_source(void)
{
	char *s = slurp(OVMX_KCORE_DIR "/vms_mscp_cl.c");

	printf("-- the shipping glue binds the driver to the REAL executive\n");
	ct_check(s != NULL, "src/kernel-core/vms_mscp_cl.c is readable");
	if (s == NULL)
		return;

	ct_check(strstr(s, "scs_send_msg(") != NULL,
		 "send_cmd -> scs_send_msg (a real credit-spending SCS send)");
	ct_check(strstr(s, "pe_buf_register(") != NULL,
		 "buf_register -> the port's THIRD service");
	ct_check(strstr(s, "pe_buf_release(") != NULL, "...and its release");
	ct_check(strstr(s, "vms_devtab_add_served_disk(") != NULL,
		 "unit_ready -> a REAL vms_devtab row");
	ct_check(strstr(s, "vms_devtab_remove_served_disk(") != NULL,
		 "unit_gone -> the row really goes away");
	ct_check(strstr(s, "vms_scs_set_block_consumer(") != NULL,
		 "and it registers as a block-transfer consumer");
	ct_check(strstr(s, "cnxman_disk_client_connect(") != NULL,
		 "connections are opened through CNXMAN's ONE "
		 "`VMS$DISK_CL_DRVR` registration");
	ct_check(strstr(s, "cnxman_get_csb(") != NULL,
		 "and the peer's SCSNODE is READ off its CSB, never composed");
	ct_check(strstr(s, "cf_timer_arm(") != NULL,
		 "the beat is FC-P0.5's timer, never a substrate one");

	/* The negative half: the glue must NOT be where MSCP is decided. */
	ct_check(strstr(s, "VMS_MSCP_STATUS(") == NULL,
		 "the glue composes NO MSCP status -- the pure driver does");
	ct_check(strstr(s, "_cmd_build") == NULL, "and it builds no command");
	ct_check(strstr(s, "$DUA") == NULL,
		 "and it spells NO device name -- the pure driver derives it "
		 "from values this file read out of the executive");
}

/* ------------------------------------------------------------------ *
 * 9.12 The devtab row: served, and with NO local backing
 * ------------------------------------------------------------------ */
static void test_devtab_served_row_source(void)
{
	char *s = slurp(OVMX_KCORE_DIR "/vms_devtab.c");

	printf("-- the served device row is MSCP-served and has no backing\n");
	ct_check(s != NULL, "src/kernel-core/vms_devtab.c is readable");
	if (s == NULL)
		return;

	ct_check(strstr(s, "vms_devtab_add_served_disk") != NULL,
		 "the served-disk entry point exists");
	ct_check(strstr(s, "disk->mscp_served = 1;") != NULL,
		 "and it sets the row's mscp_served -- what DVI$_MSCP_SERVED "
		 "reads");
	ct_check(strstr(s, "info->mscp_served = dev->mscp_served;") != NULL,
		 "and $GETDVI's projection reads it off the row");
	/* No backing: the served path must never call the local block seam. */
	ct_check(strstr(s, "vms_devtab_add_served_disk(const char *devnam)") !=
			 NULL,
		 "the entry point takes ONLY a name -- there is no local "
		 "(major, minor) for a disk whose bytes are on another node");
}

int main(void)
{
	printf("test_mscp_cl: FC-P7.1, the executive-resident MSCP disk class "
	       "driver\n");
	fake_vc_toolkit_unused();
	test_discovery_makes_devices();
	test_no_scsnode_means_no_device();
	test_read_end_to_end();
	test_short_read_is_refused();
	test_controller_timeout();
	test_write_as_far_as_grounded();
	test_close_withdraws_devices();
	test_unmatched_end_is_not_applied();
	test_unit_naming();
	test_empty_cell_is_counted();
	test_glue_source();
	test_devtab_served_row_source();
	return ct_summary("test_mscp_cl");
}
