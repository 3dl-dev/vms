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

static int pe_tx_from(struct pe_fsm *f, const uint8_t *buf, uint32_t len)
{
	int st;

	if (f->ops == NULL || f->ops->send == NULL || buf == NULL)
		return -1;
	st = f->ops->send(f->ops->ctx, buf, len);
	if (st != 0) {
		f->tx_errors++;
		return -1;
	}
	return 0;
}

/* The common case: the frame the codec just built into the scratch buffer.
 * The VC's retransmit ladder is the other caller -- it re-sends a frame that
 * lives in the unacked ring, not in scratch, because scratch has moved on. */
static int pe_tx(struct pe_fsm *f, uint32_t len)
{
	return pe_tx_from(f, f->scratch, len);
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
	f->sysid = sysid;
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
 *
 * The port has two halves and this is where they meet. The CHANNEL half owns
 * the discovery family (HELLO/SOLICIT); the VIRTUAL-CIRCUIT half owns the SCS
 * family. Both are below; these three forward declarations exist because the
 * circuit half is written after the channel half it is built on.
 * ========================================================================== */
static void pe_vc_rx_frame(struct pe_fsm *f, const uint8_t *frame, uint32_t len,
			   const struct vms_frame_info *fi);
static void pe_vc_follow_channel(struct pe_fsm *f, uint32_t ch_index,
				 enum pe_channel_action act);
static void pe_vc_tick_all(struct pe_fsm *f);
static void pe_vc_broadcast(struct pe_fsm *f, enum pe_event ev);

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
	/* The SCS envelope is the VIRTUAL CIRCUIT's (FC-P1.2): formation,
	 * sequenced messages and credit-returns all ride it, and it is routed
	 * to the circuit on the channel the sending station owns. */
	if (fi.family == (uint8_t)VMS_FFAM_SCS) {
		pe_vc_rx_frame(f, frame, len, &fi);
		return PE_CH_ACT_NONE;
	}
	/* Anything that is neither family belongs to no layer this port has.
	 * Counted, not silently dropped, so a rising number is a question for
	 * a capture. */
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
	if (act == PE_CH_ACT_NONE)
		act = pe_dispatch(f, ch, pe_event_for(frame, len, &fi), &rx);
	/* (a reset supersedes whatever the frame asked for) */

	/* Whatever the channel concluded, the circuit riding it hears it. */
	pe_vc_follow_channel(f, (uint32_t)(ch - f->ch), act);
	return act;
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
		if (act == PE_CH_ACT_NONE)
			continue;
		pe_rec_emit(out, max, &n, i, act);
		pe_vc_follow_channel(f, i, act);
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
	/* One beat drives the circuits too: their deadlines are wrap-safe
	 * comparisons, so being driven by the HELLO cadence rather than by a
	 * per-circuit timer changes when they are CHECKED, never what they
	 * decide. It is also what lets a host test run TIMVCFAIL in
	 * microseconds with no timer implementation at all. */
	pe_vc_tick_all(f);
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
	/* The circuits go first: with no link there is no circuit, and the
	 * layers above must see vc_down before the channel records tell the
	 * connection manager the same thing. */
	pe_vc_broadcast(f, PE_EV_LINK_DOWN);
	return pe_broadcast(f, PE_EV_LINK_DOWN, out, max);
}

void pe_fsm_shutdown(struct pe_fsm *f)
{
	if (f == NULL)
		return;
	pe_fsm_stop(f);
	pe_vc_broadcast(f, PE_EV_SHUTDOWN);
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
 * ==========================================================================
 * THE VIRTUAL CIRCUIT (FC-P1.2)
 *
 * vms_pe_fsm.h SS3b is the contract: the START/STACK/ACK table from p. 2-14,
 * the incarnation echo that is the join GATE (SS4(i).B), the sequencing rule
 * from SS4(h)(4), and the one invariant this item exists to guarantee --
 * recv_ack NEVER FREEZES. Read it before changing anything below.
 *
 * The same discipline as the channel half: one table indexed [vc state][event],
 * one small handler per edge, every frame built through the FC-P1.1 codec and
 * not one byte offset in this file.
 * ==========================================================================
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * Sequence arithmetic
 *
 * The wire counter is 16 bits and it really does wrap: SS4(i).A caught a real
 * member's circuit continuing at send_seq 11974. So every comparison is a
 * SIGNED DIFFERENCE and never a `<`, exactly as pe_reached() is for the clock.
 * -------------------------------------------------------------------------- */

static int seq_after(uint16_t a, uint16_t b)
{
	return (int16_t)(a - b) > 0;
}

/*
 * The next sequence after `s`, skipping 0. SS4(h)(3) grounds send_seq == 0 as
 * "this frame carries no new sequence of its own" (622/622 credit-returns), so
 * 0 cannot also be a message's position. INFERRED: no capture reaches the
 * wrap, so which of 0 or 1 a real VMS uses there is unobserved -- this node
 * emits 1 and ACCEPTS either from a peer (below), which is the tolerant half
 * of the same choice.
 */
static uint16_t seq_next(uint16_t s)
{
	uint16_t n = (uint16_t)(s + 1u);

	return n == 0u ? 1u : n;
}

static int seq_is_next(uint16_t recv_seq, uint16_t got)
{
	return got == (uint16_t)(recv_seq + 1u) || got == seq_next(recv_seq);
}

/*
 * What a received sequence number IS. The ORDER of these tests is the rule:
 * anchor first (SS4(h)(4a): the first sequenced message on a circuit anchors
 * the counter rather than being scored -- without it every fresh circuit
 * scores its first frame as a gap and tears itself down), then the normal
 * advance, then a duplicate, and only what is left is a gap.
 */
static enum pe_vc_seq_kind vc_score_seq(const struct pe_vc *vc, uint16_t got)
{
	if (!vc->recv_anchored)
		return PE_VC_SEQ_ANCHOR;
	if (seq_is_next(vc->recv_seq, got))
		return PE_VC_SEQ_NEXT;
	if (!seq_after(got, vc->recv_seq))
		return PE_VC_SEQ_DUP;
	return PE_VC_SEQ_GAP;
}

/* -------------------------------------------------------------------------
 * Tunables, each resolved the same way the channel half resolves its own:
 * a SYSGEN-derived value from pe_identity if the glue loaded one, else the
 * documented OVMX default.
 * ------------------------------------------------------------------------- */

static uint32_t vc_timvcfail_ms(const struct pe_fsm *f)
{
	return f->id.timvcfail_ms != 0u ? f->id.timvcfail_ms
					: PE_TIMVCFAIL_DEFAULT_MS;
}

static uint32_t vc_retransmit_ms(const struct pe_fsm *f)
{
	uint32_t ms;

	if (f->id.vc_retransmit_ms != 0u)
		return f->id.vc_retransmit_ms;
	ms = vc_timvcfail_ms(f) / PE_VC_RETRANSMIT_DIVISOR;
	return ms < PE_VC_RETRANSMIT_MIN_MS ? PE_VC_RETRANSMIT_MIN_MS : ms;
}

/* The VMS absolute-time clock, and nothing else (design SS3.9 rule 6). */
static uint64_t pe_now_vms(const struct pe_fsm *f)
{
	if (f->ops != NULL && f->ops->now_vms != NULL)
		return f->ops->now_vms(f->ops->ctx);
	return 0u;
}

/* --------------------------------------------------------------------------
 * The circuit table
 * -------------------------------------------------------------------------- */

struct pe_vc *pe_fsm_vc_at(struct pe_fsm *f, uint32_t index)
{
	if (f == NULL || f->vc == NULL || index >= f->n_vcs)
		return NULL;
	return f->vc[index].in_use ? &f->vc[index] : NULL;
}

struct pe_vc *pe_fsm_vc_by_sysid(struct pe_fsm *f, vms_scs_sysid_t sysid)
{
	uint32_t i;

	if (f == NULL || f->vc == NULL || sysid == 0u)
		return NULL;
	for (i = 0; i < f->n_vcs; i++) {
		if (f->vc[i].in_use && f->vc[i].peer_sysid_valid &&
		    f->vc[i].peer_sysid == sysid)
			return &f->vc[i];
	}
	return NULL;
}

static struct pe_vc *vc_by_channel(struct pe_fsm *f, uint32_t ch_index)
{
	uint32_t i;

	if (f->vc == NULL)
		return NULL;
	for (i = 0; i < f->n_vcs; i++) {
		if (f->vc[i].in_use && f->vc[i].channel == (uint8_t)ch_index)
			return &f->vc[i];
	}
	return NULL;
}

static uint32_t vc_index_of(const struct pe_fsm *f, const struct pe_vc *vc)
{
	return (uint32_t)(vc - f->vc);
}

/* A circuit for a channel that has none. Like the channel table, a full table
 * REFUSES rather than recycling: evicting a live circuit to make room would
 * drop connections the executive still believes in. */
static struct pe_vc *vc_alloc(struct pe_fsm *f, uint32_t ch_index)
{
	uint32_t i;

	if (f->vc == NULL)
		return NULL;
	for (i = 0; i < f->n_vc_slots; i++) {
		struct pe_vc *vc = &f->vc[i];

		if (vc->in_use)
			continue;
		pe_bzero(vc, (uint32_t)sizeof(*vc));
		vc->in_use = 1u;
		vc->state = (uint8_t)VMS_PE_VC_CLOSED;
		vc->channel = (uint8_t)ch_index;
		if (i + 1u > f->n_vcs)
			f->n_vcs = i + 1u;
		return vc;
	}
	f->vc_no_slot++;
	return NULL;
}

static struct pe_vc *vc_find_or_alloc(struct pe_fsm *f, uint32_t ch_index)
{
	struct pe_vc *vc = vc_by_channel(f, ch_index);

	return vc != NULL ? vc : vc_alloc(f, ch_index);
}

/* --------------------------------------------------------------------------
 * Timers. Every deadline is armed through ops->arm_timer for production AND
 * kept as a wrap-safe field, so pe_fsm_tick's one beat can drive the same
 * work on a host test with no timer implementation at all.
 * -------------------------------------------------------------------------- */

static void vc_arm(struct pe_fsm *f, const struct pe_vc *vc,
		   enum pe_timer which, uint32_t ms)
{
	if (f->ops != NULL && f->ops->arm_timer != NULL)
		f->ops->arm_timer(f->ops->ctx, which, vc_index_of(f, vc), ms);
}

static void vc_cancel(struct pe_fsm *f, const struct pe_vc *vc,
		      enum pe_timer which)
{
	if (f->ops != NULL && f->ops->cancel_timer != NULL)
		f->ops->cancel_timer(f->ops->ctx, which, vc_index_of(f, vc));
}

/*
 * TIMVCFAIL -- "the time required for an SCS virtual circuit failure to be
 * detected". Armed as a NO-PROGRESS deadline, not a total-time one: every
 * time the peer's cumulative ack releases something, the deadline moves.
 * That is what makes it the detector for the campaign's actual failure --
 * a peer whose recv_ack stops advancing while this node keeps sending
 * (SS4(O.14)/(O.15)/(O.19)) -- instead of a stall with no end.
 */
static void vc_arm_vcfail(struct pe_fsm *f, struct pe_vc *vc)
{
	vc->vcfail_due_ms = pe_now(f) + vc_timvcfail_ms(f);
	vc->vcfail_armed = 1u;
	vc_arm(f, vc, PE_TIMER_VCFAIL, vc_timvcfail_ms(f));
}

static void vc_disarm_vcfail(struct pe_fsm *f, struct pe_vc *vc)
{
	if (!vc->vcfail_armed)
		return;
	vc->vcfail_armed = 0u;
	vc->vcfail_due_ms = 0u;
	vc_cancel(f, vc, PE_TIMER_VCFAIL);
}

/* --------------------------------------------------------------------------
 * Telling the layer above. SCS must see a circuit come and go; a received
 * message is delivered by (remote system, destination Con.ID), which is the
 * boundary vms_pe.h SS4 defines.
 * -------------------------------------------------------------------------- */

static void vc_notify_up(struct pe_fsm *f, const struct pe_vc *vc)
{
	if (f->upper != NULL && f->upper->vc_up != NULL)
		f->upper->vc_up(f->upper->ctx, vc->peer_sysid);
}

static void vc_notify_down(struct pe_fsm *f, const struct pe_vc *vc,
			   enum pe_vc_down_reason reason)
{
	if (f->upper != NULL && f->upper->vc_down != NULL)
		f->upper->vc_down(f->upper->ctx, vc->peer_sysid,
				  (uint32_t)reason);
}

/* --------------------------------------------------------------------------
 * The unacked ring, keyed by seq
 * -------------------------------------------------------------------------- */

static struct pe_vc_unacked *vc_ring_alloc(struct pe_vc *vc)
{
	uint32_t i;

	for (i = 0; i < PE_VC_UNACKED_MAX; i++) {
		if (!vc->ring[i].in_use) {
			pe_bzero(&vc->ring[i], (uint32_t)sizeof(vc->ring[i]));
			vc->ring[i].in_use = 1u;
			vc->unacked++;
			return &vc->ring[i];
		}
	}
	return NULL;
}

static void vc_ring_free(struct pe_vc *vc, struct pe_vc_unacked *e)
{
	e->in_use = 0u;
	if (vc->unacked > 0u)
		vc->unacked--;
}

static void vc_ring_clear(struct pe_vc *vc)
{
	uint32_t i;

	for (i = 0; i < PE_VC_UNACKED_MAX; i++)
		vc->ring[i].in_use = 0u;
	vc->unacked = 0u;
}

/*
 * The peer's CUMULATIVE acknowledgement releases everything at or below it.
 * Cumulative is the whole point: one ack for sequence N tells this node that
 * N and everything before it arrived, so a lost ACK costs nothing as long as
 * a later one arrives -- and every frame the peer sends carries one.
 *
 * ack == 0 releases nothing: SS4(h)(3)/(4) give 0 the meaning "no sequence
 * acknowledged" (a peer that has taken nothing yet), not "sequence zero".
 */
static void vc_release_acked(struct pe_fsm *f, struct pe_vc *vc, uint16_t ack)
{
	uint32_t i;
	int released = 0;

	if (ack == 0u)
		return;
	for (i = 0; i < PE_VC_UNACKED_MAX; i++) {
		struct pe_vc_unacked *e = &vc->ring[i];

		if (!e->in_use || seq_after(e->seq, ack))
			continue;
		vc_ring_free(vc, e);
		released = 1;
	}
	if (seq_after(ack, vc->peer_recv_ack) || vc->peer_recv_ack == 0u)
		vc->peer_recv_ack = ack;
	if (!released)
		return;
	/* Progress. Either there is nothing left to wait for, or the clock on
	 * what is left starts again from now. */
	if (vc->unacked == 0u)
		vc_disarm_vcfail(f, vc);
	else
		vc_arm_vcfail(f, vc);
}

/* --------------------------------------------------------------------------
 * Building this node's own circuit frames. Every field below comes from real
 * executive state or from this circuit's real counters; the codec bakes only
 * what the wire spec grounds as a node-independent protocol constant.
 * -------------------------------------------------------------------------- */

static int vc_fill_addr(const struct pe_fsm *f, const struct pe_channel *ch,
			struct vms_scs_addr *a)
{
	if (!pe_may_send(f) || !ch->remote_lavc_valid)
		return -1;
	pe_copy(a->dst_mac, ch->remote_mac, VMS_ETH_ADDR_LEN);
	pe_copy(a->src_mac, f->id.hw_mac, VMS_ETH_ADDR_LEN);
	pe_copy(a->dst_logical, ch->remote_lavc, VMS_ETH_ADDR_LEN);
	pe_copy(a->src_logical, f->id.lavc, VMS_ETH_ADDR_LEN);
	return 0;
}

/* SS4(g) phase 2 abs 104: a FIXED 8-byte blank-padded field, a different
 * encoding from the HELLO's length-prefixed name (28/28 frames, including a
 * 2-character name that did not shift the following bytes). */
static void vc_put_nodename(const struct pe_fsm *f,
			    uint8_t out[VMS_SCS_START_NODENAME_LEN])
{
	uint32_t n = f->id.scsnode_len;
	uint32_t i;

	if (n > (uint32_t)VMS_HELLO_NODENAME_MAX)
		n = (uint32_t)VMS_HELLO_NODENAME_MAX;
	for (i = 0; i < (uint32_t)VMS_SCS_START_NODENAME_LEN; i++)
		out[i] = (i < n && f->id.scsnode[i] != 0u) ? f->id.scsnode[i]
							  : (uint8_t)' ';
}

/*
 * May this node assert a formation body at all? Two of its fields are claims
 * about identity that CANNOT be defaulted (INV-6):
 *
 *   - the incarnation quadword at abs 80 is the time THIS system booted. A
 *     node advertising one it did not boot with earns a CLUEXIT bugcheck on
 *     the surviving side after a reconnect; OVMX shipped a replayed capture
 *     value for six days and that is the bug (spec SS4(g), vms-2f3).
 *   - the composition time at abs 112 must be LIVE: the spec's grounding for
 *     it is the negative "no real node ever sends a stale one", so without an
 *     absolute-time clock there is nothing honest to write.
 *
 * Absent either, this node forms NO circuit and counts it. That is the honest
 * end of the road (Rule 9), not a zero on the wire.
 */
static int vc_identity_ok(struct pe_fsm *f)
{
	if (!f->id.incarnation_time_valid || f->ops == NULL ||
	    f->ops->now_vms == NULL) {
		f->vc_no_identity++;
		return 0;
	}
	return 1;
}

static void vc_fill_identity(struct pe_fsm *f, struct vms_scs_start_frame *s)
{
	s->scssystemid = (uint16_t)(f->sysid & 0xffffu);
	pe_copy(s->software_version, f->id.sw_version,
		VMS_SCS_START_SWVER_LEN);
	pe_copy(s->hardware_type, f->id.hw_type, VMS_SCS_START_HWTYPE_LEN);
	s->credits = f->id.cluster_credits;
	vc_put_nodename(f, s->node_name);
	s->incarnation_time = f->id.incarnation_time;
	s->message_time = pe_now_vms(f);
}

/*
 * The START/STACK body. `round` is the config-round counter at abs 58, which
 * SS4(g) grounds as walking 0 -> 1 -> 2 across START, STACK and ACK; the
 * 106-byte wire shape is identical for START and STACK (p. 2-12: a STACK
 * re-supplies the same identity body).
 *
 * THE INCARNATION ECHO IS READ, NEVER CHOSEN. `vc->echo_incarnation` was
 * copied from the channel's `peer_incarnation`, which FC-P0.8 filled from a
 * real directed HELLO. With no echo there is no START -- see SS3b.
 */
static int vc_send_start(struct pe_fsm *f, struct pe_vc *vc, uint16_t round)
{
	struct vms_scs_start_frame s;
	struct pe_channel *ch = pe_fsm_channel_at(f, vc->channel);
	uint32_t written = 0u;

	if (ch == NULL)
		return -1;
	if (!vc->echo_valid) {
		f->vc_no_incarnation++;
		return -1;
	}
	if (!vc_identity_ok(f))
		return -1;

	pe_bzero(&s, (uint32_t)sizeof(s));
	if (vc_fill_addr(f, ch, &s.addr) != 0)
		return -1;
	/* The circuit's OWN counters, not the peer's: a fresh circuit reads
	 * send_seq 1 / recv_ack 0, which is byte-exactly what SS4(i).B says a
	 * joiner sends -- and SS4(i).A's warning ("must not copy the member's
	 * send_seq into its own") is structurally impossible here because
	 * nothing but this circuit's state is ever stamped. */
	s.recv_ack = vc->recv_seq;
	s.send_seq = vc->send_seq;
	s.incarnation = vc->echo_incarnation;
	s.config_round = round;
	vc_fill_identity(f, &s);

	if (vms_scs_start_build(&s, f->scratch, (uint32_t)sizeof(f->scratch),
				&written) != VMS_CODEC_OK)
		return -1;
	if (pe_tx(f, written) != 0)
		return -1;

	vc->config_round = (uint8_t)round;
	if (round == 0u)
		vc->starts_tx++;
	else
		vc->stacks_tx++;
	return 0;
}

/* The round-2 ACK: 46 bytes, no identity body (SS4(g) phase 2). */
static int vc_send_ack_frame(struct pe_fsm *f, struct pe_vc *vc)
{
	struct vms_scs_start_frame s;
	struct pe_channel *ch = pe_fsm_channel_at(f, vc->channel);
	uint32_t written = 0u;

	if (ch == NULL || !vc->echo_valid)
		return -1;
	pe_bzero(&s, (uint32_t)sizeof(s));
	if (vc_fill_addr(f, ch, &s.addr) != 0)
		return -1;
	s.recv_ack = vc->recv_seq;
	s.send_seq = vc->send_seq;
	s.incarnation = vc->echo_incarnation;

	if (vms_scs_start_build_ack(&s, f->scratch,
				    (uint32_t)sizeof(f->scratch),
				    &written) != VMS_CODEC_OK)
		return -1;
	if (pe_tx(f, written) != 0)
		return -1;
	vc->acks_tx++;
	return 0;
}

/*
 * ***  THE ACK  ***
 *
 * The 0x48 credit-return short (SS4(h)(3)): "[20:22] (send-seq) is 0 -- a
 * credit-return carries no new sequence number of its own; it purely
 * acknowledges", with the acknowledged sequence at abs 32 and its two
 * GROUNDED repeats. It carries BOTH obligations this node owes for a message
 * it has taken: the acknowledgement, and exactly one message's worth of
 * returned credit ("strict 1-for-1", SS4(h)(3)).
 *
 * It is emitted from the receive path the moment the sequence is scored, with
 * nothing conditional between -- no upper layer, no Con.ID, no SYSAP. See
 * vms_pe_fsm.h SS3b(a): acknowledgement is a transport fact.
 */
static void vc_send_ack(struct pe_fsm *f, struct pe_vc *vc)
{
	struct vms_scs_credit_frame c;
	struct pe_channel *ch = pe_fsm_channel_at(f, vc->channel);
	uint32_t written = 0u;

	if (ch == NULL)
		return;
	pe_bzero(&c, (uint32_t)sizeof(c));
	if (vc_fill_addr(f, ch, &c.addr) != 0)
		return;
	c.acked_seq = vc->recv_seq;
	/* abs 44 is the spec's own INFERRED "sender's own outstanding seq":
	 * filled from THIS node's counter, never echoed from a peer frame. */
	c.secondary_seq = vc->send_seq;

	if (vms_scs_credit_build(&c, f->scratch, (uint32_t)sizeof(f->scratch),
				 &written) != VMS_CODEC_OK)
		return;
	if (pe_tx(f, written) != 0)
		return;
	vc->credit_tx++;
	if (vc->recv_credit > 0u)
		vc->recv_credit--;    /* the credit is now back with the peer */
}

/* --------------------------------------------------------------------------
 * Circuit lifecycle
 * -------------------------------------------------------------------------- */

/*
 * SS4(h)(4a), the measurement's own rule: "per-VC counters are reset to 0 on
 * any 0x41 START in either direction" -- and SS4(i).A: "the post-START SCS VC
 * resets to send_seq = 1 on both sides". So formation, in EITHER direction,
 * is the point at which everything the old circuit knew is discarded.
 *
 * The peer's credit GRANT is discarded with it: a grant is a promise about a
 * circuit, and this is a different circuit. Until its new START/STACK body
 * arrives this node knows of no credit and will send nothing -- honest
 * refusal rather than an invented window (INV-6).
 */
static void vc_reset_sequence(struct pe_fsm *f, struct pe_vc *vc)
{
	vc_ring_clear(vc);
	vc->send_seq = 1u;
	vc->recv_seq = 0u;
	vc->recv_anchored = 0u;
	vc->peer_recv_ack = 0u;
	vc->send_credit = 0u;
	vc->send_credit_max = 0u;
	vc->recv_credit = 0u;
	vc->recv_credit_max = f->id.cluster_credits_valid
				      ? f->id.cluster_credits : 0u;
	vc->form_tries = 0u;
}

static void vc_close(struct pe_fsm *f, struct pe_vc *vc)
{
	vc_ring_clear(vc);
	vc_disarm_vcfail(f, vc);
	vc_cancel(f, vc, PE_TIMER_RETRANSMIT);
	vc->state = (uint8_t)VMS_PE_VC_CLOSED;
	vc->form_due_ms = 0u;
	vc->form_tries = 0u;
}

/* Start (or restart) formation: reset, send the round-0 START, arm both the
 * retry cadence and the TIMVCFAIL deadline. */
static void vc_begin_formation(struct pe_fsm *f, struct pe_vc *vc)
{
	vc_reset_sequence(f, vc);
	vc->state = (uint8_t)VMS_PE_VC_START_SENT;
	vc->form_due_ms = pe_now(f) + vc_retransmit_ms(f);
	vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));
	vc_arm_vcfail(f, vc);
	if (vc_send_start(f, vc, 0u) != 0)
		vc->state = (uint8_t)VMS_PE_VC_CLOSED;
}

static void vc_open(struct pe_fsm *f, struct pe_vc *vc)
{
	if (vc->state == (uint8_t)VMS_PE_VC_OPEN)
		return;
	vc->state = (uint8_t)VMS_PE_VC_OPEN;
	vc->opens++;
	vc->form_tries = 0u;
	vc_disarm_vcfail(f, vc);     /* nothing outstanding on a fresh circuit */
	pe_log(f, "%PEA0, virtual circuit open");
	vc_notify_up(f, vc);
}

/*
 * Break the circuit. p. 2-31: when the guarantee of delivery or of
 * sequentiality cannot be satisfied "the virtual circuit between the ports
 * involved will be explicitly broken … then every connection supported by
 * this virtual circuit is also broken" -- which is why the upper layer is
 * told before anything else happens.
 *
 * Then it RE-FORMS, if the channel it rides is still verified. A port whose
 * circuit failed and stayed down would be a node that silently left the
 * cluster; a real one keeps trying (SS4(k) watched a member retransmit its
 * formation probe indefinitely).
 */
static void vc_break(struct pe_fsm *f, struct pe_vc *vc,
		     enum pe_vc_down_reason reason, const char *why)
{
	struct pe_channel *ch = pe_fsm_channel_at(f, vc->channel);
	int was_up = (vc->state == (uint8_t)VMS_PE_VC_OPEN);

	vc->last_down_reason = (uint8_t)reason;
	vc->downs++;
	vc_close(f, vc);
	if (was_up)
		vc_notify_down(f, vc, reason);
	pe_log(f, why);

	if (ch != NULL && ch->state == (uint8_t)VMS_PE_CH_B4 &&
	    reason != PE_VC_DOWN_SHUTDOWN && reason != PE_VC_DOWN_PEER_GONE) {
		f->vc_reformations++;
		vc_begin_formation(f, vc);
	}
}

/* --------------------------------------------------------------------------
 * What a received SCS frame carried
 * -------------------------------------------------------------------------- */
struct pe_vc_rx {
	const struct vms_frame_info *fi;
	const uint8_t *frame;
	uint32_t       len;
	uint16_t       recv_ack;    /* what the peer acknowledges of OUR stream */
	uint16_t       send_seq;    /* the peer's own position, 0 = none        */
	uint8_t        has_seq;     /* this class carries a new sequence        */
	uint8_t        is_start;    /* the 0x41 class                           */
	uint8_t        conid_valid;
	uint8_t        pad;
	vms_conid_t    dst_conid;
	struct vms_scs_start_frame start;   /* valid iff is_start               */
};

static const struct pe_vc_rx pe_vc_rx_none;

/* --------------------------------------------------------------------------
 * The transition handlers -- one edge each
 * -------------------------------------------------------------------------- */

/* Learn what a 106-byte START/STACK body says about the peer. All of it is
 * real wire data, and each piece keeps its own validity: a 46-byte ACK
 * carries no body and teaches nothing. */
static void vc_learn_peer(struct pe_fsm *f, struct pe_vc *vc,
			  const struct pe_vc_rx *rx)
{
	const struct vms_scs_start_frame *s = &rx->start;

	if (s->is_ack)
		return;
	vc->peer_sysid = (vms_scs_sysid_t)s->scssystemid;
	vc->peer_sysid_valid = (uint8_t)(s->scssystemid != 0u);
	pe_copy(vc->peer_name, s->node_name, VMS_SCS_START_NODENAME_LEN);
	vc->peer_name_valid = 1u;
	pe_copy(vc->peer_swver, s->software_version, VMS_SCS_START_SWVER_LEN);
	pe_copy(vc->peer_hwtype, s->hardware_type, VMS_SCS_START_HWTYPE_LEN);
	vc->peer_incarnation_time = s->incarnation_time;
	vc->peer_ident_valid = 1u;

	/* p. 2-43: the Send Credit this node may use is the one the PEER
	 * granted, and abs 95 is byte-exact to its SYSGEN CLUSTER_CREDITS
	 * (SS4(g)). Read, never assumed -- with no grant, nothing is sent. */
	vc->send_credit_max = s->credits;
	vc->send_credit = s->credits;
	(void)f;
}

/*
 * A START arrived. p. 2-14: in START SENT and in START RECEIVED the response
 * is the same -- send a STACK -- and SS4(h)(4a) adds that a START in EITHER
 * direction resets the circuit's counters. On an OPEN circuit that means the
 * peer has torn its side down and is re-forming, so the old circuit is
 * finished and the layers above are told before the new one starts.
 */
static void h_vc_rx_start(struct pe_fsm *f, struct pe_vc *vc,
			  const struct pe_vc_rx *rx)
{
	int was_up = (vc->state == (uint8_t)VMS_PE_VC_OPEN);

	vc->starts_rx++;
	if (was_up) {
		vc->last_down_reason = (uint8_t)PE_VC_DOWN_PEER_RESTART;
		vc->downs++;
		vc_notify_down(f, vc, PE_VC_DOWN_PEER_RESTART);
		pe_log(f, "%PEA0, peer re-started the circuit, re-forming");
	}
	vc_reset_sequence(f, vc);
	vc_learn_peer(f, vc, rx);
	vc->state = (uint8_t)VMS_PE_VC_STACK_SENT;
	vc->form_due_ms = pe_now(f) + vc_retransmit_ms(f);
	vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));
	vc_arm_vcfail(f, vc);
	if (vc_send_start(f, vc, 1u) != 0)
		vc->state = (uint8_t)VMS_PE_VC_CLOSED;
}

/* A STACK arrived: p. 2-14, the circuit is OPEN and an ACK goes back. In OPEN
 * this is the peer re-sending a STACK whose ACK it never saw; the ACK is sent
 * again and nothing else changes (idempotent). */
static void h_vc_rx_stack(struct pe_fsm *f, struct pe_vc *vc,
			  const struct pe_vc_rx *rx)
{
	vc->stacks_rx++;
	vc_learn_peer(f, vc, rx);
	(void)vc_send_ack_frame(f, vc);
	vc_open(f, vc);
}

/*
 * An ACK arrived. In START RECEIVED it opens the circuit. In OPEN it is
 * discarded, and that is the BOOK's behaviour, not laziness: p. 2-12, "each
 * port driver simply discards the ACK it receives … because it already
 * considers the virtual circuit to be OPEN". Counted either way.
 */
static void h_vc_rx_ack(struct pe_fsm *f, struct pe_vc *vc,
			const struct pe_vc_rx *rx)
{
	(void)rx;
	vc->acks_rx++;
	vc_open(f, vc);
}

/* Deliver a received message upward, best effort and strictly AFTER the ack.
 *
 * The whole FRAME goes up, not a slice of it: the SCS layer reads the
 * connection-control type, the credit field, the Con.ID pair and the SYSAP
 * body through its OWN codec entries, and a port that pre-sliced them would
 * be asserting offsets it has no business knowing (design SS3.9 rule 2).
 *
 * A class whose Con.ID location the codec does not GROUND is counted, not
 * delivered with an invented one (SS4(d): the other length classes "do not
 * reliably match this layout and are therefore left undecoded"). FC-P2.1
 * grounds more of them; until it does, this counter is the honest measure of
 * what this node receives and cannot yet route. The ACK went out regardless,
 * so the peer's circuit never notices (SS3b(a)).
 */
static void vc_deliver(struct pe_fsm *f, struct pe_vc *vc,
		       const struct pe_vc_rx *rx)
{
	if (!rx->conid_valid || f->upper == NULL || f->upper->message == NULL) {
		f->vc_rx_undelivered++;
		return;
	}
	f->upper->message(f->upper->ctx, vc->peer_sysid, rx->dst_conid,
			  rx->frame, rx->len);
}

/*
 * ***  THE SEQUENCED MESSAGE -- the heart of this item  ***
 *
 * The order of these five steps IS the anti-freeze guarantee, and none of
 * them may be made conditional on anything above the port:
 *
 *   1. release what the peer's cumulative ack covers (even for a duplicate
 *      or a gap: the ack it carries is still true);
 *   2. score the sequence (anchor / next / duplicate / gap);
 *   3. a GAP breaks the circuit -- p. 2-31, and SS4(h)(4a) measured that a
 *      real VAX never produces one (0 of 321,599);
 *   4. ACKNOWLEDGE, before any delivery and with no upper layer required;
 *   5. deliver -- and a duplicate is never delivered twice.
 */
static void h_vc_rx_seqmsg(struct pe_fsm *f, struct pe_vc *vc,
			   const struct pe_vc_rx *rx)
{
	enum pe_vc_seq_kind kind;

	vc->msgs_rx++;
	vc_release_acked(f, vc, rx->recv_ack);          /* 1 */

	kind = vc_score_seq(vc, rx->send_seq);          /* 2 */
	if (kind == PE_VC_SEQ_GAP) {                    /* 3 */
		vc->rx_gaps++;
		vc_break(f, vc, PE_VC_DOWN_SEQ_GAP,
			 "%PEA0, sequenced message out of order, circuit broken");
		return;
	}
	if (kind != PE_VC_SEQ_DUP) {
		vc->recv_seq = rx->send_seq;
		vc->recv_anchored = 1u;
		if (vc->recv_credit < 0xffu)
			vc->recv_credit++;
	}

	vc_send_ack(f, vc);                             /* 4 */

	if (kind == PE_VC_SEQ_DUP) {
		vc->rx_dups++;
		return;
	}
	vc_deliver(f, vc, rx);                          /* 5 */
}

/*
 * A 0x48 credit-return from the peer: an acknowledgement of OUR stream plus
 * exactly one message's worth of Send Credit back (SS4(h)(3), strict 1-for-1).
 * It carries no sequence of its own and is never itself acknowledged --
 * acking an ack is how two ports talk forever about nothing.
 */
static void h_vc_rx_credit(struct pe_fsm *f, struct pe_vc *vc,
			   const struct pe_vc_rx *rx)
{
	vc->credit_rx++;
	vc_release_acked(f, vc, rx->recv_ack);
	if (vc->send_credit < vc->send_credit_max)
		vc->send_credit++;
}

/* A datagram: unsequenced and unacknowledged by definition (p. 2-31 gives the
 * delivery guarantees to the MESSAGE service only). Delivered straight up. No
 * frame class the codec grounds today produces this event; it exists because
 * the port's service set does, and the glue can post it. */
static void h_vc_rx_datagram(struct pe_fsm *f, struct pe_vc *vc,
			     const struct pe_vc_rx *rx)
{
	if (rx->frame == NULL) {
		/* Posted without the frame that justified it -- there is
		 * nothing to deliver, and inventing an empty datagram for the
		 * layer above would be worse than counting it. */
		f->vc_rx_undelivered++;
		return;
	}
	if (f->upper != NULL && f->upper->datagram != NULL)
		f->upper->datagram(f->upper->ctx, vc->peer_sysid,
				   rx->frame, rx->len);
	else
		f->vc_rx_undelivered++;
}

/*
 * p. 2-16, the IMPLIED ACK: in START RECEIVED, any packet that requires a
 * circuit opens it -- the peer would not be sending data on a circuit it did
 * not consider open, so the ACK that would have opened it is redundant. The
 * packet is then processed normally, which is the point of the rule.
 */
static void h_vc_implied_open(struct pe_fsm *f, struct pe_vc *vc,
			      const struct pe_vc_rx *rx)
{
	vc->implied_acks++;
	pe_log(f, "%PEA0, implied acknowledgement, virtual circuit open");
	vc_open(f, vc);

	if (rx->fi == NULL)
		return;
	if (rx->fi->cls == (uint8_t)VMS_FCLS_SCS_CREDIT)
		h_vc_rx_credit(f, vc, rx);
	else if (rx->has_seq)
		h_vc_rx_seqmsg(f, vc, rx);
}

/*
 * The formation retry. p. 2-14 gives the port "a timer and an OS-dependent
 * retry limit"; here the limit is TIMVCFAIL (below), so this handler simply
 * re-sends the frame the current state is waiting on, at the same config
 * round -- a re-sent START is still round 0, which is what makes the member's
 * round counter, not ours, drive the handshake (SS4(i).A).
 */
static void h_vc_form_retry(struct pe_fsm *f, struct pe_vc *vc,
			    const struct pe_vc_rx *rx)
{
	uint32_t now = pe_now(f);

	(void)rx;
	if (vc->vcfail_armed && pe_reached(now, vc->vcfail_due_ms)) {
		vc_break(f, vc, PE_VC_DOWN_TIMVCFAIL,
			 "%PEA0, no response within TIMVCFAIL, circuit re-formed");
		return;
	}
	if (!pe_reached(now, vc->form_due_ms))
		return;
	vc->form_tries++;
	vc->form_due_ms = now + vc_retransmit_ms(f);
	vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));
	(void)vc_send_start(f, vc,
			    vc->state == (uint8_t)VMS_PE_VC_START_SENT ? 0u : 1u);
}

/*
 * Re-send one unacknowledged message. SS4(L): a message "consumes the channel
 * send_seq exactly once, and retransmissions REUSE that same send_seq". Two
 * things are refreshed and nothing else:
 *
 *   - recv_ack, so a retransmission also carries this node's LATEST
 *     acknowledgement (a retransmit that carried a stale ack would be the
 *     freeze this whole item exists to prevent, arriving by the back door);
 *   - the msgtype, 0x4b/0x5b -> 0x7b, which is the wire's own retransmit
 *     marking (SS4(h), SS4(O.19)).
 */
static void vc_resend_one(struct pe_fsm *f, struct pe_vc *vc,
			  struct pe_vc_unacked *e, uint32_t now)
{
	struct vms_frame_info fi;

	if (vms_frame_classify(e->frame, e->len, &fi) != VMS_CODEC_OK)
		return;
	if (vms_scs_seq_mark_retransmit(e->frame, e->len, &fi) != VMS_CODEC_OK)
		return;
	if (vms_frame_classify(e->frame, e->len, &fi) != VMS_CODEC_OK)
		return;
	if (vms_scs_seq_stamp(e->frame, e->len, &fi, vc->recv_seq,
			      e->seq) != VMS_CODEC_OK)
		return;
	if (pe_tx_from(f, e->frame, e->len) != 0)
		return;
	e->retransmits++;
	e->due_ms = now + vc_retransmit_ms(f);
	vc->retransmits++;
}

/*
 * The retransmit ladder on an OPEN circuit, and the TIMVCFAIL check that ends
 * it. TIMVCFAIL is tested FIRST: once the deadline has passed, re-sending
 * again would just add to a conversation the peer has stopped having.
 */
static void h_vc_retransmit(struct pe_fsm *f, struct pe_vc *vc,
			    const struct pe_vc_rx *rx)
{
	uint32_t now = pe_now(f);
	uint32_t i;

	(void)rx;
	if (vc->unacked == 0u)
		return;
	if (vc->vcfail_armed && pe_reached(now, vc->vcfail_due_ms)) {
		vc_break(f, vc, PE_VC_DOWN_TIMVCFAIL,
			 "%PEA0, peer stopped acknowledging, circuit re-formed");
		return;
	}
	for (i = 0; i < PE_VC_UNACKED_MAX; i++) {
		struct pe_vc_unacked *e = &vc->ring[i];

		if (e->in_use && pe_reached(now, e->due_ms))
			vc_resend_one(f, vc, e, now);
	}
	vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));
}

/* The TIMVCFAIL timer, if the glue arms one per circuit rather than letting
 * the retransmit beat carry the check. Same deadline, same wrap-safe test. */
static void h_vc_vcfail(struct pe_fsm *f, struct pe_vc *vc,
			const struct pe_vc_rx *rx)
{
	(void)rx;
	if (!vc->vcfail_armed || !pe_reached(pe_now(f), vc->vcfail_due_ms))
		return;
	vc_break(f, vc, PE_VC_DOWN_TIMVCFAIL,
		 "%PEA0, TIMVCFAIL expired, circuit re-formed");
}

/* The channel reached b4. This is the ONLY thing that starts a formation. */
static void h_vc_channel_up(struct pe_fsm *f, struct pe_vc *vc,
			    const struct pe_vc_rx *rx)
{
	struct pe_channel *ch = pe_fsm_channel_at(f, vc->channel);

	(void)rx;
	if (ch == NULL)
		return;
	/* SS4(i).B: the echo is the member's number for US, taken from a real
	 * directed HELLO. Copied at formation so the whole handshake carries
	 * one consistent value even if the channel learns a new one mid-way
	 * (which is itself a channel RESET, and tears this circuit down). */
	vc->echo_incarnation = ch->peer_incarnation;
	vc->echo_valid = ch->peer_incarnation_valid;
	if (ch->remote_sysid_valid && !vc->peer_sysid_valid) {
		vc->peer_sysid = ch->remote_sysid;
		vc->peer_sysid_valid = 1u;
	}
	vc_begin_formation(f, vc);
}

/* The channel stopped being verified (a peer re-form, spec SS4(i).B, or the
 * SS4(M) listen timeout). Design SS3.4: the circuit rides the channel, so it
 * goes with it -- and it does NOT re-form here, because there is no verified
 * channel to form it over. The next CHANNEL_UP starts it again. */
static void h_vc_channel_down(struct pe_fsm *f, struct pe_vc *vc,
			      const struct pe_vc_rx *rx)
{
	int was_up = (vc->state == (uint8_t)VMS_PE_VC_OPEN);

	(void)rx;
	vc->last_down_reason = (uint8_t)PE_VC_DOWN_CHANNEL;
	vc->downs++;
	vc_close(f, vc);
	if (was_up) {
		pe_log(f, "%PEA0, channel lost, virtual circuit closed");
		vc_notify_down(f, vc, PE_VC_DOWN_CHANNEL);
	}
}

/* SS4(O.30): the peer announced its departure. p. 7-29: on a last gasp the
 * port driver "immediately closes the virtual circuit … then notifies all
 * SYSAPs". No re-formation: the node said it was leaving. */
static void h_vc_peer_gone(struct pe_fsm *f, struct pe_vc *vc,
			   const struct pe_vc_rx *rx)
{
	int was_up = (vc->state == (uint8_t)VMS_PE_VC_OPEN);

	(void)rx;
	vc->last_down_reason = (uint8_t)PE_VC_DOWN_PEER_GONE;
	vc->downs++;
	vc_close(f, vc);
	if (was_up) {
		pe_log(f, "%PEA0, peer departed, virtual circuit closed");
		vc_notify_down(f, vc, PE_VC_DOWN_PEER_GONE);
	}
}

/* CLUSTER_STOP. */
static void h_vc_shutdown(struct pe_fsm *f, struct pe_vc *vc,
			  const struct pe_vc_rx *rx)
{
	int was_up = (vc->state == (uint8_t)VMS_PE_VC_OPEN);

	(void)rx;
	vc->last_down_reason = (uint8_t)PE_VC_DOWN_SHUTDOWN;
	vc_close(f, vc);
	if (was_up) {
		vc->downs++;
		vc_notify_down(f, vc, PE_VC_DOWN_SHUTDOWN);
	}
}

/* A circuit-requiring frame arrived with no circuit to take it. p. 2-31: SCS
 * control messages need no connection, but they do need a VC. Counted, never
 * answered -- answering would assert a circuit this node does not have. */
static void h_vc_no_circuit(struct pe_fsm *f, struct pe_vc *vc,
			    const struct pe_vc_rx *rx)
{
	(void)vc; (void)rx;
	f->vc_rx_no_circuit++;
}

/* Nothing to do, and NORMAL: a closed circuit still gets every timer beat and
 * every link bounce. Explicit rather than an empty cell, so the ignored-event
 * counter keeps meaning "an event the spec does not connect to this state". */
static void h_vc_idle(struct pe_fsm *f, struct pe_vc *vc,
		      const struct pe_vc_rx *rx)
{
	(void)f; (void)vc; (void)rx;
}

/* ==========================================================================
 * The VC table. [vc state][pe_event]; an empty cell is an event p. 2-14's
 * acceptable-response table does not connect to that state -- ignored and
 * COUNTED (f->vc_ignored_events), never guessed at. An ACK in START SENT is
 * the clearest example: the book's table does not admit it, so this machine
 * does not either, and the retry ladder handles the consequence.
 * ========================================================================== */
typedef void (*pe_vc_handler_t)(struct pe_fsm *, struct pe_vc *,
				const struct pe_vc_rx *);

static const pe_vc_handler_t
pe_vc_table[VMS_PE_VC_STATE__COUNT][PE_EV__COUNT] = {
	/* [CLOSED] no circuit. Only a verified channel, or the peer's own
	 * START, brings one into being. */
	[VMS_PE_VC_CLOSED] = {
		[PE_EV_CHANNEL_UP]      = h_vc_channel_up,
		[PE_EV_RX_START]        = h_vc_rx_start,      /* p. 2-12   */
		[PE_EV_RX_SEQMSG]       = h_vc_no_circuit,
		[PE_EV_RX_DATAGRAM]     = h_vc_no_circuit,
		[PE_EV_RX_CREDIT]       = h_vc_no_circuit,
		[PE_EV_TIMER_RETRANSMIT] = h_vc_idle,
		[PE_EV_TIMER_VCFAIL]    = h_vc_idle,
		[PE_EV_CHANNEL_DOWN]    = h_vc_idle,
		[PE_EV_LINK_DOWN]       = h_vc_idle,
		[PE_EV_RX_LAST_GASP]    = h_vc_idle,
		[PE_EV_SHUTDOWN]        = h_vc_idle,
	},

	/* [START SENT] our START is out. p. 2-14's acceptable responses are
	 * START (both ends started: send a STACK) and STACK (open, ack). */
	[VMS_PE_VC_START_SENT] = {
		[PE_EV_RX_START]        = h_vc_rx_start,
		[PE_EV_RX_STACK]        = h_vc_rx_stack,
		[PE_EV_RX_SEQMSG]       = h_vc_no_circuit,
		[PE_EV_RX_DATAGRAM]     = h_vc_no_circuit,
		[PE_EV_RX_CREDIT]       = h_vc_no_circuit,
		[PE_EV_TIMER_RETRANSMIT] = h_vc_form_retry,
		[PE_EV_TIMER_VCFAIL]    = h_vc_vcfail,
		[PE_EV_CHANNEL_DOWN]    = h_vc_channel_down,
		[PE_EV_LINK_DOWN]       = h_vc_channel_down,
		[PE_EV_RX_LAST_GASP]    = h_vc_peer_gone,
		[PE_EV_SHUTDOWN]        = h_vc_shutdown,
	},

	/* [STACK SENT] the book's START RECEIVED: our STACK is out and the
	 * circuit opens on an ACK, on a STACK, or -- p. 2-16 -- on any packet
	 * that requires a circuit at all. */
	[VMS_PE_VC_STACK_SENT] = {
		[PE_EV_RX_ACK]          = h_vc_rx_ack,
		[PE_EV_RX_STACK]        = h_vc_rx_stack,
		[PE_EV_RX_START]        = h_vc_rx_start,      /* re-send STACK */
		[PE_EV_RX_SEQMSG]       = h_vc_implied_open,  /* p. 2-16   */
		[PE_EV_RX_DATAGRAM]     = h_vc_implied_open,
		[PE_EV_RX_CREDIT]       = h_vc_implied_open,
		[PE_EV_TIMER_RETRANSMIT] = h_vc_form_retry,
		[PE_EV_TIMER_VCFAIL]    = h_vc_vcfail,
		[PE_EV_CHANNEL_DOWN]    = h_vc_channel_down,
		[PE_EV_LINK_DOWN]       = h_vc_channel_down,
		[PE_EV_RX_LAST_GASP]    = h_vc_peer_gone,
		[PE_EV_SHUTDOWN]        = h_vc_shutdown,
	},

	/* [OPEN] the sequenced conversation. */
	[VMS_PE_VC_OPEN] = {
		[PE_EV_RX_SEQMSG]       = h_vc_rx_seqmsg,
		[PE_EV_RX_CREDIT]       = h_vc_rx_credit,
		[PE_EV_RX_DATAGRAM]     = h_vc_rx_datagram,
		[PE_EV_RX_START]        = h_vc_rx_start,   /* peer re-formed */
		[PE_EV_RX_STACK]        = h_vc_rx_stack,   /* re-ack, stay   */
		[PE_EV_RX_ACK]          = h_vc_rx_ack,     /* p. 2-12 discard */
		[PE_EV_TIMER_RETRANSMIT] = h_vc_retransmit,
		[PE_EV_TIMER_VCFAIL]    = h_vc_vcfail,
		[PE_EV_CHANNEL_DOWN]    = h_vc_channel_down,
		[PE_EV_LINK_DOWN]       = h_vc_channel_down,
		[PE_EV_RX_LAST_GASP]    = h_vc_peer_gone,
		[PE_EV_SHUTDOWN]        = h_vc_shutdown,
	},
};

static void pe_vc_dispatch(struct pe_fsm *f, struct pe_vc *vc,
			   enum pe_event ev, const struct pe_vc_rx *rx)
{
	pe_vc_handler_t h;

	if (f == NULL || vc == NULL || !vc->in_use)
		return;
	if ((unsigned)ev >= (unsigned)PE_EV__COUNT)
		return;
	if ((unsigned)vc->state >= (unsigned)VMS_PE_VC_STATE__COUNT)
		return;

	h = pe_vc_table[vc->state][ev];
	if (h == NULL) {
		f->vc_ignored_events++;
		return;
	}
	h(f, vc, rx);
}

/* ==========================================================================
 * Receiving an SCS frame: classify, decode, dispatch
 * ========================================================================== */

/* Which pe_event a received SCS frame IS. The CODEC decides, through the
 * class gate and the typed parse -- this file reads no byte offset. The 0x41
 * class splits three ways on what the frame itself carries: the 46-byte
 * shape is the ACK, and the config round (SS4(g): 0 -> 1 -> 2) tells START
 * from STACK. */
static enum pe_event vc_event_for(const struct pe_vc_rx *rx)
{
	if (rx->is_start) {
		if (rx->start.is_ack)
			return PE_EV_RX_ACK;
		return rx->start.config_round == 0u ? PE_EV_RX_START
						    : PE_EV_RX_STACK;
	}
	if (rx->fi->cls == (uint8_t)VMS_FCLS_SCS_CREDIT)
		return PE_EV_RX_CREDIT;
	return PE_EV_RX_SEQMSG;
}

/* Decode an SCS frame into the shared view. Each field comes from the class's
 * own typed accessor and is left absent when the class does not ground it. */
static int vc_parse(const uint8_t *frame, uint32_t len,
		    const struct vms_frame_info *fi, struct pe_vc_rx *rx)
{
	struct vms_scs_credit_frame credit;
	uint32_t remote = 0u, local = 0u;
	uint16_t recv_ack = 0u, send_seq = 0u;

	rx->fi = fi;
	rx->frame = frame;
	rx->len = len;

	if (fi->cls == (uint8_t)VMS_FCLS_SCS_START) {
		if (vms_scs_start_parse(frame, len, fi, &rx->start) !=
		    VMS_CODEC_OK)
			return -1;
		rx->is_start = 1u;
		rx->recv_ack = rx->start.recv_ack;
		rx->send_seq = rx->start.send_seq;
		return 0;
	}
	if (fi->cls == (uint8_t)VMS_FCLS_SCS_CREDIT) {
		if (vms_scs_credit_parse(frame, len, fi, &credit) !=
		    VMS_CODEC_OK)
			return -1;
		/* SS4(h)(3): a credit-return acknowledges and carries no
		 * sequence of its own (send_seq == 0, 622/622). */
		rx->recv_ack = credit.acked_seq;
		return 0;
	}
	if (vms_scs_seq(frame, len, fi, &recv_ack, &send_seq) != VMS_CODEC_OK)
		return -1;
	rx->recv_ack = recv_ack;
	rx->send_seq = send_seq;
	rx->has_seq = 1u;
	if (vms_scs_conid(frame, len, fi, &remote, &local) == VMS_CODEC_OK) {
		/* The Con.ID the SENDER addressed: on a received frame that is
		 * the "remote" field, which holds THIS node's own connection
		 * identifier as the peer knows it (SS4(d), GROUNDED against
		 * SDA SHOW CONNECTIONS). */
		rx->dst_conid = (vms_conid_t)remote;
		rx->conid_valid = 1u;
	}
	return 0;
}

/*
 * An SCS-family frame arrived. It is bound to a CIRCUIT through the CHANNEL
 * the sending station owns -- a station this port has never heard a HELLO
 * from has no channel, and a frame from it is counted and dropped rather
 * than allowed to conjure a circuit out of nothing.
 */
static void pe_vc_rx_frame(struct pe_fsm *f, const uint8_t *frame, uint32_t len,
			   const struct vms_frame_info *fi)
{
	struct vms_sca_hdr hdr;
	struct pe_vc_rx rx;
	struct pe_channel *ch;
	struct pe_vc *vc;

	/* No circuit table bound: this port is FC-P0.8's port, channels only.
	 * Every SCS frame is then a frame there is no circuit to take, and it
	 * is counted as exactly that -- not silently dropped. */
	if (f->vc == NULL) {
		f->vc_rx_no_circuit++;
		return;
	}
	if (vms_sca_hdr_parse(frame, len, &hdr) != VMS_CODEC_OK) {
		f->vc_rx_parse_failed++;
		return;
	}
	ch = pe_fsm_channel_by_mac(f, hdr.eth_src);
	if (ch == NULL) {
		f->vc_rx_no_channel++;
		return;
	}
	/* An SCS frame is evidence the station is alive, exactly as a HELLO is,
	 * so it refreshes the SS4(M) listen deadline. Without this a channel
	 * carrying a BUSY circuit would time out on its own traffic. */
	ch->last_rx_ms = pe_now(f);
	ch->deadline_ms = ch->last_rx_ms + pe_listen_timeout(f);

	pe_bzero(&rx, (uint32_t)sizeof(rx));
	if (vc_parse(frame, len, fi, &rx) != 0) {
		f->vc_rx_parse_failed++;
		return;
	}

	vc = vc_by_channel(f, (uint32_t)(ch - f->ch));
	if (vc == NULL) {
		/* The peer opened the conversation. Only its START may create
		 * a circuit -- anything else is data for a circuit that does
		 * not exist, and is counted. */
		if (vc_event_for(&rx) != PE_EV_RX_START) {
			f->vc_rx_no_circuit++;
			return;
		}
		vc = vc_find_or_alloc(f, (uint32_t)(ch - f->ch));
		if (vc == NULL)
			return;
		vc->echo_incarnation = ch->peer_incarnation;
		vc->echo_valid = ch->peer_incarnation_valid;
	}
	pe_vc_dispatch(f, vc, vc_event_for(&rx), &rx);
}

/*
 * The channel layer reached a conclusion about connectivity; the circuit on
 * that channel has to hear it. This is the one place the two halves of the
 * port meet, and it is a one-way street: the circuit never tells the channel
 * anything.
 */
static void pe_vc_follow_channel(struct pe_fsm *f, uint32_t ch_index,
				 enum pe_channel_action act)
{
	struct pe_vc *vc;

	if (f->vc == NULL || act == PE_CH_ACT_NONE)
		return;
	if (act == PE_CH_ACT_VERIFIED) {
		vc = vc_find_or_alloc(f, ch_index);
		if (vc == NULL)
			return;
		pe_vc_dispatch(f, vc, PE_EV_CHANNEL_UP, &pe_vc_rx_none);
		return;
	}
	vc = vc_by_channel(f, ch_index);
	if (vc == NULL)
		return;
	pe_vc_dispatch(f, vc,
		       act == PE_CH_ACT_DEPARTED ? PE_EV_RX_LAST_GASP
						 : PE_EV_CHANNEL_DOWN,
		       &pe_vc_rx_none);
}

/* ==========================================================================
 * The VC public surface
 * ========================================================================== */

void pe_fsm_bind_vcs(struct pe_fsm *f, struct pe_vc *vcs, uint32_t n)
{
	if (f == NULL)
		return;
	f->vc = (n != 0u) ? vcs : NULL;
	f->n_vc_slots = (vcs != NULL) ? n : 0u;
	f->n_vcs = 0u;
}

void pe_fsm_set_upper(struct pe_fsm *f, const struct pe_upper_ops *upper)
{
	if (f != NULL)
		f->upper = upper;
}

int pe_vc_addr(struct pe_fsm *f, vms_scs_sysid_t dst, struct vms_scs_addr *out)
{
	struct pe_vc *vc = pe_fsm_vc_by_sysid(f, dst);
	struct pe_channel *ch;

	if (vc == NULL || out == NULL)
		return -1;
	ch = pe_fsm_channel_at(f, vc->channel);
	if (ch == NULL)
		return -1;
	pe_bzero(out, (uint32_t)sizeof(*out));
	return vc_fill_addr(f, ch, out);
}

/*
 * Give a caller-built sequenced frame its position on the circuit and send it.
 *
 * The order matters and is the "no hole in send_seq" guarantee (SS3b(1)): the
 * frame is classified and stamped, the ring entry is taken, and ONLY THEN is
 * send_seq advanced -- so a frame this port refuses never consumes a sequence
 * number, and a frame the substrate refuses keeps both its number and its ring
 * entry so the retransmit ladder fills the position rather than leaving a gap
 * the peer would break the circuit over.
 */
int pe_vc_send_frame(struct pe_fsm *f, vms_scs_sysid_t dst,
		     const uint8_t *frame, uint32_t len)
{
	struct vms_frame_info fi;
	struct pe_vc_unacked *e;
	struct pe_vc *vc;
	uint16_t seq;

	if (f == NULL || frame == NULL)
		return PE_VC_SEND_BADFRAME;
	vc = pe_fsm_vc_by_sysid(f, dst);
	if (vc == NULL || vc->state != (uint8_t)VMS_PE_VC_OPEN)
		return PE_VC_SEND_NOCIRCUIT;
	if (len > PE_VC_FRAME_MAX)
		return PE_VC_SEND_TOOBIG;
	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return PE_VC_SEND_BADFRAME;
	/* p. 2-43: no credit, no message. The grant is the PEER's, read from
	 * its formation body; this node never invents a window for itself. */
	if (vc->send_credit == 0u) {
		vc->send_refused_credit++;
		return PE_VC_SEND_NOCREDIT;
	}

	seq = vc->send_seq;
	e = vc_ring_alloc(vc);
	if (e == NULL) {
		vc->send_refused_ring++;
		return PE_VC_SEND_RINGFULL;
	}
	pe_copy(e->frame, frame, len);
	e->len = len;
	e->seq = seq;
	if (vms_scs_seq_stamp(e->frame, e->len, &fi, vc->recv_seq, seq) !=
	    VMS_CODEC_OK) {
		vc_ring_free(vc, e);          /* nothing was sent, nothing */
		return PE_VC_SEND_BADFRAME;   /* was consumed              */
	}

	vc->send_seq = seq_next(seq);
	vc->send_credit--;
	vc->msgs_tx++;
	e->due_ms = pe_now(f) + vc_retransmit_ms(f);
	if (!vc->vcfail_armed)
		vc_arm_vcfail(f, vc);
	vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));

	if (pe_tx_from(f, e->frame, e->len) != 0)
		return PE_VC_SEND_TXFAIL;     /* held for the ladder */
	return PE_VC_SEND_OK;
}

void pe_fsm_vc_timer(struct pe_fsm *f, uint32_t index)
{
	struct pe_vc *vc = pe_fsm_vc_at(f, index);

	if (vc == NULL)
		return;
	pe_vc_dispatch(f, vc, PE_EV_TIMER_RETRANSMIT, &pe_vc_rx_none);
}

/*
 * The RX_* events carry evidence -- the parsed frame that justified them --
 * and pe_fsm_rx is the only place that evidence exists, so a bare post of one
 * is refused rather than dispatched with an empty view. The same rule the
 * channel half applies, for the same reason.
 */
static int pe_vc_event_needs_frame(enum pe_event ev)
{
	switch (ev) {
	case PE_EV_RX_START:
	case PE_EV_RX_STACK:
	case PE_EV_RX_ACK:
	case PE_EV_RX_SEQMSG:
	case PE_EV_RX_CREDIT:
		return 1;
	default:
		return 0;
	}
}

void pe_fsm_vc_event(struct pe_fsm *f, uint32_t index, enum pe_event ev)
{
	struct pe_vc *vc = pe_fsm_vc_at(f, index);

	if (vc == NULL || pe_vc_event_needs_frame(ev))
		return;
	pe_vc_dispatch(f, vc, ev, &pe_vc_rx_none);
}

/* Every live circuit's timer work, from the one port beat. */
static void pe_vc_tick_all(struct pe_fsm *f)
{
	uint32_t i;

	if (f->vc == NULL)
		return;
	for (i = 0; i < f->n_vcs; i++) {
		if (f->vc[i].in_use)
			pe_vc_dispatch(f, &f->vc[i], PE_EV_TIMER_RETRANSMIT,
				       &pe_vc_rx_none);
	}
}

static void pe_vc_broadcast(struct pe_fsm *f, enum pe_event ev)
{
	uint32_t i;

	if (f->vc == NULL)
		return;
	for (i = 0; i < f->n_vcs; i++) {
		if (f->vc[i].in_use)
			pe_vc_dispatch(f, &f->vc[i], ev, &pe_vc_rx_none);
	}
}

void pe_fsm_vc_project(const struct pe_fsm *f, const struct pe_vc *vc,
		       struct vms_pe_vc_view *out)
{
	uint32_t now;

	if (out == NULL)
		return;
	pe_bzero(out, (uint32_t)sizeof(*out));
	if (vc == NULL || !vc->in_use)
		return;

	if (vc->peer_sysid_valid) {
		out->peer_sysid_lo = (uint32_t)(vc->peer_sysid & 0xffffffffu);
		out->peer_sysid_hi =
			(uint32_t)((vc->peer_sysid >> 32) & 0xffffffffu);
	}
	out->state = vc->state;
	out->send_seq = vc->send_seq;
	out->recv_seq = vc->recv_seq;
	/* What this node PUTS on the wire as its cumulative acknowledgement is
	 * recv_seq itself (SS4(h)(4): "recv_ack = recv_seq"), so the two view
	 * columns are the same executive cell read twice -- never a separate
	 * number that could drift from what was actually sent. */
	out->recv_ack = vc->recv_seq;
	out->peer_recv_ack = vc->peer_recv_ack;
	out->unacked = vc->unacked;
	out->retransmits = vc->retransmits;
	/* The peer's incarnation quadword, as ITS formation body carried it;
	 * zero with no ident learned, never a placeholder. */
	if (vc->peer_ident_valid) {
		out->incarnation_lo =
			(uint32_t)(vc->peer_incarnation_time & 0xffffffffu);
		out->incarnation_hi =
			(uint32_t)((vc->peer_incarnation_time >> 32) &
				   0xffffffffu);
	}
	if (vc->vcfail_armed && f != NULL) {
		now = pe_now(f);
		out->timvcfail_ms_left =
			pe_reached(now, vc->vcfail_due_ms)
				? 0u : (vc->vcfail_due_ms - now);
	}
	out->credits_send = vc->send_credit;
	out->credits_receive = vc->recv_credit;
}

static const char *const pe_vc_state_names[VMS_PE_VC_STATE__COUNT] = {
	"CLOSED", "START SENT", "STACK SENT", "OPEN"
};

const char *pe_vc_state_name(enum vms_pe_vc_state s)
{
	if ((unsigned)s >= (unsigned)VMS_PE_VC_STATE__COUNT)
		return "?";
	return pe_vc_state_names[s];
}

const char *pe_vc_down_reason_name(enum pe_vc_down_reason r)
{
	switch (r) {
	case PE_VC_DOWN_SEQ_GAP:      return "sequence gap";
	case PE_VC_DOWN_TIMVCFAIL:    return "TIMVCFAIL";
	case PE_VC_DOWN_CHANNEL:      return "channel lost";
	case PE_VC_DOWN_PEER_RESTART: return "peer restarted";
	case PE_VC_DOWN_PEER_GONE:    return "peer departed";
	case PE_VC_DOWN_SHUTDOWN:     return "shutdown";
	default:                      return "?";
	}
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
