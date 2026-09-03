/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scs_test_harness.h - the R1 rig for the SCS FSM (FC-P2.2).
 *
 * Two `struct scs_fsm` instances wired to each other through a DELIVERY QUEUE,
 * not a direct call. The queue is the point: in the executive the peer's reply
 * arrives as a later event on the one fork thread, never as a re-entry into the
 * handler that is still mid-send. A harness that called straight through would
 * deliver the op-1 echo while the initiator was still inside h_local_connect
 * and had not yet reached CONNECT_SENT -- a reentrancy the real system cannot
 * have, and a test artifact that would hide real edges. So: send records and
 * enqueues, and scsh_pump() drains at top level.
 *
 * The harness also plays the PORT: it answers ops->addr with the four
 * addresses a circuit would supply, and it re-assembles the body-level
 * application message into the 204-byte frame the port would put on the wire
 * (abs 0-55 the port's, the caller's body spliced at abs 56 -- exactly what
 * pe_vc_send_msg does). Nothing here fabricates an SCS field: every byte the
 * FSM asserts is built by the FSM through the codec.
 */
#ifndef OVMX_SCS_TEST_HARNESS_H
#define OVMX_SCS_TEST_HARNESS_H

#include <stdint.h>
#include <stddef.h>

#include "vms_scs_fsm.h"

#define SCSH_CDL      8u
#define SCSH_SBS      4u
#define SCSH_SW       4u
#define SCSH_QUEUE   64u
#define SCSH_TRACE   64u
#define SCSH_FRAME  256u

/* Not every test uses every helper; a header of shared statics would
 * otherwise red -Werror=unused-function in whichever TU skips one. */
#define SCSH_UNUSED __attribute__((unused))

/* Where the body-level buffer lands in the frame the PORT assembles: the
 * port's own abs 0-55 span comes first (design SS3.2.4). Derived from the two
 * public geometry constants, never restated. */
#define SCSH_BODY_ABS_OFF (SCS_MSG_FRAME_LEN - SCS_MSG_BODY_LEN)   /* 56 */

struct scsh_pkt {
	uint8_t         used;
	uint8_t         frame[SCSH_FRAME];
	uint32_t        len;
	vms_scs_sysid_t from;
	vms_conid_t     dst_conid;
};

struct scsh_node;

/* A SYSAP whose every callback is counted, so a test asserts on what the
 * SYSAP was actually told rather than on a frame count. */
struct scsh_sysap {
	struct scs_sysap_ops ops;
	struct scsh_node    *node;
	int                  connect_decision; /* 0 accept, DEFER, else reject */
	uint32_t             n_connect_req;
	uint32_t             n_opened;
	uint32_t             n_message;
	uint32_t             n_closed;
	uint32_t             n_send_failed;
	uint32_t             last_close_reason;
	uint32_t             last_send_failed_reason;
	vms_conid_t          last_listen_conid;
	vms_conid_t          last_opened_conid;
	vms_conid_t          last_closed_conid;
	uint8_t              last_msg[SCS_SYSAP_BODY_LEN];
	uint16_t             return_credit_immediately; /* 1 = release on rx */
};

struct scsh_node {
	struct scs_fsm      fsm;
	struct scs_cdt      cdl[SCSH_CDL];
	struct scs_sb       sbs[SCSH_SBS];
	struct scs_sendwait sw[SCSH_SW];
	struct scs_fsm_ops  ops;
	vms_scs_sysid_t     sysid;
	struct scsh_node   *peer;

	/* injected failures, each a countdown */
	int                 fail_ctrl;
	int                 fail_msg;
	int                 fail_addr;
	int                 drop_tx;      /* build+record, deliver nothing */

	/* what this node PUT ON THE WIRE, in order */
	uint16_t            tx_op[SCSH_TRACE];
	uint16_t            tx_credit[SCSH_TRACE];
	uint32_t            n_tx;

	/* timers */
	uint32_t            armed[SCS_TIMER__COUNT];
	uint32_t            cancelled[SCS_TIMER__COUNT];
	uint32_t            last_key[SCS_TIMER__COUNT];

	struct scsh_sysap  *sysap;
	uint32_t            now_ms;
};

/* One shared inbound queue: every packet carries its destination node. */
struct scsh_wire {
	struct scsh_pkt  q[SCSH_QUEUE];
	struct scsh_node *dst[SCSH_QUEUE];
	uint32_t          head;
	uint32_t          tail;
	uint32_t          dropped;
};

static struct scsh_wire scsh_wire;

SCSH_UNUSED static void scsh_trace(struct scsh_node *n, uint16_t op, uint16_t credit)
{
	if (n->n_tx < SCSH_TRACE) {
		n->tx_op[n->n_tx] = op;
		n->tx_credit[n->n_tx] = credit;
	}
	n->n_tx++;
}

SCSH_UNUSED static void scsh_enqueue(struct scsh_node *dst, vms_scs_sysid_t from,
			 vms_conid_t dst_conid, const uint8_t *frame,
			 uint32_t len)
{
	uint32_t i, k;

	if (dst == (struct scsh_node *)0 || len > SCSH_FRAME) {
		scsh_wire.dropped++;
		return;
	}
	i = scsh_wire.tail % SCSH_QUEUE;
	if (scsh_wire.q[i].used) {
		scsh_wire.dropped++;
		return;
	}
	scsh_wire.q[i].used = 1u;
	scsh_wire.q[i].len = len;
	scsh_wire.q[i].from = from;
	scsh_wire.q[i].dst_conid = dst_conid;
	for (k = 0; k < len; k++)
		scsh_wire.q[i].frame[k] = frame[k];
	scsh_wire.dst[i] = dst;
	scsh_wire.tail++;
}

/* ---- the injected ops ---------------------------------------------------- */

/* Read the verb and credit BACK OFF THE BUILT FRAME through the codec, so the
 * trace records what the wire would carry, never what the caller intended. */
SCSH_UNUSED static void scsh_record_ctrl(struct scsh_node *n, const uint8_t *frame,
			     uint32_t len)
{
	struct vms_scs_hdr h;

	if (vms_scs_hdr_parse_frame(frame, len, &h) == VMS_CODEC_OK)
		scsh_trace(n, h.mtype, h.credit);
	else
		scsh_trace(n, 0xffffu, 0xffffu);
}

SCSH_UNUSED static int scsh_send_ctrl(void *ctx, vms_scs_sysid_t dst,
			  const uint8_t *frame, uint32_t len)
{
	struct scsh_node *n = (struct scsh_node *)ctx;

	if (n->fail_ctrl > 0) {
		n->fail_ctrl--;
		return -1;
	}
	scsh_record_ctrl(n, frame, len);
	if (!n->drop_tx && n->peer != (struct scsh_node *)0 &&
	    n->peer->sysid == dst)
		scsh_enqueue(n->peer, n->sysid, 0u, frame, len);
	return 0;
}

/*
 * The PORT's half of an application message: build abs 0-55 (here: zeros, as
 * pe_vc_send_msg itself leaves abs 36-55, plus the addressing the real port
 * fills) and splice the caller's body at abs 56. The SCS header the FSM built
 * is copied through untouched -- that is what makes the receiving node's
 * credit read a read of what this node actually asserted.
 */
SCSH_UNUSED static int scsh_send_msg(void *ctx, vms_scs_sysid_t dst, vms_conid_t dst_conid,
			 const uint8_t *body, uint32_t len)
{
	struct scsh_node *n = (struct scsh_node *)ctx;
	uint8_t frame[SCS_MSG_FRAME_LEN];
	struct vms_scs_hdr h;
	uint32_t i;

	if (n->fail_msg > 0) {
		n->fail_msg--;
		return -1;
	}
	if (len != SCS_MSG_BODY_LEN)
		return -1;
	for (i = 0; i < SCS_MSG_FRAME_LEN; i++)
		frame[i] = 0u;
	for (i = 0; i < len; i++)
		frame[SCSH_BODY_ABS_OFF + i] = body[i];

	if (vms_scs_hdr_parse_frame(frame, SCS_MSG_FRAME_LEN, &h) ==
	    VMS_CODEC_OK)
		scsh_trace(n, h.mtype, h.credit);
	else
		scsh_trace(n, 0xffffu, 0xffffu);

	if (!n->drop_tx && n->peer != (struct scsh_node *)0 &&
	    n->peer->sysid == dst)
		scsh_enqueue(n->peer, n->sysid, dst_conid, frame,
			     SCS_MSG_FRAME_LEN);
	return 0;
}

SCSH_UNUSED static int scsh_addr(void *ctx, vms_scs_sysid_t dst, struct vms_scs_addr *out)
{
	struct scsh_node *n = (struct scsh_node *)ctx;
	uint32_t i;

	if (n->fail_addr > 0) {
		n->fail_addr--;
		return -1;
	}
	for (i = 0; i < VMS_ETH_ADDR_LEN; i++) {
		out->dst_mac[i] = (uint8_t)(0x08u + i);
		out->src_mac[i] = (uint8_t)(0x10u + i);
		out->dst_logical[i] = (uint8_t)(dst & 0xffu);
		out->src_logical[i] = (uint8_t)(n->sysid & 0xffu);
	}
	return 0;
}

SCSH_UNUSED static void scsh_arm(void *ctx, enum scs_timer which, uint32_t key, uint32_t ms)
{
	struct scsh_node *n = (struct scsh_node *)ctx;

	(void)ms;
	n->armed[which]++;
	n->last_key[which] = key;
}

SCSH_UNUSED static void scsh_cancel(void *ctx, enum scs_timer which, uint32_t key)
{
	struct scsh_node *n = (struct scsh_node *)ctx;

	(void)key;
	n->cancelled[which]++;
}

SCSH_UNUSED static uint32_t scsh_now(void *ctx)
{
	return ((struct scsh_node *)ctx)->now_ms;
}

SCSH_UNUSED static void scsh_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

/* ---- the SYSAP ----------------------------------------------------------- */

SCSH_UNUSED static int scsh_connect_req(void *ctx, vms_conid_t local_conid,
			    vms_scs_sysid_t peer, vms_conid_t peer_conid,
			    const uint8_t *conndata, uint32_t conndata_len)
{
	struct scsh_sysap *s = (struct scsh_sysap *)ctx;

	(void)peer;
	(void)peer_conid;
	(void)conndata;
	(void)conndata_len;
	s->n_connect_req++;
	s->last_listen_conid = local_conid;
	return s->connect_decision;
}

SCSH_UNUSED static void scsh_opened(void *ctx, vms_conid_t local_conid)
{
	struct scsh_sysap *s = (struct scsh_sysap *)ctx;

	s->n_opened++;
	s->last_opened_conid = local_conid;
}

SCSH_UNUSED static int scsh_message(void *ctx, vms_conid_t local_conid,
			const uint8_t *body, uint32_t len)
{
	struct scsh_sysap *s = (struct scsh_sysap *)ctx;
	uint32_t i;

	s->n_message++;
	for (i = 0; i < len && i < SCS_SYSAP_BODY_LEN; i++)
		s->last_msg[i] = body[i];
	if (s->return_credit_immediately)
		(void)scs_fsm_return_credit(&s->node->fsm, local_conid, 1u);
	return 0;
}

SCSH_UNUSED static void scsh_closed(void *ctx, vms_conid_t local_conid, uint32_t reason)
{
	struct scsh_sysap *s = (struct scsh_sysap *)ctx;

	s->n_closed++;
	s->last_closed_conid = local_conid;
	s->last_close_reason = reason;
}

SCSH_UNUSED static void scsh_send_failed(void *ctx, vms_conid_t local_conid,
			     uint32_t reason)
{
	struct scsh_sysap *s = (struct scsh_sysap *)ctx;

	(void)local_conid;
	s->n_send_failed++;
	s->last_send_failed_reason = reason;
}

SCSH_UNUSED static void scsh_sysap_init(struct scsh_sysap *s, struct scsh_node *n)
{
	uint32_t i;
	uint8_t *b = (uint8_t *)s;

	for (i = 0; i < (uint32_t)sizeof(*s); i++)
		b[i] = 0u;
	s->node = n;
	s->ops.connect_req = scsh_connect_req;
	s->ops.opened = scsh_opened;
	s->ops.message = scsh_message;
	s->ops.closed = scsh_closed;
	s->ops.send_failed = scsh_send_failed;
	s->ops.ctx = s;
	n->sysap = s;
}

/* ---- node lifecycle ------------------------------------------------------ */

SCSH_UNUSED static void scsh_node_init(struct scsh_node *n, vms_scs_sysid_t sysid,
			   uint16_t conid_seed)
{
	uint32_t i;
	uint8_t *b = (uint8_t *)n;

	for (i = 0; i < (uint32_t)sizeof(*n); i++)
		b[i] = 0u;
	n->sysid = sysid;
	n->ops.send_ctrl = scsh_send_ctrl;
	n->ops.send_msg = scsh_send_msg;
	n->ops.addr = scsh_addr;
	n->ops.arm_timer = scsh_arm;
	n->ops.cancel_timer = scsh_cancel;
	n->ops.now_ms = scsh_now;
	n->ops.log = scsh_log;
	n->ops.ctx = n;

	(void)scs_fsm_init(&n->fsm, &n->ops);
	(void)scs_fsm_bind_cdl(&n->fsm, n->cdl, SCSH_CDL);
	(void)scs_fsm_bind_sbs(&n->fsm, n->sbs, SCSH_SBS);
	(void)scs_fsm_bind_sendwait(&n->fsm, n->sw, SCSH_SW);
	scs_fsm_seed_conid(&n->fsm, conid_seed);
}

SCSH_UNUSED static void scsh_wire_reset(void)
{
	uint32_t i;
	uint8_t *b = (uint8_t *)&scsh_wire;

	for (i = 0; i < (uint32_t)sizeof(scsh_wire); i++)
		b[i] = 0u;
}

/* Drain the wire until it is empty (bounded, so a delivery storm fails the
 * test instead of hanging it). */
SCSH_UNUSED static uint32_t scsh_pump(void)
{
	uint32_t delivered = 0u;
	uint32_t guard = 0u;

	while (scsh_wire.head != scsh_wire.tail && guard++ < SCSH_QUEUE * 4u) {
		uint32_t i = scsh_wire.head % SCSH_QUEUE;
		struct scsh_node *dst = scsh_wire.dst[i];
		uint8_t frame[SCSH_FRAME];
		uint32_t len = scsh_wire.q[i].len;
		vms_scs_sysid_t from = scsh_wire.q[i].from;
		vms_conid_t conid = scsh_wire.q[i].dst_conid;
		uint32_t k;

		for (k = 0; k < len; k++)
			frame[k] = scsh_wire.q[i].frame[k];
		scsh_wire.q[i].used = 0u;
		scsh_wire.head++;
		scs_fsm_rx_message(&dst->fsm, from, conid, frame, len);
		delivered++;
	}
	return delivered;
}

/* ---- direct injection: a peer frame built through the codec ---------------
 *
 * For the edges a cooperating peer would never produce (a REJECT of our
 * connect, a second connect while the SYSAP is deciding, a stale Con.ID).
 * The frame is BUILT BY THE CODEC, so an injected frame is byte-shaped
 * exactly like one a real node would send -- the test cannot accidentally
 * feed the FSM something no wire could carry.
 */
SCSH_UNUSED static uint16_t scsh_ctrl_content_for_op(uint16_t op)
{
	switch (op) {
	case SCS_MTYPE_CON_REQ:
	case SCS_MTYPE_ACCP_REQ:
		return VMS_SCSCTRL_LEN_CONNECT;
	case SCS_MTYPE_CON_RSP:
		return VMS_SCSCTRL_LEN_ECHO;
	case SCS_MTYPE_ACCP_RSP:
	case SCS_MTYPE_REJ_REQ:
	case SCS_MTYPE_DISC_REQ:
		return VMS_SCSCTRL_LEN_MARKER;
	default:
		return VMS_SCSCTRL_LEN_SHORT;
	}
}

SCSH_UNUSED static void scsh_inject_ctrl(struct scsh_node *dst, vms_scs_sysid_t from,
			     uint16_t op, vms_conid_t to_conid,
			     vms_conid_t from_conid, uint16_t credit,
			     const uint8_t *name1, const uint8_t *name2)
{
	struct vms_scs_ctrl_frame c;
	uint8_t frame[SCSH_FRAME];
	uint32_t written = 0u;
	uint16_t content = scsh_ctrl_content_for_op(op);
	uint32_t i;
	uint8_t *p = (uint8_t *)&c;

	for (i = 0; i < (uint32_t)sizeof(c); i++)
		p[i] = 0u;
	for (i = 0; i < VMS_ETH_ADDR_LEN; i++) {
		c.hdr.eth_dst[i] = (uint8_t)(0x10u + i);
		c.hdr.eth_src[i] = (uint8_t)(0x08u + i);
		c.hdr.dst_lavc[i] = (uint8_t)(dst->sysid & 0xffu);
		c.hdr.src_lavc[i] = (uint8_t)(from & 0xffu);
	}
	c.hdr.connect_flag = VMS_SCSCTRL_CONNECT_FLAG;
	c.hdr.word30 = (uint16_t)(VMS_SCS_MT_SETUP |
				  ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	c.hdr.sca_len_field = (uint16_t)(content - 2u);
	c.inner_len = (uint16_t)(content - SCS_INNERLEN_BIAS);
	c.op = op;
	c.credit = credit;
	c.conid_remote = to_conid;
	c.conid_local = from_conid;
	c.has_marker = (uint8_t)(content >= VMS_SCSCTRL_LEN_MARKER);
	c.has_tail4 = (uint8_t)(content == VMS_SCSCTRL_LEN_ECHO);
	c.has_names = (uint8_t)(content == VMS_SCSCTRL_LEN_CONNECT);
	c.has_blank = c.has_names;
	if (c.has_names) {
		for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++) {
			c.name1[i] = name1 != (const uint8_t *)0 ? name1[i] : 0u;
			c.name2[i] = name2 != (const uint8_t *)0 ? name2[i] : 0u;
		}
	}
	if (vms_scs_ctrl_build(&c, frame, (uint32_t)sizeof(frame), &written) !=
	    VMS_CODEC_OK)
		return;
	scs_fsm_rx_message(&dst->fsm, from, to_conid, frame, written);
}

/* An application message from a peer, carrying a real credit value. */
SCSH_UNUSED static void scsh_inject_msg(struct scsh_node *dst, vms_scs_sysid_t from,
			    vms_conid_t to_conid, vms_conid_t from_conid,
			    uint16_t credit, uint8_t fill)
{
	struct vms_scs_hdr h;
	uint8_t body[SCS_SYSAP_BODY_LEN];
	uint8_t buf[SCS_MSG_BODY_LEN];
	uint8_t frame[SCS_MSG_FRAME_LEN];
	uint32_t i;

	for (i = 0; i < SCS_SYSAP_BODY_LEN; i++)
		body[i] = fill;
	h.inner_len = SCS_MSG_INNER_LEN;
	h.mtype = (uint16_t)SCS_MTYPE_APPL_MSG;
	h.credit = credit;
	h.conid_remote = to_conid;
	h.conid_local = from_conid;
	if (vms_scs_msg_body_build(&h, body, SCS_SYSAP_BODY_LEN, buf,
				   (uint32_t)sizeof(buf)) != VMS_CODEC_OK)
		return;
	for (i = 0; i < SCS_MSG_FRAME_LEN; i++)
		frame[i] = 0u;
	for (i = 0; i < SCS_MSG_BODY_LEN; i++)
		frame[SCSH_BODY_ABS_OFF + i] = buf[i];
	scs_fsm_rx_message(&dst->fsm, from, to_conid, frame,
			   SCS_MSG_FRAME_LEN);
}

/* Two nodes, wired to each other, one SYSAP registered on each. */
SCSH_UNUSED static const uint8_t scsh_name_a[VMS_SCS_PROCNAME_LEN] =
	{ 'S','C','S','$','D','I','R','E','C','T','O','R','Y',' ',' ',' ' };
SCSH_UNUSED static const uint8_t scsh_name_b[VMS_SCS_PROCNAME_LEN] =
	{ 'V','M','S','$','V','A','X','c','l','u','s','t','e','r',' ',' ' };

SCSH_UNUSED static void scsh_link(struct scsh_node *a, struct scsh_node *b)
{
	a->peer = b;
	b->peer = a;
	scs_fsm_vc_up(&a->fsm, b->sysid);
	scs_fsm_vc_up(&b->fsm, a->sysid);
}

/* How many times `op` appears in a node's transmit trace. */
SCSH_UNUSED static uint32_t scsh_count_op(const struct scsh_node *n, uint16_t op)
{
	uint32_t i, c = 0u;
	uint32_t lim = n->n_tx < SCSH_TRACE ? n->n_tx : SCSH_TRACE;

	for (i = 0; i < lim; i++) {
		if (n->tx_op[i] == op)
			c++;
	}
	return c;
}

/* The index of the FIRST `op` in the trace, or -1. */
SCSH_UNUSED static int scsh_first_op(const struct scsh_node *n, uint16_t op)
{
	uint32_t i;
	uint32_t lim = n->n_tx < SCSH_TRACE ? n->n_tx : SCSH_TRACE;

	for (i = 0; i < lim; i++) {
		if (n->tx_op[i] == op)
			return (int)i;
	}
	return -1;
}

SCSH_UNUSED static struct scs_cdt *scsh_cdt(struct scsh_node *n, vms_conid_t conid)
{
	return scs_fsm_cdt_by_conid(&n->fsm, conid);
}

SCSH_UNUSED static int scsh_state(struct scsh_node *n, vms_conid_t conid)
{
	struct scs_cdt *c = scsh_cdt(n, conid);

	return c == (struct scs_cdt *)0 ? -1 : (int)c->state;
}

/* The conservation invariant of vms_scs_fsm.h SS5, checked on one CDT. */
SCSH_UNUSED static int scsh_ledger_balanced(const struct scs_cdt *c)
{
	return (uint32_t)c->credit_receive + (uint32_t)c->credit_held +
	       (uint32_t)c->credit_pending == (uint32_t)c->credit_grant;
}

/* Open a connection A -> B and drive it to OPEN on both ends. */
SCSH_UNUSED static int scsh_open_pair(struct scsh_node *a, struct scsh_node *b,
			  uint16_t a_credits, vms_conid_t *a_conid)
{
	struct scs_connect_args args;
	uint32_t i;
	uint8_t *p = (uint8_t *)&args;

	for (i = 0; i < (uint32_t)sizeof(args); i++)
		p[i] = 0u;
	args.local_name = scsh_name_a;
	args.remote_name = scsh_name_b;
	args.sysap = &a->sysap->ops;
	args.dst = b->sysid;
	args.initial_credits = a_credits;

	if (scs_fsm_connect(&a->fsm, &args, a_conid) != SCS_OK)
		return -1;
	(void)scsh_pump();
	return 0;
}

#endif /* OVMX_SCS_TEST_HARNESS_H */
