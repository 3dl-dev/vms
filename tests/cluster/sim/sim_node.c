/* SPDX-License-Identifier: GPL-2.0 */
/* sim_node.c - one simulated node: the SHIPPING pe FSM with its ops bound to
 * the virtual LAN and the virtual clock. See sim_node.h. */

#include <string.h>

#include "sim.h"
#include "sim_node.h"

#include "vms_cluster_codec.h"
#include "vms_cluster_fork.h"   /* CF_RX_BUFS_DEFAULT: the pool a real port owns */
#include "vms_cluster_codec_vc.h"

/* ------------------------------------------------------------------ *
 * The injected ops -- the ONLY way the FSM reaches this harness
 * ------------------------------------------------------------------ */

static int sim_ops_send(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct sim_node *n = (struct sim_node *)ctx;

	n->tx_calls++;
	n->tx_bytes += len;
	(void)sim_lan_xmit(&n->sim->lan, n->index, frame, len,
			   n->sim->clock.now_ms);
	/* The virtual LAN never refuses a transmit: a lost frame is LOSS, which
	 * the FSM must survive, not a transmit error, which is a different
	 * failure with a different counter (pe_fsm.tx_errors). A scenario that
	 * wants transmit errors gets its own op. */
	return 0;
}

static void sim_ops_arm(void *ctx, enum pe_timer which, uint32_t key,
			uint32_t ms)
{
	struct sim_node *n = (struct sim_node *)ctx;

	sim_clock_arm(&n->sim->clock, n->index, (uint8_t)which, key, ms);
}

static void sim_ops_cancel(void *ctx, enum pe_timer which, uint32_t key)
{
	struct sim_node *n = (struct sim_node *)ctx;

	sim_clock_cancel(&n->sim->clock, n->index, (uint8_t)which, key);
}

static uint32_t sim_ops_now_ms(void *ctx)
{
	struct sim_node *n = (struct sim_node *)ctx;

	return sim_clock_now_ms(&n->sim->clock);
}

static uint64_t sim_ops_now_vms(void *ctx)
{
	struct sim_node *n = (struct sim_node *)ctx;

	return sim_clock_now_vms(&n->sim->clock);
}

static void sim_ops_log(void *ctx, const char *msg)
{
	struct sim_node *n = (struct sim_node *)ctx;
	size_t len;

	n->logs++;
	if (msg == NULL)
		return;
	len = strlen(msg);
	if (len >= sizeof(n->last_log))
		len = sizeof(n->last_log) - 1u;
	memcpy(n->last_log, msg, len);
	n->last_log[len] = '\0';
}

/*
 * alloc/free stay NULL, exactly as the FC-P0.8/FC-P1.2 host tests leave them.
 * The pe FSM owns a fixed scratch buffer and a bound circuit table and has no
 * business allocating; a NULL function pointer is a harder guarantee of that
 * than any counter, because reaching for one crashes the run instead of
 * quietly passing it.
 */
static void sim_ops_bind(struct sim_node *n)
{
	memset(&n->ops, 0, sizeof(n->ops));
	n->ops.send = sim_ops_send;
	n->ops.arm_timer = sim_ops_arm;
	n->ops.cancel_timer = sim_ops_cancel;
	n->ops.now_ms = sim_ops_now_ms;
	n->ops.now_vms = sim_ops_now_vms;
	n->ops.log = sim_ops_log;
	n->ops.ctx = n;
}

/* ------------------------------------------------------------------ *
 * The layer above -- a recorder (see sim_node.h §2)
 * ------------------------------------------------------------------ */

/* The recorder's slot for one peer, allocated on first sight. NULL only if the
 * harness ever saw more distinct peers than the LAN has ports. */
static struct sim_upper_peer *sim_upper_slot(struct sim_upper *u,
					     vms_scs_sysid_t from)
{
	uint32_t i;

	for (i = 0; i < SIM_MAX_NODES; i++) {
		if (u->peer[i].sysid == (uint16_t)from)
			return &u->peer[i];
	}
	for (i = 0; i < SIM_MAX_NODES; i++) {
		if (u->peer[i].sysid == 0u) {
			u->peer[i].sysid = (uint16_t)from;
			return &u->peer[i];
		}
	}
	return NULL;
}

/*
 * The sequence a delivered frame really carries, read through the SAME codec
 * accessor the port reads it with. The port hands the WHOLE frame upward
 * (vms_pe.h §4), so this is a read of real wire bytes and not of anything the
 * harness stamped.
 */
static int sim_frame_send_seq(const uint8_t *frame, uint32_t len, uint16_t *out)
{
	struct vms_frame_info fi;
	uint16_t recv_ack = 0u, send_seq = 0u;

	if (frame == NULL)
		return -1;
	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_scs_seq(frame, len, &fi, &recv_ack, &send_seq) != VMS_CODEC_OK)
		return -1;
	*out = send_seq;
	return 0;
}

/* The next sequence after `s`, skipping 0 -- the port's own rule (§4(h)(3):
 * send_seq 0 means "this frame carries no sequence of its own", so it can
 * never be a message's position). */
static uint16_t sim_seq_next(uint16_t s)
{
	uint16_t n = (uint16_t)(s + 1u);

	return n == 0u ? 1u : n;
}

static void sim_up_check_order(struct sim_upper *u, vms_scs_sysid_t from,
			       const uint8_t *frame, uint32_t len)
{
	struct sim_upper_peer *p = sim_upper_slot(u, from);
	uint16_t seq = 0u;

	if (p == NULL)
		return;
	if (sim_frame_send_seq(frame, len, &seq) != 0) {
		u->seq_unreadable++;
		return;
	}
	if (!p->seen) {
		/* §4(i).A: a freshly formed circuit starts at 1 on both sides. */
		if (seq != 1u)
			u->out_of_order++;
		p->seen = 1u;
	} else if (seq != sim_seq_next(p->last_seq)) {
		u->out_of_order++;
	}
	p->last_seq = seq;
}

static void sim_up_message(void *ctx, vms_scs_sysid_t from, vms_conid_t conid,
			   const uint8_t *body, uint32_t len)
{
	struct sim_upper *u = (struct sim_upper *)ctx;

	u->messages++;
	u->last_conid = conid;
	u->last_len = len;
	u->bytes += len;
	sim_up_check_order(u, from, body, len);
}

static void sim_up_datagram(void *ctx, vms_scs_sysid_t from,
			    const uint8_t *body, uint32_t len)
{
	struct sim_upper *u = (struct sim_upper *)ctx;

	(void)from;
	(void)body;
	u->datagrams++;
	u->bytes += len;
}

static void sim_up_vc_up(void *ctx, vms_scs_sysid_t peer)
{
	(void)peer;
	((struct sim_upper *)ctx)->ups++;
}

static void sim_up_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	struct sim_upper *u = (struct sim_upper *)ctx;
	struct sim_upper_peer *p = sim_upper_slot(u, peer);

	u->downs++;
	u->last_down_reason = reason;
	/* The circuit that comes back is a NEW circuit and its sequence starts
	 * again (§4(h)(4a)); the old frontier is not evidence about it. */
	if (p != NULL) {
		p->seen = 0u;
		p->last_seq = 0u;
	}
}

static void sim_upper_bind(struct sim_node *n)
{
	n->upper_ops.message = sim_up_message;
	n->upper_ops.datagram = sim_up_datagram;
	n->upper_ops.vc_up = sim_up_vc_up;
	n->upper_ops.vc_down = sim_up_vc_down;
	n->upper_ops.ctx = &n->upper;
	pe_fsm_set_upper(&n->fsm, &n->upper_ops);
}

/* ------------------------------------------------------------------ *
 * Configuration
 * ------------------------------------------------------------------ */

/* The lab's cluster group number, and the SYSGEN CLUSTER_CREDITS the lab's
 * VAXes were measured to grant (§4(g), abs 95). Harness defaults a scenario
 * overrides; both are named so a reader sees a configured value, not a magic
 * number appearing mid-frame. */
static const uint8_t sim_default_group[6] = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };
#define SIM_DEFAULT_CREDITS 10u

/* Receive buffers a simulated port owns: FC-P0.5's OWN default, taken from the
 * header rather than copied, so a simulated node's credit ledger has exactly
 * the bank a booted one does -- and a scenario that opens enough circuits runs
 * the pool down here the same way. */
#define SIM_DEFAULT_RX_POOL_BUFS CF_RX_BUFS_DEFAULT

void sim_node_cfg_default(struct sim_node_cfg *cfg, const char *name,
			  uint16_t sysid, uint8_t index)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->name = name;
	cfg->sysid = sysid;

	/* A locally-administered unicast address (bit 1 of octet 0 set, group
	 * bit clear), distinct per node. Deliberately NOT a DEC OUI and
	 * deliberately NOT the node's cluster-LOGICAL address: spec §4(a).0's
	 * hardest-won lesson is that abs 0 and abs 16 are different addresses,
	 * and a harness whose hardware MAC happened to equal its LAVC address
	 * could not catch the bug that cost a third lab node to find. */
	cfg->hw_mac[0] = 0x02u;
	cfg->hw_mac[1] = 0x00u;
	cfg->hw_mac[2] = 0x00u;
	cfg->hw_mac[3] = 0x53u;   /* 'S' */
	cfg->hw_mac[4] = 0x49u;   /* 'I' */
	cfg->hw_mac[5] = index;

	memcpy(cfg->mcast, sim_default_group, 6);
	cfg->credits = SIM_DEFAULT_CREDITS;
	cfg->credits_valid = 1u;
	cfg->rx_pool_bufs = SIM_DEFAULT_RX_POOL_BUFS;
	cfg->max_sca_len = 1500u;

	/* This node's OWN honest identity. Eight and four ASCII, the widths the
	 * wire grounds; no capture's "VMS V7.3" and no captured hardware class
	 * appears anywhere in this harness. */
	cfg->sw_version = "VMX SIM.";
	cfg->hw_type = "SIM ";
}

/* Is a fixed-width field all zeros (i.e. "not overridden")? */
static int all_zero(const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (b[i] != 0u)
			return 0;
	}
	return 1;
}

void sim_node_cfg_overlay(struct sim_node_cfg *base,
			  const struct sim_node_cfg *ov)
{
	if (!all_zero(ov->hw_mac, 6))
		memcpy(base->hw_mac, ov->hw_mac, 6);
	if (!all_zero(ov->mcast, 6))
		memcpy(base->mcast, ov->mcast, 6);
	if (ov->credits_valid) {
		base->credits = ov->credits;
		base->credits_valid = 1u;
	}
	if (ov->rx_pool_bufs != 0u)
		base->rx_pool_bufs = ov->rx_pool_bufs;
	if (ov->max_sca_len != 0u)
		base->max_sca_len = ov->max_sca_len;
	if (ov->hello_interval_ms != 0u)
		base->hello_interval_ms = ov->hello_interval_ms;
	if (ov->listen_timeout_ms != 0u)
		base->listen_timeout_ms = ov->listen_timeout_ms;
	if (ov->timvcfail_ms != 0u)
		base->timvcfail_ms = ov->timvcfail_ms;
	if (ov->vc_retransmit_ms != 0u)
		base->vc_retransmit_ms = ov->vc_retransmit_ms;
	if (ov->sw_version != NULL)
		base->sw_version = ov->sw_version;
	if (ov->hw_type != NULL)
		base->hw_type = ov->hw_type;
}

/* Copy an ASCII field into a fixed, blank-padded wire slot. `src` shorter than
 * the slot is padded; longer is truncated, which is a scenario error the
 * caller can see in the dump. */
static void fill_ascii(uint8_t *dst, uint32_t n, const char *src, uint8_t pad)
{
	uint32_t i, len = 0u;

	/* Measure first: reading src[i] past its terminator would run off the
	 * end of a short string literal. */
	if (src != NULL) {
		while (len < n && src[len] != '\0')
			len++;
	}
	for (i = 0; i < n; i++)
		dst[i] = (i < len) ? (uint8_t)src[i] : pad;
}

/* Build the identity the FSM will assert on the wire. Every field is either
 * the scenario's declared configuration or a value read from the clock. */
static void sim_node_identity(const struct sim_node *n, struct pe_identity *id)
{
	memset(id, 0, sizeof(*id));

	memcpy(id->hw_mac, n->cfg.hw_mac, 6);
	id->hw_mac_valid = 1u;
	fill_ascii(id->scsnode, VMS_HELLO_NODENAME_MAX, n->cfg.name, ' ');
	id->scsnode_len = (uint8_t)VMS_HELLO_NODENAME_MAX;
	memcpy(id->mcast, n->cfg.mcast, 6);
	id->mcast_valid = 1u;
	id->max_sca_len = n->cfg.max_sca_len;
	id->hello_interval_ms = n->cfg.hello_interval_ms;
	id->listen_timeout_ms = n->cfg.listen_timeout_ms;

	/* The cluster credential (design §5.3) is an OPEN question: this
	 * executive holds no token, so none is configured, a zero goes out and
	 * pe_fsm.nonce_absent counts it. The simulator does not invent one. */

	fill_ascii(id->sw_version, VMS_SCS_START_SWVER_LEN, n->cfg.sw_version,
		   ' ');
	id->sw_version_valid = 1u;
	fill_ascii(id->hw_type, VMS_SCS_START_HWTYPE_LEN, n->cfg.hw_type, ' ');
	id->hw_type_valid = 1u;
	id->credits_requested = n->cfg.credits;
	id->credits_requested_valid = n->cfg.credits_valid;
	/* The buffers the port owns. The FSM's ledger grants each circuit the
	 * smaller of the request and what is left of this, and advertises the
	 * grant -- so abs 95 tracks the pool, not the configuration. */
	id->rx_pool_bufs = n->cfg.rx_pool_bufs;

	/* Spec §4(g) abs 80: the instant this system BOOTED, sampled once from
	 * the clock at boot and never again. Not a captured quadword and not a
	 * per-frame sample -- replaying either is the bug §4(g) records. */
	id->incarnation_time = n->incarnation;
	id->incarnation_time_valid = 1u;

	id->timvcfail_ms = n->cfg.timvcfail_ms;
	id->vc_retransmit_ms = n->cfg.vc_retransmit_ms;
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */

void sim_node_attach(struct sim_node *n, struct sim *s, uint8_t index,
		     const struct sim_node_cfg *cfg)
{
	memset(n, 0, sizeof(*n));
	n->cfg = *cfg;
	n->sim = s;
	n->index = index;
}

int sim_node_boot(struct sim_node *n)
{
	struct pe_identity id;

	if (n->booted)
		return 0;
	n->boot_ms = n->sim->clock.now_ms;
	n->incarnation = sim_clock_now_vms(&n->sim->clock);

	sim_ops_bind(n);
	sim_node_identity(n, &id);
	if (pe_fsm_init(&n->fsm, &id, n->cfg.sysid, &n->ops) != 0)
		return -1;
	/* The FSM refuses an SCSSYSTEMID that does not fit the two bytes the
	 * wire grounds; it then emits NOTHING, which is honest but is never
	 * what a scenario meant, so it is reported rather than run. */
	if (!n->fsm.id.lavc_valid)
		return -1;

	pe_fsm_bind_vcs(&n->fsm, n->vcs, SIM_MAX_NODES);
	sim_upper_bind(n);
	pe_fsm_start(&n->fsm);
	sim_lan_set_up(&n->sim->lan, n->index, 1);
	n->booted = 1u;
	return 0;
}

void sim_node_halt(struct sim_node *n)
{
	if (!n->booted)
		return;
	(void)pe_fsm_send_last_gasp(&n->fsm);   /* §4(O.30), p. 7-29 */
	pe_fsm_shutdown(&n->fsm);
	sim_lan_set_up(&n->sim->lan, n->index, 0);
	n->booted = 0u;
}

void sim_node_set_link(struct sim_node *n, int up)
{
	if (up) {
		sim_lan_set_up(&n->sim->lan, n->index, 1);
		pe_fsm_link_up(&n->fsm);
		return;
	}
	/* Both halves, in this order: the FSM drops its circuits and channels
	 * first (so the layers above see vc_down), then the wire stops
	 * carrying its frames. pe_fsm_link_down records the fact but does not
	 * gate transmission -- gating the wire is the LAN's job. */
	(void)pe_fsm_link_down(&n->fsm, NULL, 0u);
	sim_lan_set_up(&n->sim->lan, n->index, 0);
}

/* ------------------------------------------------------------------ *
 * Reading back
 * ------------------------------------------------------------------ */

struct pe_vc *sim_node_vc_to(struct sim_node *n, uint16_t peer_sysid)
{
	return pe_fsm_vc_by_sysid(&n->fsm, (vms_scs_sysid_t)peer_sysid);
}

struct pe_channel *sim_node_channel_to(struct sim_node *n,
				       const uint8_t peer_mac[6])
{
	return pe_fsm_channel_by_mac(&n->fsm, peer_mac);
}
