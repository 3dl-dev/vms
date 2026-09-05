/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scs_dir_test_harness.h - the R1 rig for the SCS Directory Service (FC-P2.3).
 *
 * It is FC-P2.2's two-node scs_test_harness.h with one addition per node: a
 * `struct scs_dir` bound to that node's `struct scs_fsm` through the ops table
 * FC-P2.4's glue will bind in the executive. That binding is the ONLY new
 * code, and it is deliberately trivial -- every entry is a one-line dereference
 * into a scs_fsm_* service, so a test failure is a failure of the directory or
 * the registry, never of the rig.
 *
 * IT ALSO TAPS THE WIRE. scsdh_send_ctrl records the WHOLE frame before
 * handing it to the P2.2 harness, so a test can read a directory answer back
 * off the bytes that went out -- through the codec, at the body level the
 * SYSAP itself works in -- rather than trusting what the sender intended.
 */
#ifndef OVMX_SCS_DIR_TEST_HARNESS_H
#define OVMX_SCS_DIR_TEST_HARNESS_H

#include "scs_test_harness.h"
#include "vms_scs_dir.h"

#define SCSDH_PEERS      4u
#define SCSDH_INQUIRIES  8u
#define SCSDH_TAP       32u

/* One captured frame: the bytes, plus a GLOBAL arrival ordinal so a test can
 * reconstruct the order the two nodes' frames actually interleaved in -- which
 * is what a capture shows and what a per-node trace cannot. */
struct scsdh_tap {
	uint8_t  frame[SCSH_FRAME];
	uint32_t len;
	uint32_t seq;
};

static uint32_t scsdh_tap_seq;

struct scsdh_node {
	struct scsh_node        n;      /* the FC-P2.2 rig, unchanged        */
	struct scs_dir          dir;
	struct scs_dir_ops      dir_ops;
	struct scs_dir_peer     peers[SCSDH_PEERS];
	struct scs_dir_inquiry  inq[SCSDH_INQUIRIES];

	struct scsdh_tap        tap[SCSDH_TAP];
	uint32_t                n_tap;

	/* what this node's own lookups were told */
	uint32_t                n_results;
	uint8_t                 last_name[VMS_SCS_PROCNAME_LEN];
	int                     last_present;
	int                     results_present[SCSDH_INQUIRIES];
	uint8_t                 results_name[SCSDH_INQUIRIES]
					   [VMS_SCS_PROCNAME_LEN];
};

/* ---- the glue FC-P2.4 will write, in one place ---------------------------- */

SCSH_UNUSED static int scsdh_sysap_lookup(void *ctx, const uint8_t *name,
			      struct scs_sysap_info *out)
{
	struct scsdh_node *d = (struct scsdh_node *)ctx;

	return scs_fsm_sysap_lookup(&d->n.fsm, name, out);
}

SCSH_UNUSED static int scsdh_connect(void *ctx, vms_scs_sysid_t dst,
			 const struct scs_sysap_ops *sysap,
			 uint16_t initial_credits, vms_conid_t *out_conid)
{
	struct scsdh_node *d = (struct scsdh_node *)ctx;
	struct scs_connect_args args;
	uint32_t i;
	uint8_t *p = (uint8_t *)&args;

	for (i = 0; i < (uint32_t)sizeof(args); i++)
		p[i] = 0u;
	/* p. 2-51's own name pair: the Process Poller connects TO the
	 * Directory Service. */
	args.local_name = scs_dir_name_lookup;
	args.remote_name = scs_dir_name_directory;
	args.sysap = sysap;
	args.dst = dst;
	args.initial_credits = initial_credits;
	return scs_fsm_connect(&d->n.fsm, &args, out_conid);
}

SCSH_UNUSED static int scsdh_send(void *ctx, vms_conid_t conid, const uint8_t *body,
		      uint32_t len)
{
	struct scsdh_node *d = (struct scsdh_node *)ctx;

	return scs_fsm_send_msg(&d->n.fsm, conid, body, len);
}

SCSH_UNUSED static int scsdh_return_credit(void *ctx, vms_conid_t conid, uint16_t n)
{
	struct scsdh_node *d = (struct scsdh_node *)ctx;

	return scs_fsm_return_credit(&d->n.fsm, conid, n);
}

SCSH_UNUSED static int scsdh_disconnect(void *ctx, vms_conid_t conid)
{
	struct scsdh_node *d = (struct scsdh_node *)ctx;

	return scs_fsm_disconnect(&d->n.fsm, conid);
}

SCSH_UNUSED static uint32_t scsdh_now(void *ctx)
{
	return ((struct scsdh_node *)ctx)->n.now_ms;
}

SCSH_UNUSED static void scsdh_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

/* ---- the wire tap -------------------------------------------------------- */

SCSH_UNUSED static int scsdh_send_ctrl(void *ctx, vms_scs_sysid_t dst,
			   const uint8_t *frame, uint32_t len)
{
	struct scsh_node *n = (struct scsh_node *)ctx;
	struct scsdh_node *d = (struct scsdh_node *)ctx;   /* n is first */
	uint32_t i;

	if (d->n_tap < SCSDH_TAP && len <= SCSH_FRAME) {
		for (i = 0; i < len; i++)
			d->tap[d->n_tap].frame[i] = frame[i];
		d->tap[d->n_tap].len = len;
		d->tap[d->n_tap].seq = scsdh_tap_seq++;
		d->n_tap++;
	}
	return scsh_send_ctrl(n, dst, frame, len);
}

/* ---- a SYSAP that exists only to BE REGISTERED ---------------------------
 *
 * `MSCP$DISK` and `VMS$VAXcluster` are other items' SYSAPs (FC-P6.x, FC-P3.x).
 * What the directory reads about them is the REGISTRATION, so a test needs a
 * real entry in the real SDIR queue and nothing more. This table makes one:
 * it is a genuine registration, not a directory-side fake.
 */
SCSH_UNUSED static int scsdh_stub_connect_req(void *ctx, vms_conid_t local_conid,
				  vms_scs_sysid_t peer, vms_conid_t peer_conid,
				  const uint8_t *conndata, uint32_t len)
{
	(void)ctx;
	(void)local_conid;
	(void)peer;
	(void)peer_conid;
	(void)conndata;
	(void)len;
	return 0;
}

SCSH_UNUSED static struct scs_sysap_ops scsdh_stub_ops = {
	scsdh_stub_connect_req,
	(void (*)(void *, vms_conid_t))0,
	(int (*)(void *, vms_conid_t, const uint8_t *, uint32_t))0,
	(void (*)(void *, vms_conid_t, uint32_t))0,
	(void (*)(void *, vms_conid_t, uint32_t))0,
	(const uint8_t *)0,   /* accept_conndata: this stub declares none */
	(void *)0
};

/* ---- lifecycle ----------------------------------------------------------- */

SCSH_UNUSED static void scsdh_node_init(struct scsdh_node *d, vms_scs_sysid_t sysid,
			    uint16_t conid_seed)
{
	uint32_t i;
	uint8_t *b = (uint8_t *)d;

	for (i = 0; i < (uint32_t)sizeof(*d); i++)
		b[i] = 0u;
	scsh_node_init(&d->n, sysid, conid_seed);
	scsdh_tap_seq = 0u;
	d->n.ops.send_ctrl = scsdh_send_ctrl;   /* the tap, above scsh's */

	d->dir_ops.sysap_lookup = scsdh_sysap_lookup;
	d->dir_ops.connect = scsdh_connect;
	d->dir_ops.send = scsdh_send;
	d->dir_ops.return_credit = scsdh_return_credit;
	d->dir_ops.disconnect = scsdh_disconnect;
	d->dir_ops.now_ms = scsdh_now;
	d->dir_ops.log = scsdh_log;
	d->dir_ops.ctx = d;

	(void)scs_dir_init(&d->dir, &d->dir_ops);
	(void)scs_dir_bind_peers(&d->dir, d->peers, SCSDH_PEERS);
	(void)scs_dir_bind_inquiries(&d->dir, d->inq, SCSDH_INQUIRIES);
}

/* Register the SCS Directory Service on this node, as the glue does at
 * CLUSTER_START. */
SCSH_UNUSED static int scsdh_listen_directory(struct scsdh_node *d)
{
	return scs_fsm_listen(&d->n.fsm, scs_dir_name_directory,
			      scs_dir_server_ops(&d->dir),
			      (uint16_t)SCS_DIR_CREDITS_DEFAULT);
}

/* Register any other SYSAP name, so a directory answer about it is a real
 * registry hit. */
SCSH_UNUSED static int scsdh_listen_name(struct scsdh_node *d, const uint8_t *name)
{
	return scs_fsm_listen(&d->n.fsm, name, &scsdh_stub_ops, 1u);
}

/* ---- the result callback ------------------------------------------------- */

SCSH_UNUSED static void scsdh_result(void *ctx, vms_scs_sysid_t from, const uint8_t *name,
			 int present)
{
	struct scsdh_node *d = (struct scsdh_node *)ctx;
	uint32_t i;

	(void)from;
	if (d->n_results < SCSDH_INQUIRIES) {
		d->results_present[d->n_results] = present;
		for (i = 0; i < VMS_SCS_PROCNAME_LEN; i++)
			d->results_name[d->n_results][i] = name[i];
	}
	for (i = 0; i < VMS_SCS_PROCNAME_LEN; i++)
		d->last_name[i] = name[i];
	d->last_present = present;
	d->n_results++;
}

/* ---- reading a tapped frame back ----------------------------------------- */

/* Decode tapped frame `i` of `d` as a directory message. Returns 0 on
 * success. Uses the SAME body-level path the SYSAP does, so the test cannot
 * read bytes the SYSAP could not. */
SCSH_UNUSED static int scsdh_tap_dir(const struct scsdh_node *d, uint32_t i,
			 struct vms_scs_dir_msg *out)
{
	const uint8_t *body = (const uint8_t *)0;
	uint32_t body_len = 0u;
	struct vms_scs_hdr h;

	if (i >= d->n_tap)
		return -1;
	if (vms_scs_hdr_parse_frame(d->tap[i].frame, d->tap[i].len, &h) !=
	    VMS_CODEC_OK)
		return -1;
	if (h.mtype != (uint16_t)SCS_MTYPE_APPL_MSG)
		return -1;
	if (vms_scs_msg_body(d->tap[i].frame, d->tap[i].len, &body,
			     &body_len) != VMS_CODEC_OK)
		return -1;
	return vms_scs_dir_msg_parse(body, body_len, out) == VMS_CODEC_OK
		       ? 0 : -1;
}

/* The op verb of tapped frame `i`, or 0xffff. */
SCSH_UNUSED static uint16_t scsdh_tap_op(const struct scsdh_node *d, uint32_t i)
{
	struct vms_scs_hdr h;

	if (i >= d->n_tap)
		return 0xffffu;
	if (vms_scs_hdr_parse_frame(d->tap[i].frame, d->tap[i].len, &h) !=
	    VMS_CODEC_OK)
		return 0xffffu;
	return h.mtype;
}

/* The SCA content length of tapped frame `i` (frame length minus the Ethernet
 * header), which is what pins the 94-content directory class. */
SCSH_UNUSED static uint32_t scsdh_tap_content(const struct scsdh_node *d, uint32_t i)
{
	if (i >= d->n_tap)
		return 0u;
	return d->tap[i].len - (uint32_t)VMS_ETH_HDR_LEN;
}

#endif /* OVMX_SCS_DIR_TEST_HARNESS_H */
