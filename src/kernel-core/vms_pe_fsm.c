// SPDX-License-Identifier: GPL-2.0
/*
 * vms_pe_fsm.c - the NISCA CHANNEL state machine (FC-P0.8).
 *
 * The contract, the wire grounding and the honesty rules are in
 * vms_pe_fsm.h -- read it first. This file is the behaviour: one table indexed
 * [channel state][pe_event], one small handler per edge, and the small amount of
 * bookkeeping around it.
 *
 * READ THE TABLE, NOT THE PROSE. pe_table[][] below IS the specification of this
 * machine. Every populated cell cites the spec section that puts the edge there;
 * the cells marked INFERRED say so and say why; every EMPTY cell is an event the
 * wire spec does not connect to that state, and is ignored and COUNTED
 * (f->ignored_events) rather than guessed at. That is the same discipline
 * vms_cnxman_csb.c's ten-state ladder uses.
 *
 * PURE TU (include gate RULE 4): no seam primitive, no allocation, no libc, no
 * clock but ops->now_ms. Frames are built ONLY through the FC-P0.7 HELLO codec
 * -- there is not one byte offset in this file -- and transmitted only through
 * ops->send. That is what makes a twenty-second listen timeout and a four-rung
 * six-second size ladder testable in microseconds on the host, and what lets the
 * identical source run in both kmods and in the rung-2 N-node simulator.
 */

#include "vms_pe_fsm.h"

/* ==========================================================================
 * Small shared helpers
 *
 * This TU calls no library: each substrate spells memset/memcmp differently and
 * a pure TU must build on a bare host too. The objects are a few dozen bytes.
 * ========================================================================== */

static void pe_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		b[i] = 0u;
}

static void pe_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static int pe_mac_eq(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < VMS_ETH_ADDR_LEN; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* The injected clock, and nothing else (design SS3.9 rule 6). Zero when no
 * clock was injected -- a caller error, not a licence to read the substrate. */
static uint32_t pe_now(const struct pe_fsm *f)
{
	if (f->ops != NULL && f->ops->now_ms != NULL)
		return f->ops->now_ms(f->ops->ctx);
	return 0u;
}

/*
 * Wrap-safe deadline test. The injected clock is a 32-bit millisecond counter,
 * so it rolls over about every 49.7 days -- well inside a VMS uptime. `now >=
 * deadline` would, at the rollover, make every deadline look unreachable and
 * freeze the whole channel apparatus for weeks; the signed-difference form is
 * correct across the wrap. It is also why a clock that appears to run BACKWARDS
 * fires nothing, which is the honest response to a bad sample.
 */
static int pe_reached(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static void pe_log(const struct pe_fsm *f, const char *msg)
{
	if (f->ops != NULL && f->ops->log != NULL)
		f->ops->log(f->ops->ctx, msg);
}

static uint32_t pe_hello_interval(const struct pe_fsm *f)
{
	return f->id.hello_interval_ms != 0u ? f->id.hello_interval_ms
					     : PE_HELLO_INTERVAL_DEFAULT_MS;
}

static uint32_t pe_listen_timeout(const struct pe_fsm *f)
{
	return f->id.listen_timeout_ms != 0u ? f->id.listen_timeout_ms
					     : PE_LISTEN_TIMEOUT_DEFAULT_MS;
}

/* ==========================================================================
 * Building this node's own HELLOs
 *
 * Every field below comes from struct pe_identity -- real executive state the
 * glue read out of the seam and out of SYSGEN -- or is a FORMAT constant the
 * wire spec's SS4(b) field table publishes, which stands on exactly the footing
 * the ethertype does. Nothing is copied out of a received frame, and nothing is
 * a byte array lifted from somebody else's capture.
 * ========================================================================== */

/* SS4(a) abs 22: observed constant 0x0001 on every discovery frame. */
#define PE_CONNECT_FLAG 0x0001u

/* SS4(b) abs 94: the LE16 of the wire bytes 92 05 (the spec prints the word as
 * "0x9205"; the bytes are what the codec places). */
#define PE_TRAILER_9205 0x0592u

/* SS4(b) abs 126 / abs 130 / abs 132: the wire bytes 26 00 / 64 00 / 00 00. */
#define PE_TRAILER_2600 0x0026u
#define PE_TRAILER_0064 0x0064u
#define PE_TRAILER_0000 0x0000u

/* SS4(b) abs 102-111: the constant tail, byte-exact in every captured HELLO. */
static const uint8_t pe_tail_const[VMS_HELLO_TAILCONST_LEN] = {
	0xbc, 0x00, 0x03, 0x58, 0x51, 0x41, 0x00, 0x00, 0x00, 0x00
};

/*
 * SS4(k): the size ladder a real member was OBSERVED to walk when its padded
 * probe went unacked. Used as a table with its cite -- the search RULE that
 * produced these numbers is explicitly not derivable from passive capture and
 * is not reconstructed here (CLAUDE.md Rule 8).
 */
const uint16_t pe_probe_ladder[PE_PROBE_LADDER_N] = { 1500u, 1069u, 853u, 745u };

/*
 * The abs 96-101 field. SS4(b) records it as a value that changes every frame,
 * "plausibly a local timer/tick, not confirmed", and the codec asks for a LIVE
 * value rather than a frozen snapshot. OVMX puts its OWN monotonic clock there
 * in 100 ns units. That is an honest local timestamp; it is NOT a claim to know
 * what VMS means by the field.
 */
static void pe_put_tick(const struct pe_fsm *f, uint8_t out[6])
{
	uint64_t tick = (uint64_t)pe_now(f) * 10000ull;
	uint32_t i;

	for (i = 0; i < 6u; i++)
		out[i] = (uint8_t)((tick >> (8u * i)) & 0xffu);
}

/* The span every OVMX HELLO shares, from real state. */
static void pe_hello_common(struct pe_fsm *f, struct vms_hello_frame *h)
{
	pe_bzero(h, (uint32_t)sizeof(*h));

	h->hdr.sca_len_field = (uint16_t)(VMS_HELLO_SCA_LEN - 2u);
	h->hdr.connect_flag = PE_CONNECT_FLAG;
	pe_copy(h->hdr.eth_src, f->id.hw_mac, VMS_ETH_ADDR_LEN);
	pe_copy(h->hdr.src_lavc, f->id.lavc, VMS_ETH_ADDR_LEN);
	pe_copy(h->hw_mac, f->id.hw_mac, VMS_ETH_ADDR_LEN);

	h->disc.namelen = f->id.scsnode_len;
	pe_copy(h->disc.name, f->id.scsnode, VMS_HELLO_NODENAME_MAX);
	pe_copy(h->disc.cap_span, f->id.cap_span, VMS_DISC_CAPSPAN_LEN);
	pe_copy(h->disc.reserved_64, f->id.reserved_64, VMS_DISC_RESERVED64_LEN);

	h->trailer_9205 = PE_TRAILER_9205;
	pe_copy(h->tail_const, pe_tail_const, VMS_HELLO_TAILCONST_LEN);
	h->trailer_2600 = PE_TRAILER_2600;
	h->trailer_0064 = PE_TRAILER_0064;
	h->trailer_0000 = PE_TRAILER_0000;
	pe_put_tick(f, h->timer_tick);
}

/*
 * The connect/join nonce. GROUNDED zero on a periodic multicast HELLO; on a
 * directed HELLO and on the SS4(O.30) last gasp the real cluster carries a
 * shared token this executive does not have until design SS5.3 / FC-P0.13
 * resolves where one legitimately comes from. Absent means a zero goes out and
 * is COUNTED -- never a token replayed out of a capture.
 */
static void pe_put_nonce(struct pe_fsm *f, struct vms_hello_frame *h)
{
	if (f->id.join_nonce_valid) {
		pe_copy(h->disc.nonce, f->id.join_nonce, VMS_DISC_NONCE_LEN);
		return;
	}
	f->nonce_absent++;
}

/* A multicast HELLO: SS4(a)/SS4(b) -- incarnation 0, poller sweep 0. */
static void pe_hello_multicast(struct pe_fsm *f, struct vms_hello_frame *h,
			       uint8_t word)
{
	pe_hello_common(f, h);
	pe_copy(h->hdr.eth_dst, f->id.mcast, VMS_ETH_ADDR_LEN);
	pe_copy(h->hdr.dst_lavc, f->id.mcast, VMS_ETH_ADDR_LEN);
	h->hdr.word30 = (uint16_t)word;
	h->incarnation = 0u;
	h->poller_sweep = 0u;
}

/*
 * A directed HELLO. SS4(a).0 is the whole point of the two different addresses:
 * the frame is DELIVERED to the peer's hardware MAC at abs 0 and ADDRESSED to
 * the peer's cluster-LOGICAL address at abs 16. Mirroring abs 0 into abs 16
 * makes the peer silently drop every reply and never finalise the channel.
 *
 * SS4(i).B: our own directed HELLO always carries incarnation 1. SS4(b): the
 * poller-sweep marker is 31 on a directed HELLO, byte-exact against SDA
 * SHOW PORTS "Poller Sweep 31".
 */
static void pe_hello_directed(struct pe_fsm *f, const struct pe_channel *ch,
			      struct vms_hello_frame *h, uint8_t word)
{
	pe_hello_common(f, h);
	pe_copy(h->hdr.eth_dst, ch->remote_mac, VMS_ETH_ADDR_LEN);
	pe_copy(h->hdr.dst_lavc, ch->remote_lavc, VMS_ETH_ADDR_LEN);
	h->hdr.word30 = (uint16_t)word;
	h->incarnation = (uint16_t)PE_OWN_DIRECTED_INCARNATION;
	h->poller_sweep = PE_POLLER_SWEEP_DIRECTED;
	pe_put_nonce(f, h);
}

/* ==========================================================================
 * Transmitting -- always through the codec, never a hand-laid buffer
 * ========================================================================== */

static int pe_tx(struct pe_fsm *f, uint32_t len)
{
	int st;

	if (f->ops == NULL || f->ops->send == NULL)
		return -1;
	st = f->ops->send(f->ops->ctx, f->scratch, len);
	if (st != 0) {
		f->tx_errors++;
		return -1;
	}
	return 0;
}

/* Nothing goes out without a source identity: an SCA frame whose abs 24 is not
 * this node's real cluster-LOGICAL address is not a frame OVMX is entitled to
 * send (INV-6), so the port stays silent and says so. */
static int pe_may_send(const struct pe_fsm *f)
{
	return f->id.lavc_valid && f->id.hw_mac_valid;
}

static int pe_emit_plain(struct pe_fsm *f, const struct vms_hello_frame *h)
{
	uint32_t written = 0u;

	if (vms_hello_build(h, f->scratch, (uint32_t)sizeof(f->scratch),
			    &written) != VMS_CODEC_OK)
		return -1;
	return pe_tx(f, written);
}

static int pe_emit_padded(struct pe_fsm *f, const struct vms_hello_frame *h,
			  uint16_t sca_len)
{
	uint32_t written = 0u;

	if (vms_hello_build_padded(h, sca_len, f->scratch,
				   (uint32_t)sizeof(f->scratch),
				   &written) != VMS_CODEC_OK)
		return -1;
	return pe_tx(f, written);
}

/* One directed HELLO carrying `word`, counted per the b-ladder. */
static int pe_send_directed(struct pe_fsm *f, struct pe_channel *ch, uint8_t word)
{
	struct vms_hello_frame h;

	if (!pe_may_send(f) || !ch->remote_lavc_valid)
		return -1;
	pe_hello_directed(f, ch, &h, word);
	if (pe_emit_plain(f, &h) != 0)
		return -1;
	ch->hello_tx++;
	if (word == PE_PFW_VERIFY_B3)
		ch->b3_tx++;
	else if (word == PE_PFW_VERIFY_B4)
		ch->b4_tx++;
	return 0;
}

/* One SS4(k) size-verification probe: the same directed HELLO, carrying b3
 * (SS4(a).1: "the padded SS4(k) size-verify HELLOs are simply b3 REQUESTs"),
 * zero-padded to `sca_len`. */
static int pe_send_padded(struct pe_fsm *f, struct pe_channel *ch,
			  uint16_t sca_len)
{
	struct vms_hello_frame h;

	if (!pe_may_send(f) || !ch->remote_lavc_valid)
		return -1;
	pe_hello_directed(f, ch, &h, PE_PFW_VERIFY_B3);
	if (pe_emit_padded(f, &h, sca_len) != 0)
		return -1;
	ch->hello_tx++;
	ch->b3_tx++;
	ch->padded_tx++;
	return 0;
}

static void pe_send_multicast(struct pe_fsm *f)
{
	struct vms_hello_frame h;

	if (!pe_may_send(f) || !f->id.mcast_valid)
		return;
	pe_hello_multicast(f, &h, PE_PFW_MULTICAST);
	if (pe_emit_plain(f, &h) == 0)
		f->mcast_hello_tx++;
}

/* ==========================================================================
 * The channel table
 * ========================================================================== */

struct pe_channel *pe_fsm_channel_at(struct pe_fsm *f, uint32_t index)
{
	if (f == NULL || index >= f->n_channels)
		return NULL;
	return f->ch[index].in_use ? &f->ch[index] : NULL;
}

struct pe_channel *pe_fsm_channel_by_mac(struct pe_fsm *f,
					 const uint8_t mac[VMS_ETH_ADDR_LEN])
{
	uint32_t i;

	if (f == NULL || mac == NULL)
		return NULL;
	for (i = 0; i < f->n_channels; i++) {
		if (f->ch[i].in_use && pe_mac_eq(f->ch[i].remote_mac, mac))
			return &f->ch[i];
	}
	return NULL;
}

/* A new station. The table is never recycled under pressure: a full table
 * REFUSES the station and counts it, because silently evicting a live channel
 * to make room would drop a circuit the executive still believes in. */
static struct pe_channel *pe_channel_alloc(struct pe_fsm *f,
					   const uint8_t mac[VMS_ETH_ADDR_LEN])
{
	uint32_t i;

	for (i = 0; i < (uint32_t)PE_MAX_CHANNELS; i++) {
		struct pe_channel *ch = &f->ch[i];

		if (ch->in_use)
			continue;
		pe_bzero(ch, (uint32_t)sizeof(*ch));
		ch->in_use = 1u;
		ch->state = (uint8_t)VMS_PE_CH_CLOSED;
		pe_copy(ch->remote_mac, mac, VMS_ETH_ADDR_LEN);
		if (i + 1u > f->n_channels)
			f->n_channels = i + 1u;
		return ch;
	}
	f->rx_no_slot++;
	return NULL;
}

/*
 * Undo everything the b-ladder and the size verification proved. Called when
 * the peer says the channel's generation is over -- a new incarnation, a
 * re-INIT, a link bounce. The learned IDENTITY and the counters survive: they
 * are real facts about a station this node has genuinely seen.
 */
static void pe_ladder_reset(struct pe_channel *ch)
{
	ch->verified_pktsz = 0u;
	ch->probe_sca_len = 0u;
	ch->probe_rung = 0u;
	ch->probe_tries = 0u;
	ch->probe_exhausted = 0u;
	ch->probe_due_ms = 0u;
}

static void pe_channel_close(struct pe_channel *ch)
{
	pe_ladder_reset(ch);
	ch->state = (uint8_t)VMS_PE_CH_CLOSED;
	ch->deadline_ms = 0u;
}

/* ==========================================================================
 * The SS4(k) size verification
 * ========================================================================== */

/* The highest ladder rung this channel may legitimately probe at. */
static uint8_t pe_probe_rung_for(uint16_t cap)
{
	uint8_t i;

	for (i = 0; i < (uint8_t)PE_PROBE_LADDER_N; i++) {
		if (pe_probe_ladder[i] <= cap)
			return i;
	}
	return (uint8_t)PE_PROBE_LADDER_N;   /* nothing on the ladder fits */
}

static void pe_probe_arm(struct pe_fsm *f, struct pe_channel *ch, uint8_t rung)
{
	if (rung >= (uint8_t)PE_PROBE_LADDER_N) {
		/* SS4(k) stops at 745 and is silent about what follows. Stop
		 * probing and record it, rather than invent a next step. */
		ch->probe_sca_len = 0u;
		ch->probe_exhausted = 1u;
		return;
	}
	ch->probe_rung = rung;
	ch->probe_tries = 0u;
	ch->probe_sca_len = pe_probe_ladder[rung];
	ch->probe_due_ms = pe_now(f) + PE_PROBE_RETRANSMIT_MS;
	(void)pe_send_padded(f, ch, ch->probe_sca_len);
}

/*
 * Start a size verification. `cap` is 0 for our own initiative (SS4(k) step 2,
 * "emit a padded HELLO up to NISCS_MAX_PKTSZ to advertise its own channel
 * size") or the size a received padded HELLO carried (step 1, the
 * reciprocation that is present in the successful formation and absent in the
 * stalled one). Either way the port never probes above what its own interface
 * can carry, and never claims a size it has not proven.
 */
static void pe_probe_start(struct pe_fsm *f, struct pe_channel *ch, uint16_t cap)
{
	uint16_t limit = f->id.max_sca_len;

	if (limit == 0u)
		return;                     /* MTU unknown: assert no size */
	if (cap != 0u && cap < limit)
		limit = cap;
	if (ch->probe_sca_len != 0u)
		return;                     /* one probe in flight at a time */
	if (ch->verified_pktsz >= limit)
		return;                     /* already proven at least this far */
	pe_probe_arm(f, ch, pe_probe_rung_for(limit));
}

/* The retransmit ladder: GROUNDED at ~4 frames per size, 6.010 s apart. */
static void pe_probe_tick(struct pe_fsm *f, struct pe_channel *ch, uint32_t now)
{
	if (ch->probe_sca_len == 0u)
		return;
	if (!pe_reached(now, ch->probe_due_ms))
		return;

	ch->probe_tries++;
	if (ch->probe_tries >= (uint8_t)PE_PROBE_TRIES_PER_SIZE) {
		pe_probe_arm(f, ch, (uint8_t)(ch->probe_rung + 1u));
		return;
	}
	ch->probe_due_ms = now + PE_PROBE_RETRANSMIT_MS;
	(void)pe_send_padded(f, ch, ch->probe_sca_len);
}

/* ==========================================================================
 * What a received frame carried
 * ========================================================================== */
struct pe_rx {
	const struct vms_frame_info *fi;
	const struct vms_sca_hdr    *hdr;
	const struct vms_disc_body  *disc;
	uint16_t incarnation;        /* abs 92, meaningful only when directed */
	uint8_t  incarnation_valid;
	uint8_t  directed;           /* addressed to THIS node, not the group */
};

/* The pe_rx a timer or a link event carries: nothing. One file-scope constant
 * rather than a per-call literal, so no handler can be handed a NULL view. */
static const struct pe_rx pe_rx_none;

/*
 * Initiate a verify: SS4(a).1 grounds b3 as the REQUEST any node may originate.
 * There is deliberately no b2 anywhere in this file -- see the header.
 */
static void pe_initiate_verify(struct pe_fsm *f, struct pe_channel *ch)
{
	/*
	 * A padded size probe IS a b3 REQUEST (SS4(a).1), so while one is
	 * outstanding there is nothing to add: sending a plain b3 beside it
	 * would make the peer's single b4 CONFIRM ambiguous, and this FSM would
	 * then be crediting a packet size it had not actually proven. The
	 * REQUEST is out either way, so the ladder position still advances.
	 */
	if (ch->probe_sca_len == 0u &&
	    pe_send_directed(f, ch, PE_PFW_VERIFY_B3) != 0)
		return;
	if (ch->state == (uint8_t)VMS_PE_CH_CLOSED ||
	    ch->state == (uint8_t)VMS_PE_CH_SEEN)
		ch->state = (uint8_t)VMS_PE_CH_B3;
}

/* ==========================================================================
 * The transition handlers -- one edge each
 * ========================================================================== */

/*
 * A HELLO from a station whose channel has not started its ladder. The plan
 * item's "directed HELLO on first sight": the moment a station is seen, OVMX
 * addresses it directly and asks it to verify the channel.
 *
 * INFERRED (timing only). SS4(a).1 grounds that a node initiates a verify with
 * b3 and SS4(k) step 2 explicitly recommends OVMX "send first"; what is NOT
 * grounded is whether the golden joiner fires its first directed HELLO before
 * the member's b2 arrives, because in both reference formations the member
 * happened to init first. Sending it is the recommended shape and costs one
 * frame; waiting risks the member never initiating at all.
 *
 * A station whose HELLO carried no cluster-LOGICAL address gets NO directed
 * frame: SS4(a).0 requires the peer's LOGICAL address at abs 16, and there is
 * no honest way to fill it from the hardware MAC. The channel rests at SEEN and
 * a later HELLO that does carry one starts the ladder.
 */
static enum pe_channel_action h_first_sight(struct pe_fsm *f,
					    struct pe_channel *ch,
					    const struct pe_rx *rx)
{
	(void)rx;
	if (ch->state == (uint8_t)VMS_PE_CH_CLOSED)
		ch->state = (uint8_t)VMS_PE_CH_SEEN;
	if (ch->remote_lavc_valid)
		pe_initiate_verify(f, ch);
	return PE_CH_ACT_NONE;
}

/* A HELLO on a channel already walking its ladder: the peer is alive. That is
 * the whole content of the event, and pe_fsm_rx has already refreshed the
 * listen deadline from it. Explicit rather than an empty cell, because a
 * keepalive is a NORMAL event and must not be counted as one with no edge. */
static enum pe_channel_action h_hello_noted(struct pe_fsm *f,
					    struct pe_channel *ch,
					    const struct pe_rx *rx)
{
	(void)f; (void)ch; (void)rx;
	return PE_CH_ACT_NONE;
}

/*
 * SS4(c) SOLICIT: a satellite asking to be served a system disk. OVMX has no
 * MSCP server until FC-P6.x, so it answers NOTHING and counts the request. Spec
 * SS4(p)'s rule is an allowlist with no default; answering a request this node
 * cannot serve is how OVMX crashed two real VAXes.
 */
static enum pe_channel_action h_solicit(struct pe_fsm *f, struct pe_channel *ch,
					const struct pe_rx *rx)
{
	(void)ch; (void)rx;
	f->rx_solicit++;
	return PE_CH_ACT_NONE;
}

/*
 * SS4(a).1 b2, channel INIT: the established member's first directed contact.
 * The response rule is "reply X+1": b2 -> b3.
 *
 * A b2 arriving on a channel that had already reached b4 is INFERRED to mean
 * the member re-INITed the channel (the reference captures show b2 exactly once
 * per channel, so a second one is outside the observed data). The conservative
 * reading is taken: everything the old generation proved is discarded and the
 * ladder re-runs, and the layers above are told. Believing a stale b4 across a
 * peer-side re-INIT is the failure that cannot be undone.
 */
static enum pe_channel_action h_verify_b2(struct pe_fsm *f,
					  struct pe_channel *ch,
					  const struct pe_rx *rx)
{
	int was_verified = (ch->state == (uint8_t)VMS_PE_CH_B4);

	(void)rx;
	ch->b2_rx++;
	if (was_verified) {
		pe_ladder_reset(ch);
		ch->resets++;
		pe_log(f, "%PEA0, peer re-initialised the channel, re-verifying");
	}
	ch->state = (uint8_t)VMS_PE_CH_B2;
	(void)pe_send_directed(f, ch, PE_PFW_VERIFY_B3);
	return was_verified ? PE_CH_ACT_RESET : PE_CH_ACT_NONE;
}

/*
 * SS4(a).1 b3, channel-verify REQUEST. Two separate obligations, both GROUNDED:
 *
 *  1. Answer it with b4 "in immediate (~0.2 ms) response". OVMX holding abs-30
 *     at a fixed b3 and never emitting b4 is exactly what made VAX1 loop the
 *     padded-HELLO flood forever (vms-d94).
 *  2. If the REQUEST arrived PADDED, reciprocate with our own padded HELLO on
 *     the reverse channel (SS4(k): exactly one padded HELLO per direction in
 *     the successful formation, zero from the joiner in the stalled one).
 *
 * Answering somebody else's b3 proves their->our direction only, so it does not
 * advance OUR ladder; a channel that has not yet asked for its own verification
 * asks now.
 */
static enum pe_channel_action h_verify_b3(struct pe_fsm *f,
					  struct pe_channel *ch,
					  const struct pe_rx *rx)
{
	ch->b3_rx++;
	(void)pe_send_directed(f, ch, PE_PFW_VERIFY_B4);

	if (rx->fi != NULL &&
	    rx->fi->cls == (uint8_t)VMS_FCLS_HELLO_PADDED) {
		ch->padded_rx++;
		pe_probe_start(f, ch, rx->fi->sca_content);
	}

	if (ch->state == (uint8_t)VMS_PE_CH_CLOSED ||
	    ch->state == (uint8_t)VMS_PE_CH_SEEN)
		pe_initiate_verify(f, ch);
	return PE_CH_ACT_NONE;
}

/*
 * SS4(a).1 b4, channel-verify CONFIRM -- the ack of a b3 WE sent, and the only
 * thing that makes a channel usable.
 *
 * SS4(a).1 also grounds that a padded b3 is acked by a PLAIN b4, so a b4
 * arriving while a size probe is outstanding is that probe's confirmation --
 * and while a probe is outstanding this FSM sends no other b3 on the channel
 * (see the channel tick), so there is nothing else it could be acking.
 */
static enum pe_channel_action h_verify_b4(struct pe_fsm *f,
					  struct pe_channel *ch,
					  const struct pe_rx *rx)
{
	int first = (ch->state != (uint8_t)VMS_PE_CH_B4);

	(void)rx;
	ch->b4_rx++;
	if (ch->probe_sca_len != 0u) {
		ch->verified_pktsz = ch->probe_sca_len;
		ch->probe_sca_len = 0u;
		ch->probe_tries = 0u;
	}
	if (!first)
		return PE_CH_ACT_NONE;      /* the steady b3<->b4 oscillation */

	ch->state = (uint8_t)VMS_PE_CH_B4;
	pe_log(f, "%PEA0, channel verified");
	pe_probe_start(f, ch, 0u);          /* SS4(k) step 2: advertise our size */
	return PE_CH_ACT_VERIFIED;
}

/*
 * SS4(i).B: the peer now attributes a DIFFERENT incarnation to this node, so it
 * regards the previous channel generation as gone. Everything the old
 * generation proved goes with it, the ladder re-runs, and the layers above are
 * told -- FC-P1.2's START must echo the NEW number, and a circuit built on the
 * old one is stale.
 */
static enum pe_channel_action h_new_incarnation(struct pe_fsm *f,
						struct pe_channel *ch,
						const struct pe_rx *rx)
{
	ch->peer_incarnation = rx->incarnation;
	ch->peer_incarnation_valid = 1u;
	ch->resets++;
	pe_ladder_reset(ch);
	ch->state = (uint8_t)VMS_PE_CH_SEEN;
	pe_log(f, "%PEA0, peer advertised a new incarnation, channel reset");
	if (ch->remote_lavc_valid)
		pe_initiate_verify(f, ch);
	return PE_CH_ACT_RESET;
}

/*
 * SS4(O.30): the peer's clean-leave last gasp -- a multicast HELLO with the
 * departure marker at abs 30. GROUNDED byte-exact on two real-VAX clean leaves,
 * appearing exactly once per capture, with the departing node emitting nothing
 * afterwards. This is p. 7-29's "announced departure": the channel goes at
 * once, and the caller passes that distinction to CNXMAN so it takes the
 * immediate-reconfiguration path instead of waiting out the whole reconnect
 * period for a node that has already said goodbye.
 */
static enum pe_channel_action h_last_gasp(struct pe_fsm *f,
					  struct pe_channel *ch,
					  const struct pe_rx *rx)
{
	(void)rx;
	pe_channel_close(ch);
	pe_log(f, "%PEA0, peer announced departure, channel closed");
	return PE_CH_ACT_DEPARTED;
}

/*
 * The channel's own tick. One beat does all three jobs, each against the
 * injected clock and each wrap-safe -- the same shape FC-P3.6's once-a-second
 * reconnect beat has, and the reason no second timer identity is needed here.
 */
static enum pe_channel_action h_tick(struct pe_fsm *f, struct pe_channel *ch,
				     const struct pe_rx *rx)
{
	uint32_t now = pe_now(f);

	(void)rx;

	/* SS4(M): healthy silence never exceeded 3.153 s in 747 s of captured
	 * wire, and a real departure showed 395.955 s; the two populations do
	 * not overlap. Past the deadline this node stops believing in the
	 * channel rather than holding a circuit it cannot support. */
	if (pe_reached(now, ch->deadline_ms)) {
		pe_channel_close(ch);
		pe_log(f, "%PEA0, no HELLO within the listen timeout, channel lost");
		return PE_CH_ACT_LOST;
	}

	/* SS4(a).1's keepalive: "each node periodically re-initiates the verify
	 * with a fresh b3 REQUEST ... and the peer immediately acks it with a b4
	 * CONFIRM." pe_initiate_verify holds it back while a size probe is
	 * outstanding -- that probe is already the b3. */
	if (ch->remote_lavc_valid)
		pe_initiate_verify(f, ch);
	pe_probe_tick(f, ch, now);
	return PE_CH_ACT_NONE;
}

/* A closed channel has nothing to tick, nothing to lose and nothing to shut
 * down. Explicit rather than an empty cell: these events reach it on every beat
 * and every link bounce, and they are NORMAL -- counting them as edges the wire
 * spec does not ground would bury the counter that exists to find real gaps. */
static enum pe_channel_action h_idle(struct pe_fsm *f, struct pe_channel *ch,
				     const struct pe_rx *rx)
{
	(void)f; (void)ch; (void)rx;
	return PE_CH_ACT_NONE;
}

/* The interface went away. There is no connectivity to any station over a link
 * that is down, so no channel may stay verified across it. */
static enum pe_channel_action h_link_down(struct pe_fsm *f,
					  struct pe_channel *ch,
					  const struct pe_rx *rx)
{
	(void)f; (void)rx;
	pe_channel_close(ch);
	return PE_CH_ACT_LOST;
}

/* CLUSTER_STOP. Closes quietly: the departure announcement is a separate,
 * deliberate act (pe_fsm_send_last_gasp), driven by the connection manager. */
static enum pe_channel_action h_close(struct pe_fsm *f, struct pe_channel *ch,
				      const struct pe_rx *rx)
{
	(void)f; (void)rx;
	pe_channel_close(ch);
	return PE_CH_ACT_NONE;
}

/* ==========================================================================
 * The table. [channel state][pe_event]; an empty cell is an event the wire
 * spec does not connect to that state -- ignored and COUNTED, never guessed.
 * ========================================================================== */
typedef enum pe_channel_action (*pe_handler_t)(struct pe_fsm *,
					       struct pe_channel *,
					       const struct pe_rx *);

static const pe_handler_t pe_table[VMS_PE_CH_STATE__COUNT][PE_EV__COUNT] = {
	/*
	 * [CLOSED] no channel: either never started, or closed by a departure,
	 * a link bounce or the listen timeout. Its identity and counters are
	 * retained, so a station that comes back is recognised rather than
	 * re-discovered. No tick: a closed channel has no deadline to miss.
	 */
	[VMS_PE_CH_CLOSED] = {
		[PE_EV_RX_HELLO]           = h_first_sight,   /* SS4(a) */
		[PE_EV_RX_SOLICIT]         = h_solicit,       /* SS4(c) */
		[PE_EV_RX_VERIFY_B2]       = h_verify_b2,     /* SS4(a).1 */
		[PE_EV_RX_VERIFY_B3]       = h_verify_b3,     /* SS4(a).1 */
		[PE_EV_RX_NEW_INCARNATION] = h_new_incarnation, /* SS4(i).B */
		[PE_EV_TIMER_CHANNEL]      = h_idle,
		[PE_EV_LINK_DOWN]          = h_idle,
		[PE_EV_SHUTDOWN]           = h_idle,
	},

	/*
	 * [SEEN] a HELLO arrived but no verify has been asked for -- which on
	 * this implementation means only one thing: the station has not yet
	 * given us a cluster-LOGICAL address to direct a HELLO at (SS4(a).0).
	 */
	[VMS_PE_CH_SEEN] = {
		[PE_EV_RX_HELLO]           = h_first_sight,
		[PE_EV_RX_SOLICIT]         = h_solicit,
		[PE_EV_RX_VERIFY_B2]       = h_verify_b2,
		[PE_EV_RX_VERIFY_B3]       = h_verify_b3,
		[PE_EV_RX_NEW_INCARNATION] = h_new_incarnation,
		[PE_EV_RX_LAST_GASP]       = h_last_gasp,     /* SS4(O.30) */
		[PE_EV_TIMER_CHANNEL]      = h_tick,
		[PE_EV_LINK_DOWN]          = h_link_down,
		[PE_EV_SHUTDOWN]           = h_close,
	},

	/*
	 * [B2] the member's INIT was answered with b3 and the CONFIRM is
	 * awaited. This is the GROUNDED joiner path: member b2 -> joiner b3 ->
	 * member b4.
	 */
	[VMS_PE_CH_B2] = {
		[PE_EV_RX_HELLO]           = h_hello_noted,
		[PE_EV_RX_SOLICIT]         = h_solicit,
		[PE_EV_RX_VERIFY_B2]       = h_verify_b2,
		[PE_EV_RX_VERIFY_B3]       = h_verify_b3,
		[PE_EV_RX_VERIFY_B4]       = h_verify_b4,
		[PE_EV_RX_NEW_INCARNATION] = h_new_incarnation,
		[PE_EV_RX_LAST_GASP]       = h_last_gasp,
		[PE_EV_TIMER_CHANNEL]      = h_tick,
		[PE_EV_LINK_DOWN]          = h_link_down,
		[PE_EV_SHUTDOWN]           = h_close,
	},

	/* [B3] our own b3 REQUEST is outstanding. Reached without any b2 when
	 * two OVMX nodes verify each other, which is what the rung-2 simulator
	 * and the rung-4 executive harness run on. */
	[VMS_PE_CH_B3] = {
		[PE_EV_RX_HELLO]           = h_hello_noted,
		[PE_EV_RX_SOLICIT]         = h_solicit,
		[PE_EV_RX_VERIFY_B2]       = h_verify_b2,
		[PE_EV_RX_VERIFY_B3]       = h_verify_b3,
		[PE_EV_RX_VERIFY_B4]       = h_verify_b4,
		[PE_EV_RX_NEW_INCARNATION] = h_new_incarnation,
		[PE_EV_RX_LAST_GASP]       = h_last_gasp,
		[PE_EV_TIMER_CHANNEL]      = h_tick,
		[PE_EV_LINK_DOWN]          = h_link_down,
		[PE_EV_SHUTDOWN]           = h_close,
	},

	/* [B4] verified and usable. The b3<->b4 oscillation runs from here for
	 * as long as the channel lives. */
	[VMS_PE_CH_B4] = {
		[PE_EV_RX_HELLO]           = h_hello_noted,
		[PE_EV_RX_SOLICIT]         = h_solicit,
		[PE_EV_RX_VERIFY_B2]       = h_verify_b2,     /* INFERRED re-INIT */
		[PE_EV_RX_VERIFY_B3]       = h_verify_b3,
		[PE_EV_RX_VERIFY_B4]       = h_verify_b4,
		[PE_EV_RX_NEW_INCARNATION] = h_new_incarnation,
		[PE_EV_RX_LAST_GASP]       = h_last_gasp,
		[PE_EV_TIMER_CHANNEL]      = h_tick,
		[PE_EV_LINK_DOWN]          = h_link_down,
		[PE_EV_SHUTDOWN]           = h_close,
	},
};

static enum pe_channel_action pe_dispatch(struct pe_fsm *f,
					  struct pe_channel *ch,
					  enum pe_event ev,
					  const struct pe_rx *rx)
{
	pe_handler_t h;

	if (f == NULL || ch == NULL || !ch->in_use)
		return PE_CH_ACT_NONE;
	if ((unsigned)ev >= (unsigned)PE_EV__COUNT)
		return PE_CH_ACT_NONE;
	if ((unsigned)ch->state >= (unsigned)VMS_PE_CH_STATE__COUNT)
		return PE_CH_ACT_NONE;

	h = pe_table[ch->state][ev];
	if (h == NULL) {
		f->ignored_events++;
		return PE_CH_ACT_NONE;
	}
	return h(f, ch, rx);
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

int pe_fsm_init(struct pe_fsm *f, const struct pe_identity *id,
		vms_scs_sysid_t sysid, const struct pe_ops *ops)
{
	if (f == NULL || ops == NULL)
		return -1;

	pe_bzero(f, (uint32_t)sizeof(*f));
	f->ops = ops;
	if (id != NULL)
		f->id = *id;

	/*
	 * SS4(a) grounds the cluster-LOGICAL address as aa:00:04:00:<LE16>, and
	 * SS3's decoder ring shows the two bytes carrying the DECnet-style node
	 * address (VAX1 1025, VAX2 1026). A SCSSYSTEMID that does not fit those
	 * two bytes has no representation on this wire, so the address is left
	 * INVALID and the port emits nothing -- truncating it would put a
	 * different node's identity on the LAN.
	 */
	f->id.lavc_valid = 0u;
	if (sysid != 0u && sysid <= 0xffffu) {
		vms_cluster_lavc_addr_build((uint16_t)sysid, f->id.lavc);
		f->id.lavc_valid = 1u;
	}
	return 0;
}

static void pe_arm_beat(struct pe_fsm *f)
{
	if (f->ops != NULL && f->ops->arm_timer != NULL)
		f->ops->arm_timer(f->ops->ctx, PE_TIMER_HELLO, 0u,
				  pe_hello_interval(f));
}

void pe_fsm_start(struct pe_fsm *f)
{
	if (f == NULL || f->running)
		return;
	f->running = 1u;
	f->link_up = 1u;
	pe_arm_beat(f);
}

void pe_fsm_stop(struct pe_fsm *f)
{
	if (f == NULL || !f->running)
		return;
	f->running = 0u;
	if (f->ops != NULL && f->ops->cancel_timer != NULL)
		f->ops->cancel_timer(f->ops->ctx, PE_TIMER_HELLO, 0u);
}

/* ==========================================================================
 * Receive: classify, learn, dispatch
 * ========================================================================== */

/* Which pe_event a received discovery frame IS. The codec decides, through the
 * class-gated abs-30 accessor -- this file never reads a byte offset. */
static enum pe_event pe_event_for(const uint8_t *frame, uint32_t len,
				  const struct vms_frame_info *fi)
{
	uint8_t word = 0u;

	if (fi->cls == (uint8_t)VMS_FCLS_SOLICIT)
		return PE_EV_RX_SOLICIT;
	if (vms_sca_chan_word(frame, len, fi, &word) != VMS_CODEC_OK)
		return PE_EV_RX_HELLO;

	switch (word) {
	case PE_PFW_VERIFY_B2:  return PE_EV_RX_VERIFY_B2;
	case PE_PFW_VERIFY_B3:  return PE_EV_RX_VERIFY_B3;
	case PE_PFW_VERIFY_B4:  return PE_EV_RX_VERIFY_B4;
	case PE_PFW_LAST_GASP:  return PE_EV_RX_LAST_GASP;
	default:                return PE_EV_RX_HELLO;
	}
}

/* Everything the frame honestly tells us about the station that sent it. Each
 * value carries its own validity: a name is learned only from a frame that
 * carried one, a sysid only from an address that really is a LOGICAL one. */
static void pe_channel_learn(struct pe_fsm *f, struct pe_channel *ch,
			     const struct pe_rx *rx)
{
	uint16_t sysid = 0u;

	ch->last_rx_ms = pe_now(f);
	ch->deadline_ms = ch->last_rx_ms + pe_listen_timeout(f);
	ch->hello_rx++;

	if (vms_cluster_lavc_is_logical(rx->hdr->src_lavc)) {
		pe_copy(ch->remote_lavc, rx->hdr->src_lavc, VMS_ETH_ADDR_LEN);
		ch->remote_lavc_valid = 1u;
		if (vms_cluster_lavc_sysid(rx->hdr->src_lavc, &sysid) ==
		    VMS_CODEC_OK) {
			ch->remote_sysid = (vms_scs_sysid_t)sysid;
			ch->remote_sysid_valid = 1u;
		}
	}

	if (rx->disc != NULL &&
	    rx->disc->namelen != 0u &&
	    rx->disc->namelen <= (uint8_t)VMS_HELLO_NODENAME_MAX) {
		ch->remote_name_len = rx->disc->namelen;
		pe_copy(ch->remote_name, rx->disc->name, VMS_HELLO_NODENAME_MAX);
	}
}

/* Is this frame ours? A directed frame names THIS node's LOGICAL address at
 * abs 16; a multicast one names the cluster group. Anything else is somebody
 * else's traffic on a shared LAN and is counted, not processed. */
static int pe_frame_is_ours(const struct pe_fsm *f, const struct vms_sca_hdr *hdr,
			    uint8_t *directed)
{
	if (f->id.lavc_valid && pe_mac_eq(hdr->dst_lavc, f->id.lavc)) {
		*directed = 1u;
		return 1;
	}
	if (f->id.mcast_valid && pe_mac_eq(hdr->dst_lavc, f->id.mcast)) {
		*directed = 0u;
		return 1;
	}
	return 0;
}

/*
 * SS4(i).B. A directed HELLO's abs-92 word is the incarnation the SENDER
 * attributes to US. The first one seen is recorded; a DIFFERENT one afterwards
 * is the peer telling us the previous channel generation is over, and is
 * dispatched as its own event so the reset is a table edge like any other.
 */
static enum pe_channel_action pe_check_incarnation(struct pe_fsm *f,
						   struct pe_channel *ch,
						   const struct pe_rx *rx)
{
	if (!rx->directed || !rx->incarnation_valid)
		return PE_CH_ACT_NONE;
	if (!ch->peer_incarnation_valid) {
		ch->peer_incarnation = rx->incarnation;
		ch->peer_incarnation_valid = 1u;
		return PE_CH_ACT_NONE;
	}
	if (ch->peer_incarnation == rx->incarnation)
		return PE_CH_ACT_NONE;
	return pe_dispatch(f, ch, PE_EV_RX_NEW_INCARNATION, rx);
}

/* Decode a discovery frame into the shared pe_rx view. 0 on success. */
static int pe_parse_discovery(const uint8_t *frame, uint32_t len,
			      const struct vms_frame_info *fi,
			      struct vms_hello_frame *hello,
			      struct vms_solicit_frame *sol,
			      struct pe_rx *rx)
{
	if (fi->cls == (uint8_t)VMS_FCLS_SOLICIT) {
		if (vms_solicit_parse(frame, len, fi, sol) != VMS_CODEC_OK)
			return -1;
		rx->hdr = &sol->hdr;
		rx->disc = &sol->disc;
		return 0;
	}
	if (vms_hello_parse(frame, len, fi, hello) != VMS_CODEC_OK)
		return -1;
	rx->hdr = &hello->hdr;
	rx->disc = &hello->disc;
	rx->incarnation = hello->incarnation;
	rx->incarnation_valid = 1u;
	return 0;
}

enum pe_channel_action pe_fsm_rx(struct pe_fsm *f, const uint8_t *frame,
				 uint32_t len)
{
	struct vms_hello_frame hello;
	struct vms_solicit_frame sol;
	struct vms_frame_info fi;
	struct pe_rx rx;
	struct pe_channel *ch;
	enum pe_channel_action act;
	vms_codec_status_t st;

	if (f == NULL || frame == NULL)
		return PE_CH_ACT_NONE;
	f->rx_frames++;

	st = vms_frame_classify(frame, len, &fi);
	if (st == VMS_CODEC_E_NOTSCA) {
		f->rx_not_sca++;
		return PE_CH_ACT_NONE;
	}
	/* Anything that is not a discovery-family frame belongs to a layer this
	 * FSM does not own (the SCS envelope is FC-P1.2's). Counted, not
	 * silently dropped, so a rising number is a question for a capture. */
	if (fi.family != (uint8_t)VMS_FFAM_DISCOVERY) {
		f->rx_unclassified++;
		return PE_CH_ACT_NONE;
	}

	pe_bzero(&rx, (uint32_t)sizeof(rx));
	rx.fi = &fi;
	if (pe_parse_discovery(frame, len, &fi, &hello, &sol, &rx) != 0) {
		f->rx_parse_failed++;
		return PE_CH_ACT_NONE;
	}
	if (!pe_frame_is_ours(f, rx.hdr, &rx.directed)) {
		f->rx_not_for_us++;
		return PE_CH_ACT_NONE;
	}

	ch = pe_fsm_channel_by_mac(f, rx.hdr->eth_src);
	if (ch == NULL) {
		ch = pe_channel_alloc(f, rx.hdr->eth_src);
		if (ch == NULL)
			return PE_CH_ACT_NONE;
	}
	pe_channel_learn(f, ch, &rx);

	act = pe_check_incarnation(f, ch, &rx);
	if (act != PE_CH_ACT_NONE)
		return act;   /* the reset supersedes whatever the frame asked */

	return pe_dispatch(f, ch, pe_event_for(frame, len, &fi), &rx);
}

/* ==========================================================================
 * Timers, link and shutdown
 * ========================================================================== */

static void pe_rec_emit(struct pe_channel_rec *out, uint32_t max, uint32_t *n,
			uint32_t index, enum pe_channel_action act)
{
	if (out == NULL || *n >= max)
		return;
	out[*n].channel_index = index;
	out[*n].action = (uint8_t)act;
	out[*n].pad[0] = 0u;
	out[*n].pad[1] = 0u;
	out[*n].pad[2] = 0u;
	(*n)++;
}

/* Dispatch one event to every live channel, collecting the actions. */
static uint32_t pe_broadcast(struct pe_fsm *f, enum pe_event ev,
			     struct pe_channel_rec *out, uint32_t max)
{
	uint32_t i, n = 0u;

	for (i = 0; i < f->n_channels; i++) {
		enum pe_channel_action act;

		if (!f->ch[i].in_use)
			continue;
		act = pe_dispatch(f, &f->ch[i], ev, &pe_rx_none);
		if (act != PE_CH_ACT_NONE)
			pe_rec_emit(out, max, &n, i, act);
	}
	return n;
}

uint32_t pe_fsm_tick(struct pe_fsm *f, struct pe_channel_rec *out, uint32_t max)
{
	uint32_t n;

	if (f == NULL)
		return 0u;

	/* SS4(q): the cadence is an ongoing obligation of a member, not a
	 * discovery-only behaviour, so it runs whatever the channels are doing. */
	pe_send_multicast(f);
	n = pe_broadcast(f, PE_EV_TIMER_CHANNEL, out, max);
	if (f->running)
		pe_arm_beat(f);
	return n;
}

enum pe_channel_action pe_fsm_channel_timer(struct pe_fsm *f, uint32_t index)
{
	struct pe_channel *ch = pe_fsm_channel_at(f, index);

	if (ch == NULL)
		return PE_CH_ACT_NONE;
	return pe_dispatch(f, ch, PE_EV_TIMER_CHANNEL, &pe_rx_none);
}

/*
 * The DISCOVERY-family events this FSM acts on carry evidence -- the parsed
 * frame that justified them -- and pe_fsm_rx is the only place that evidence
 * exists, so a bare post of one is refused rather than dispatched with an empty
 * view. The VC-family events (START/STACK/ACK/SEQMSG/DATAGRAM/CREDIT, FC-P1.2's)
 * are NOT refused here: the channel table has no edge for any of them, so a post
 * lands on an empty cell and is counted in f->ignored_events -- which is the
 * auditable record that the channel layer answers nothing above the circuit.
 */
static int pe_event_needs_frame(enum pe_event ev)
{
	switch (ev) {
	case PE_EV_RX_HELLO:
	case PE_EV_RX_SOLICIT:
	case PE_EV_RX_VERIFY_B2:
	case PE_EV_RX_VERIFY_B3:
	case PE_EV_RX_VERIFY_B4:
	case PE_EV_RX_NEW_INCARNATION:
	case PE_EV_RX_LAST_GASP:
		return 1;
	default:
		return 0;
	}
}

enum pe_channel_action pe_fsm_event(struct pe_fsm *f, uint32_t index,
				    enum pe_event ev)
{
	struct pe_channel *ch = pe_fsm_channel_at(f, index);

	if (ch == NULL || pe_event_needs_frame(ev))
		return PE_CH_ACT_NONE;
	return pe_dispatch(f, ch, ev, &pe_rx_none);
}

void pe_fsm_link_up(struct pe_fsm *f)
{
	if (f == NULL)
		return;
	f->link_up = 1u;
	if (f->running)
		pe_arm_beat(f);
}

uint32_t pe_fsm_link_down(struct pe_fsm *f, struct pe_channel_rec *out,
			  uint32_t max)
{
	if (f == NULL)
		return 0u;
	f->link_up = 0u;
	return pe_broadcast(f, PE_EV_LINK_DOWN, out, max);
}

void pe_fsm_shutdown(struct pe_fsm *f)
{
	if (f == NULL)
		return;
	pe_fsm_stop(f);
	(void)pe_broadcast(f, PE_EV_SHUTDOWN, NULL, 0u);
}

int pe_fsm_send_last_gasp(struct pe_fsm *f)
{
	struct vms_hello_frame h;

	if (f == NULL)
		return -1;
	if (!pe_may_send(f) || !f->id.mcast_valid)
		return -1;

	/* SS4(O.30): byte-for-byte the periodic multicast HELLO except abs 30
	 * (a0 -> b1) and the cluster nonce at abs 68. Two fields, and this
	 * builder changes exactly those two. */
	pe_hello_multicast(f, &h, PE_PFW_LAST_GASP);
	pe_put_nonce(f, &h);
	if (pe_emit_plain(f, &h) != 0)
		return -1;
	f->last_gasps_built++;
	pe_log(f, "%PEA0, leaving the cluster, last gasp sent");
	return 0;
}

/* ==========================================================================
 * Projection -- the same struct the diagnostics ioctl hands userland
 * ========================================================================== */

void pe_fsm_channel_project(const struct pe_channel *ch,
			    struct vms_pe_channel_view *out)
{
	if (out == NULL)
		return;
	pe_bzero(out, (uint32_t)sizeof(*out));
	if (ch == NULL || !ch->in_use)
		return;

	pe_copy(out->remote_mac, ch->remote_mac, VMS_ETH_ADDR_LEN);
	out->state = ch->state;
	out->last_rx_ms = ch->last_rx_ms;
	out->hello_tx = ch->hello_tx;
	out->hello_rx = ch->hello_rx;
	out->verified_pktsz = ch->verified_pktsz;

	/* A sysid the executive never learned stays zero AND its flag stays
	 * clear, so a reader blanks the column rather than printing a node id
	 * nobody claimed (snapshot rule 2 / INV-6). */
	if (ch->remote_sysid_valid) {
		out->remote_sysid_valid = 1u;
		out->remote_sysid_lo = (uint32_t)(ch->remote_sysid & 0xffffffffu);
		out->remote_sysid_hi =
			(uint32_t)((ch->remote_sysid >> 32) & 0xffffffffu);
	}
}

/* ==========================================================================
 * Names -- the spelling the console and a failing test both use
 * ========================================================================== */

static const char *const pe_state_names[VMS_PE_CH_STATE__COUNT] = {
	"CLOSED", "SEEN", "B2", "B3", "B4"
};

static const char *const pe_action_names[PE_CH_ACT__COUNT] = {
	"none", "verified", "reset", "lost", "departed"
};

const char *pe_channel_state_name(enum vms_pe_channel_state s)
{
	if ((unsigned)s >= (unsigned)VMS_PE_CH_STATE__COUNT)
		return "?";
	return pe_state_names[s];
}

const char *pe_channel_action_name(enum pe_channel_action a)
{
	if ((unsigned)a >= (unsigned)PE_CH_ACT__COUNT)
		return "?";
	return pe_action_names[a];
}
