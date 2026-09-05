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

/*
 * The abs 47-67 DISCOVERY FORMAT span (SS4(a) "capability/version-ish" +
 * "unknown"). LEARNED off a real peer, exactly like the join nonce below --
 * see vms_pe_fsm.h SS4's E56 note for the capture pair that pins it. Absent
 * means the codec's zero goes out and is COUNTED; nothing here invents bytes.
 */
static void pe_put_disc_format(struct pe_fsm *f, struct vms_hello_frame *h)
{
	if (!f->id.disc_format_valid) {
		f->disc_format_absent++;
		return;
	}
	pe_copy(h->disc.cap_span, f->id.cap_span, VMS_DISC_CAPSPAN_LEN);
	pe_copy(h->disc.reserved_64, f->id.reserved_64,
		VMS_DISC_RESERVED64_LEN);
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
	pe_put_disc_format(f, h);

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
 * shared token, LEARNED live off a real peer's own directed frame by
 * pe_learn_join_nonce() below (spec SS4(g): the token is the same cluster-wide
 * value on every node, so hearing it once from any peer is enough). Absent
 * means a zero goes out and is COUNTED -- never a token replayed out of a
 * stored capture file.
 */
static void pe_put_nonce(struct pe_fsm *f, struct vms_hello_frame *h)
{
	if (f->id.join_nonce_valid) {
		pe_copy(h->disc.nonce, f->id.join_nonce, VMS_DISC_NONCE_LEN);
		return;
	}
	f->nonce_absent++;
}

/* True iff every byte of a VMS_DISC_NONCE_LEN span is zero. */
static int pe_span_is_zero(const uint8_t *p, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (p[i] != 0u)
			return 0;
	}
	return 1;
}

static int pe_nonce_is_zero(const uint8_t n[VMS_DISC_NONCE_LEN])
{
	return pe_span_is_zero(n, VMS_DISC_NONCE_LEN);
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

	/* The bank every credit this port advertises is drawn against (§4b).
	 * Its size is the buffer count the glue read off the pool the fork
	 * context really allocated -- not a number this file chose. */
	pe_credit_init(&f->credit, f->id.rx_pool_bufs);
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

/*
 * Learn the cluster join nonce off a real peer's own frame, the moment one
 * arrives carrying a non-zero value (spec SS4(a)/SS4(g)): a directed HELLO
 * from an established member ALREADY carries the live cluster-wide token in
 * the clear (E55 wire observation,
 * `tests/lab/captures/e53-group257-refire-20260903.pcap`: VAX1's directed b2
 * to this node carries a constant non-zero nonce on every one of 310 frames).
 * This is the SAME learn-from-the-wire discipline `pe_channel_learn()` above
 * already uses for `remote_lavc` -- read once from a live peer, in THIS
 * session, never a value this file invented or copied out of a stored
 * capture file (INV-6; Rule 8: the (group#, password) -> nonce hash itself is
 * unpublished and is never computed here -- this is "the cluster's on-wire
 * assignment" instead). Learned once per run: SS4(g) grounds the token as
 * constant cluster-wide, so re-learning would only re-assert the same bytes.
 */
static void pe_learn_join_nonce(struct pe_fsm *f, const struct pe_rx *rx)
{
	if (f->id.join_nonce_valid || rx->disc == NULL)
		return;
	if (pe_nonce_is_zero(rx->disc->nonce))
		return;
	pe_copy(f->id.join_nonce, rx->disc->nonce, VMS_DISC_NONCE_LEN);
	f->id.join_nonce_valid = 1u;
	f->nonce_learned = 1u;
}

/*
 * E56 -- learn the abs 47-67 DISCOVERY FORMAT span the same way, off the first
 * real peer's discovery frame that carries a non-zero one. This is the join
 * gate, not cosmetics: with the span zeroed an established member completes
 * the whole SS4(a).1 b2/b3/b4 channel verify with this node and then never
 * opens a circuit at all (E55 re-fire, 0 member STARTs in 242 s), whereas with
 * it present the same member emits its round-0 0x41 START unprompted 10 ms
 * after b4 (`ovmx-5fe-channel-formed-20260728.pcap`). The value is node-
 * INDEPENDENT -- byte-identical on VAX1/VAX2/VAX3 and on the OVMX build that
 * reached MEMBER -- so hearing it once from any peer is enough, exactly as for
 * the cluster-wide nonce, and echoing it asserts nothing about THIS system
 * (name, SCSSYSTEMID, hardware MAC, incarnation and software version all live
 * in other fields and stay this node's own).
 */
static void pe_learn_disc_format(struct pe_fsm *f, const struct pe_rx *rx)
{
	if (f->id.disc_format_valid || rx->disc == NULL)
		return;
	if (pe_span_is_zero(rx->disc->cap_span, VMS_DISC_CAPSPAN_LEN))
		return;
	pe_copy(f->id.cap_span, rx->disc->cap_span, VMS_DISC_CAPSPAN_LEN);
	pe_copy(f->id.reserved_64, rx->disc->reserved_64,
		VMS_DISC_RESERVED64_LEN);
	f->id.disc_format_valid = 1u;
	f->disc_format_learned = 1u;
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
	pe_learn_join_nonce(f, &rx);
	pe_learn_disc_format(f, &rx);

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
 * FC-P1.9 corrected the LOSS half of it to design SS3.2.5's go-back-N ruling:
 * a receive window of 1 that DISCARDS and RE-ACKS on a gap instead of breaking,
 * a sender that goes back N from its oldest unacked entry, and a bounded ladder
 * whose exhaustion -- not a first out-of-order frame -- is what breaks the
 * circuit. SS3b(4) of the header carries the reasoning.
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
 * What a received sequence number IS: the normal advance, then a duplicate,
 * and what is left is a gap.
 *
 * ***  THERE IS NO "ANCHOR ON THE FIRST FRAME" HERE, AND THAT IS DELIBERATE.
 *
 * FC-P1.2 imported one, citing SS4(h)(4a). Read that passage again: the
 * anchor is a property of the CAPTURE SCANNER -- "a capture, like a node
 * attaching to a circuit already carrying traffic, cannot know what preceded
 * the first frame it sees ... Without that anchor the same scan reports 147
 * gaps; the extra 106 are all anchor cases (the first sequenced message seen
 * on a VC, typically at a capture that starts mid-stream)". A PORT IS NOT IN
 * THAT POSITION: it FORMED this circuit, and SS4(i).A grounds where the
 * circuit starts -- "the post-START SCS VC resets to send_seq = 1 on both
 * sides and runs the SS4h lockstep byte-identical to fresh (0 residuals)".
 * vc_reset_sequence puts recv_seq at 0 on exactly that authority, so the
 * peer's first sequenced message IS scored, as the 1 it must be.
 *
 * Anchoring on whatever arrives first is not merely redundant here, it LOSES
 * DATA: if the first message of a circuit is lost, the second one becomes the
 * anchor, recv_seq jumps to it, this node acknowledges a message it never
 * received, and the sender's ring releases it. FC-P1.9's R2 acceptance caught
 * exactly that (2 of 48 pipelined messages silently never delivered at 10 %
 * loss, seed 3), and under go-back-N it is unnecessary as well as wrong: a
 * first frame that really is lost is now a GAP, which costs a discard and a
 * re-ack and is repaired by the sender's ladder.
 *
 * A peer that violated SS4(i).A and opened at some other number would gap
 * forever and be broken by its own exhausted ladder -- loudly, and with a
 * reason, which is the honest end of the road (Rule 9) rather than silently
 * adopting a position this node cannot justify (INV-6).
 */
static enum pe_vc_seq_kind vc_score_seq(const struct pe_vc *vc, uint16_t got)
{
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

/*
 * The retransmit cadence. With no SYSGEN-derived value the default spreads the
 * whole PE_VC_RETRANSMIT_TRIES ladder across TIMVCFAIL, so ladder exhaustion --
 * the detector design SS3.2.5 puts in charge of "the peer is not
 * acknowledging" -- lands before the silence timer that would otherwise steal
 * the reason. See the constants' comment in vms_pe_fsm.h.
 */
static uint32_t vc_retransmit_ms(const struct pe_fsm *f)
{
	uint32_t ms;

	if (f->id.vc_retransmit_ms != 0u)
		return f->id.vc_retransmit_ms;
	ms = vc_timvcfail_ms(f) / PE_VC_RETRANSMIT_DIVISOR;
	return ms < PE_VC_RETRANSMIT_MIN_MS ? PE_VC_RETRANSMIT_MIN_MS : ms;
}

/*
 * THE CIRCUIT'S abs-36 VALUE, IN ONE PLACE (E66).
 *
 * SS4(i).B's node-incarnation echo is not a property of a message class, it is
 * a property of the CIRCUIT: a real node stamps the same number on its 0x41
 * START/STACK/ACK, on every 0x4b/0x5b/0x7b sequenced message and on every 0x48
 * credit-return it sends over that circuit, and it changes only when the peer
 * advertises a new one (af2-firsttimer: a joiner walks 1->2->3 across all three
 * classes in lockstep with the member's directed-HELLO advertisement). So every
 * builder reads it HERE, from `vc->echo_incarnation` -- the value the channel
 * copied out of a real directed HELLO at formation (h_vc_channel_up) -- and no
 * builder holds a constant of its own.
 *
 * 0 means "this circuit has no echo". The codec REFUSES a zero rather than
 * writing one (INV-6), so the caller's only job is to count the refusal.
 */
static uint16_t vc_incarnation(const struct pe_vc *vc)
{
	return vc->echo_valid ? vc->echo_incarnation : 0u;
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
		/* E82: explicit, so a REUSED slot can never inherit another
		 * system's high-water marks -- acking on a fresh circuit from
		 * a number the previous peer sent is the E76 assertion. */
		cm_guard_init(&vc->guard);
		if (i + 1u > f->n_vcs)
			f->n_vcs = i + 1u;
		return vc;
	}
	f->vc_no_slot++;
	return NULL;
}

/* ==========================================================================
 * THE CIRCUIT IS WITH THE SYSTEM; THE CHANNEL IS ITS PATH (E83)
 *
 * WHAT THE BUG WAS. This file used to key a circuit on the CHANNEL: one
 * pe_vc per remote Ethernet address. A real VMS node is reachable at more
 * than one LAN address at once -- its hardware address and the logical
 * `AA-00-04-00-xx-yy` address DECnet programs into the same adapter -- so a
 * single VAX gave this port SEVERAL channels, and therefore several circuits,
 * all naming the same SCSSYSTEMID. SCS keys its System Block on the SYSTEM, so
 * when ANY of those circuits went down, scs_fsm_vc_down() walked that one
 * System Block and closed EVERY connection to that node with path-lost --
 * including connections riding a channel that was perfectly alive.
 *
 * MEASURED (integration note E83, join-e80refire-1788563452.pcap): three
 * Ethernet sources -- 08:00:2b:1e:85:61, 08:00:2b:7a:fa:e2 and
 * aa:00:04:00:01:04 -- all carried the logical address aa:00:04:00:01:04 at
 * abs 24, i.e. SCSSYSTEMID 1025, node VAX1. One of them went quiet at
 * t=449.249; twenty seconds later (§4(M)) its channel timed out, its circuit
 * broke, and an ACCEPTED VMS$VAXcluster connection on ANOTHER channel to the
 * SAME VAX was closed path-lost -- 17.4 s after this node had advertised on
 * it, while the VAX still held it. The wire proves the surviving circuit never
 * went down: its very next frame carried send_seq 15, continuing 14, with no
 * 0x41 and no counter reset.
 *
 * THE MODEL. The design says it in one line -- the port "builds VIRTUAL
 * CIRCUITS (VCs) between nodes over ONE OR MORE LAN paths (channels)"
 * (docs/design-cluster-node.md), and vms_pe.h §4 says the port "forms a
 * VIRTUAL CIRCUIT with the remote SYSTEM". So: ONE circuit per remote system,
 * carried over one or more verified channels; the bound channel is the path it
 * is using now; the circuit fails when the LAST path does, not the first.
 *
 * INV-6. Every judgement below rests on something really read: the peer's own
 * logical LAVC address off its own frame (pe_channel_learn), the b2/b3/b4
 * ladder's own conclusion, that channel's own §4(M) deadline, and the
 * incarnation the peer really advertised. A channel that never carried a
 * logical address has NO system as far as this port is concerned, and is never
 * merged onto another circuit on a guess.
 * ========================================================================== */

/*
 * IS THIS CHANNEL A PATH THIS PORT MAY STILL USE? Two measured facts and no
 * assumption: the ladder reached b4 (§4(a).1, the channel is VERIFIED) and the
 * station is still inside its own §4(M) listen deadline. The deadline is
 * re-tested rather than left to the state byte, because the channel beat and
 * the circuit beat are separate: a station whose last frame is older than the
 * listen timeout is not alive merely because nobody has run its timer yet.
 */
static int pe_channel_is_live(const struct pe_channel *ch, uint32_t now)
{
	if (ch == NULL || !ch->in_use)
		return 0;
	if (ch->state != (uint8_t)VMS_PE_CH_B4)
		return 0;
	return !pe_reached(now, ch->deadline_ms);
}

/* The channel a circuit is riding right now, or NULL if that slot is gone. */
static struct pe_channel *vc_path(struct pe_fsm *f, const struct pe_vc *vc)
{
	return pe_fsm_channel_at(f, vc->channel);
}

/* The circuit belonging to the SYSTEM this channel reaches, or NULL -- either
 * because the channel has never carried a logical address (so this port does
 * not know what system it is) or because that system has no circuit yet. */
static struct pe_vc *vc_by_channel_system(struct pe_fsm *f,
					  const struct pe_channel *ch)
{
	if (ch == NULL || !ch->remote_sysid_valid)
		return NULL;
	return pe_fsm_vc_by_sysid(f, ch->remote_sysid);
}

/*
 * Another LIVE channel to `sysid`, other than `except`. Index, or -1.
 *
 * THE LINK IS TESTED FIRST, and it is not belt-and-braces. pe_fsm_link_down()
 * deliberately tells the CIRCUITS before it tells the channels ("the layers
 * above must see vc_down before the channel records say the same thing"), so at
 * that instant every channel is still sitting in b4. Without this test a link
 * bounce would find a dozen "live" alternates and hold up circuits that have no
 * wire under them at all -- which is the exact fabrication of liveness this
 * whole change is not allowed to commit.
 */
static int32_t pe_alt_path(struct pe_fsm *f, vms_scs_sysid_t sysid,
			   uint32_t except, uint32_t now)
{
	uint32_t i;

	if (sysid == 0u || !f->link_up)
		return -1;
	for (i = 0; i < f->n_channels; i++) {
		const struct pe_channel *ch = &f->ch[i];

		if (i == except || !ch->remote_sysid_valid)
			continue;
		if (ch->remote_sysid != sysid)
			continue;
		if (pe_channel_is_live(ch, now))
			return (int32_t)i;
	}
	return -1;
}

/*
 * MOVE A CIRCUIT ONTO ANOTHER CHANNEL TO THE SAME SYSTEM. Nothing about the
 * CIRCUIT changes -- not its state, not its sequence numbers, not its unacked
 * ring, not the credit its peer granted -- because none of those belong to the
 * LAN path. What changes is only which station's addresses the next frame is
 * built with (vc_fill_addr reads them from the bound channel).
 */
static void vc_rebind_path(struct pe_fsm *f, struct pe_vc *vc,
			   uint32_t ch_index)
{
	vc->channel = (uint8_t)ch_index;
	vc->path_moves++;
	f->vc_path_moves++;
}

/*
 * MAY THIS CIRCUIT CONTINUE ON `ch`? §4(i).B: a directed HELLO carries the
 * incarnation the peer attributes to US, and a CHANGE in it means the peer
 * regards this node as a new generation -- which is a channel RESET, and tears
 * the circuit down. So a path is a continuation of THIS circuit only if it
 * advertises the same number the circuit has been stamping on every frame it
 * sends. A different number is not a failover; it is the peer having re-formed,
 * and the circuit then breaks exactly as it did before this rule existed.
 */
static int vc_path_continues(const struct pe_vc *vc,
			     const struct pe_channel *ch)
{
	if (!vc->echo_valid)
		return 1;               /* nothing yet that a path could contradict */
	return ch->peer_incarnation_valid &&
	       ch->peer_incarnation == vc->echo_incarnation;
}

/*
 * THE CIRCUIT THIS CHANNEL BELONGS TO. Returns the circuit to use, or NULL
 * when this channel must not have one:
 *   - the circuit table is full (vc_alloc counts it), or
 *   - the system already has a circuit on a path that is still LIVE, so this
 *     channel is a second ROUTE to it (counted in vc_paths_redundant) and a
 *     second circuit would be the E83 defect all over again.
 */
static struct pe_vc *vc_for_path(struct pe_fsm *f, uint32_t ch_index)
{
	struct pe_channel *ch = pe_fsm_channel_at(f, ch_index);
	struct pe_vc *vc = vc_by_channel(f, ch_index);

	if (vc != NULL)
		return vc;
	vc = vc_by_channel_system(f, ch);
	if (vc == NULL)
		return vc_alloc(f, ch_index);
	if (pe_channel_is_live(vc_path(f, vc), pe_now(f))) {
		f->vc_paths_redundant++;
		return NULL;
	}
	vc_rebind_path(f, vc, ch_index);
	return vc;
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
 * The OLDEST unacknowledged entry: the lowest sequence still outstanding.
 *
 * The ring is a slot pool, not an ordered queue -- vc_ring_alloc takes the
 * first free slot -- so "oldest" is a question about the SEQUENCE, answered
 * with the same signed-difference comparison every other sequence test in this
 * file uses. Go-back-N starts here and the ladder is bounded here: this entry
 * is the hole the receiver is stuck behind.
 */
static struct pe_vc_unacked *vc_ring_oldest(struct pe_vc *vc)
{
	struct pe_vc_unacked *best = NULL;
	uint32_t i;

	for (i = 0; i < PE_VC_UNACKED_MAX; i++) {
		struct pe_vc_unacked *e = &vc->ring[i];

		if (!e->in_use)
			continue;
		if (best == NULL || seq_after(best->seq, e->seq))
			best = e;
	}
	return best;
}

/* The next outstanding entry after `seq`, in sequence order: go-back-N resends
 * the tail IN ORDER, because a receiver with a window of 1 takes them in no
 * other. NULL at the end of the ring. */
static struct pe_vc_unacked *vc_ring_next_after(struct pe_vc *vc, uint16_t seq)
{
	struct pe_vc_unacked *best = NULL;
	uint32_t i;

	for (i = 0; i < PE_VC_UNACKED_MAX; i++) {
		struct pe_vc_unacked *e = &vc->ring[i];

		if (!e->in_use || !seq_after(e->seq, seq))
			continue;
		if (best == NULL || seq_after(best->seq, e->seq))
			best = e;
	}
	return best;
}

/* --------------------------------------------------------------------------
 * SEND CREDIT -- the peer's grant, spent by a message and returned by that
 * message's ACKNOWLEDGEMENT, whichever frame class carries it
 *
 * THE INVARIANT this pair keeps, and the one pe_vc_send_frame() spends against:
 *
 *     send_credit + unacked == send_credit_max
 *
 * i.e. the window LEFT plus the messages still outstanding is exactly the grant
 * the peer made. It holds by construction: a send debits one and rings one; an
 * ack releases k and returns k; a retransmit re-sends bytes already rung and
 * already debited, so it moves neither.
 * -------------------------------------------------------------------------- */

/*
 * Return `n` messages' worth of the peer's grant, never past the grant itself.
 * Clamping DOWN is the direction that matters: a window wider than the peer
 * granted is a promise this node was never given (p. 2-43's bank rule read from
 * the other side), and a uint8 that wrapped would be exactly that.
 */
static void vc_credit_return(struct pe_vc *vc, uint32_t n)
{
	uint32_t c = (uint32_t)vc->send_credit + n;

	if (c > (uint32_t)vc->send_credit_max)
		c = (uint32_t)vc->send_credit_max;
	vc->send_credit = (uint8_t)c;
}

/*
 * The peer's grant, taken from its START/STACK body (abs 95). The window it
 * opens is the grant MINUS whatever is still outstanding, so the invariant
 * above survives the one case where this runs on a circuit that is already
 * carrying traffic: h_vc_rx_stack() re-learns the peer on an OPEN circuit when
 * the peer re-sends a STACK whose ACK it never saw, and a flat
 * `send_credit = grant` there would hand this node back credit it had spent on
 * messages the peer has not yet acknowledged.
 */
static void vc_credit_grant(struct pe_vc *vc, uint8_t grant)
{
	vc->send_credit_max = grant;
	vc->send_credit = (grant > vc->unacked)
				  ? (uint8_t)(grant - vc->unacked)
				  : (uint8_t)0u;
}

/*
 * The peer's CUMULATIVE acknowledgement releases everything at or below it.
 * Cumulative is the whole point: one ack for sequence N tells this node that
 * N and everything before it arrived, so a lost ACK costs nothing as long as
 * a later one arrives -- and every frame the peer sends carries one.
 *
 * ...WHICH IS ALSO WHY THE CREDIT COMES BACK HERE (E75). One message costs one
 * credit, and it is the ACKNOWLEDGEMENT of that message that returns it: the
 * peer has taken it out of the buffer it granted. This node had been returning
 * credit ONLY on the standalone 0x48 credit-return class, and the golden wire
 * refutes that reading decisively: 94-99% of every acknowledgement a real node
 * receives rides PIGGYBACKED on the peer's own sequenced message, not on a
 * 0x48. §4(h)(3)'s "strict 1-for-1" is a true statement about the 0x48 CLASS
 * -- it returns exactly one message's worth -- and was wrongly read here as the
 * only channel credit comes back on. The figures, both captures and the
 * re-derivation command are in vms_pe_fsm.h's CREDIT section.
 *
 * ack == 0 releases nothing: SS4(h)(3)/(4) give 0 the meaning "no sequence
 * acknowledged" (a peer that has taken nothing yet), not "sequence zero".
 */
static void vc_release_acked(struct pe_fsm *f, struct pe_vc *vc, uint16_t ack)
{
	uint32_t i;
	uint32_t released = 0u;

	if (ack == 0u)
		return;
	for (i = 0; i < PE_VC_UNACKED_MAX; i++) {
		struct pe_vc_unacked *e = &vc->ring[i];

		if (!e->in_use || seq_after(e->seq, ack))
			continue;
		vc_ring_free(vc, e);
		released++;
	}
	if (seq_after(ack, vc->peer_recv_ack) || vc->peer_recv_ack == 0u)
		vc->peer_recv_ack = ack;
	if (released == 0u)
		return;
	/* Exactly as many credits as there were messages: a duplicate ack
	 * releases nothing and therefore returns nothing, so no acknowledgement
	 * can be counted twice however many frames repeat it. */
	vc_credit_return(vc, released);
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

/*
 * The two formation-body fields that assert what this node HAS, rather than
 * who it is: the software version it broadcasts at abs 72 and the Send Credit
 * it grants at abs 95. Neither has a default here -- with nothing to assert,
 * zero goes out and the omission is COUNTED, exactly as the discovery-format
 * span is (E56/E57, INV-6). What a peer sent is NEVER a source for either: a
 * VAX's "VMS V7.3" is that VAX's identity, and its credits are that VAX's
 * buffers.
 *
 * THE CREDIT IS READ OFF THE CIRCUIT, NOT OFF THE CONFIGURATION (E60).
 * vc->recv_credit_max is what pe_credit_reserve() committed to THIS circuit
 * out of the port's real receive-buffer pool (§4b). SYSGEN CLUSTER_CREDITS is
 * upstream of that -- it is how many buffers the operator asked for, and the
 * frame carries how many were actually got. When they differ, the wire tells
 * the truth, because the peer is going to send exactly this many messages
 * without waiting and every one of them needs somewhere to land.
 */
static void vc_fill_advertised(struct pe_fsm *f, const struct pe_vc *vc,
			       struct vms_scs_start_frame *s)
{
	if (f->id.sw_version_valid)
		pe_copy(s->software_version, f->id.sw_version,
			VMS_SCS_START_SWVER_LEN);
	else
		f->vc_sw_version_absent++;

	/* Whatever the reservation is, that is what goes out -- and a grant of
	 * zero is COUNTED however it arose (nothing configured, nothing
	 * requested, or a pool already fully promised): on the wire all three
	 * say the same thing to the peer, and a circuit that can take no
	 * message must not be an invisible one. */
	s->credits = vc->recv_credit_max;
	if (s->credits == 0u)
		f->vc_credits_absent++;
}

static void vc_fill_identity(struct pe_fsm *f, const struct pe_vc *vc,
			     struct vms_scs_start_frame *s)
{
	s->scssystemid = (uint16_t)(f->sysid & 0xffffu);
	vc_fill_advertised(f, vc, s);
	pe_copy(s->hardware_type, f->id.hw_type, VMS_SCS_START_HWTYPE_LEN);
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
	vc_fill_identity(f, vc, &s);

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
	if (!vc->echo_valid) {
		/* No SS4(i).B echo: abs 36 would have to be invented. Count it
		 * and send nothing -- a credit-return carrying a made-up
		 * incarnation is exactly the frame a member discards (E66). */
		f->vc_no_incarnation++;
		return;
	}
	pe_bzero(&c, (uint32_t)sizeof(c));
	if (vc_fill_addr(f, ch, &c.addr) != 0)
		return;
	c.acked_seq = vc->recv_seq;
	/* abs 44 is the spec's own INFERRED "sender's own outstanding seq":
	 * filled from THIS node's counter, never echoed from a peer frame. */
	c.secondary_seq = vc->send_seq;
	c.incarnation = vc_incarnation(vc);

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
 * The receive-buffer credit ledger (vms_pe_fsm.h §4b, p. 2-42/2-43/2-45)
 *
 * Four small operations over two counters. The whole reason it is a ledger
 * and not an expression is the invariant in pe_credit_reserve(): the sum of
 * everything this port has promised can never exceed the buffers it owns.
 * -------------------------------------------------------------------------- */

void pe_credit_init(struct pe_credit_ledger *l, uint32_t pool_bufs)
{
	if (l == NULL)
		return;
	l->pool = pool_bufs;
	l->reserved = 0u;
}

uint32_t pe_credit_available(const struct pe_credit_ledger *l)
{
	if (l == NULL || l->reserved >= l->pool)
		return 0u;
	return l->pool - l->reserved;
}

uint8_t pe_credit_reserve(struct pe_credit_ledger *l, uint32_t want)
{
	uint32_t grant;

	if (l == NULL)
		return 0u;

	grant = pe_credit_available(l);
	if (want < grant)
		grant = want;
	if (grant > 0xffu)
		grant = 0xffu;   /* the width of the field it is advertised in */

	l->reserved += grant;
	return (uint8_t)grant;
}

void pe_credit_release(struct pe_credit_ledger *l, uint8_t granted)
{
	if (l == NULL)
		return;
	if ((uint32_t)granted > l->reserved)
		l->reserved = 0u;
	else
		l->reserved -= (uint32_t)granted;
}

/*
 * Hand this circuit's share of the port's receive buffers back to the pool
 * (p. 2-43: "A SYSAP can also request SCS to deallocate buffers for a
 * connection"). Idempotent -- a circuit that holds nothing releases nothing --
 * because the close path and the re-formation path both run through it.
 */
static void vc_credit_release_from(struct pe_fsm *f, struct pe_vc *vc)
{
	pe_credit_release(&f->credit, vc->recv_credit_max);
	vc->recv_credit_max = 0u;
}

/*
 * Commit receive buffers to this circuit, the way p. 2-43 has a SYSAP do it
 * at connection formation: ask for what the operator configured (SYSGEN
 * CLUSTER_CREDITS), take what the port's pool can actually back, and remember
 * exactly that -- because that number, and not the request, is what goes on
 * the wire at abs 95.
 *
 * With no CLUSTER_CREDITS committed by any boot, this node asks for nothing
 * and gets nothing: 0 on the wire, counted by vc_fill_advertised(). There is
 * no default here to paper over an unconfigured store (INV-6).
 */
static void vc_credit_reserve_for(struct pe_fsm *f, struct pe_vc *vc)
{
	uint32_t want;

	vc_credit_release_from(f, vc);
	want = f->id.credits_requested_valid
		       ? (uint32_t)f->id.credits_requested : 0u;
	vc->recv_credit_max = pe_credit_reserve(&f->credit, want);
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
 *
 * `recv_seq = 0` IS THE RECEIVE ANCHOR, and it is SS4(i).A's, not a guess:
 * "the post-START SCS VC resets to send_seq = 1 on both sides", so the next
 * sequenced message this circuit may take is 1. See vc_score_seq() for why
 * this file does not instead anchor on whatever frame arrives first.
 */
static void vc_reset_sequence(struct pe_fsm *f, struct pe_vc *vc)
{
	vc_ring_clear(vc);
	vc->send_seq = 1u;
	vc->recv_seq = 0u;
	vc->peer_recv_ack = 0u;
	vc->send_credit = 0u;
	vc->send_credit_max = 0u;
	vc->recv_credit = 0u;
	vc_credit_reserve_for(f, vc);
	vc->form_tries = 0u;
}

static void vc_close(struct pe_fsm *f, struct pe_vc *vc)
{
	vc_ring_clear(vc);
	vc_disarm_vcfail(f, vc);
	vc_cancel(f, vc, PE_TIMER_RETRANSMIT);
	/* A closed circuit promises nothing, so it holds no buffers: the share
	 * goes straight back to the pool for the circuits that are still up. */
	vc_credit_release_from(f, vc);
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
	 * (SS4(g); every real node in the golden captures advertises 10).
	 * Read, never assumed -- with no grant, nothing is sent. */
	vc_credit_grant(vc, s->credits);
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
	/*
	 * FC-P6.1: the port's OWN third service gets first refusal. A
	 * block-transfer frame is not an SCS message -- its abs 56 is the
	 * 28-byte block header, not an envelope -- so handing one upward would
	 * be handing SCS a frame whose Con.ID field does not exist. pe_blk_rx_try
	 * takes it ONLY when it names a buffer this port registered (a positive
	 * executive fact, never a guess about bytes) and returns 0 otherwise, so
	 * an ordinary message reaches the code below untouched.
	 */
	if (rx->frame != NULL &&
	    pe_blk_rx_try(f, vc, rx->fi, rx->frame, rx->len))
		return;

	/*
	 * FC-P7.1: TRAP 1's receive side. A READ end message arrives with the
	 * transfer's FINAL chunk piggybacked PAST its own declared length, and
	 * this frame is BOTH -- so the tail is taken first and the message is
	 * still delivered below. Taking it first is not a preference: an SCS
	 * consumer that saw the end message before the last chunk landed would
	 * complete a short transfer as a success.
	 */
	if (rx->frame != NULL)
		(void)pe_blk_rx_trailer_try(f, vc, rx->frame, rx->len);

	/*
	 * E82: the ONE place this port learns what the peer has actually said,
	 * and therefore the only thing this node is entitled to acknowledge.
	 *
	 * It is HERE, in the delivery path, and not in the parser: a frame that
	 * arrived out of sequence was DISCARDED (h_vc_rx_gap) and a frame the
	 * codec would not classify never became a message. Raising the peer's
	 * high-water mark from either would let this node ack a message it did
	 * not take, which is the E76 assertion the guard exists to refuse.
	 */
	if (rx->frame != NULL && rx->fi != NULL)
		cm_guard_rx(&vc->guard, rx->frame, rx->len, rx->fi);

	if (!rx->conid_valid || f->upper == NULL || f->upper->message == NULL) {
		f->vc_rx_undelivered++;
		return;
	}
	f->upper->message(f->upper->ctx, vc->peer_sysid, rx->dst_conid,
			  rx->frame, rx->len);
}

/*
 * ***  THE GAP  ***  (design SS3.2.5, the E10 ruling)
 *
 * The receive window is 1. A frame ahead of recv_seq + 1 is DISCARDED --
 * there is no reorder buffer to put it in, and inventing one would be a
 * selective-repeat window no oracle grounds (Rule 8) -- recv_seq does NOT
 * advance, the gap is COUNTED, and the cumulative ack of recv_seq goes out
 * again IMMEDIATELY. That duplicate ack is the whole signal: it tells the
 * sender exactly where the hole is, and the sender's ladder resends from
 * there.
 *
 * NOTHING IS BROKEN HERE. What p. 2-31 breaks a circuit for is a guarantee
 * that CANNOT BE SATISFIED, and a gap the sender will fill on its next
 * retransmit is a guarantee being satisfied the way the wire's own retransmit
 * msgtype (SS4(h)) and SS4(k)'s ~25-retry ladder show a real port satisfying
 * it. The break belongs to the SENDER's exhausted ladder
 * (PE_VC_DOWN_RETRANSMIT_EXHAUSTED), which is a measurement, not a guess.
 *
 * The ack rides the same 0x48 credit-return every other acknowledgement does,
 * because SS4(h)(3) grounds no other acknowledgement form. It therefore also
 * returns a credit for a message this node did NOT take -- and the peer's own
 * receive path clamps that at the grant it issued (h_vc_rx_credit), so the
 * window can never inflate past what the sender was actually given. Absorbing
 * that is strictly better than emitting a bare-ack frame class no capture has
 * ever shown.
 */
static void h_vc_rx_gap(struct pe_fsm *f, struct pe_vc *vc)
{
	vc->rx_gaps++;
	vc_send_ack(f, vc);
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
 *   3. a GAP is discarded and re-acked -- go-back-N, design SS3.2.5;
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
		h_vc_rx_gap(f, vc);
		return;
	}
	if (kind != PE_VC_SEQ_DUP) {
		vc->recv_seq = rx->send_seq;
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
 *
 * BOTH halves are vc_release_acked()'s (E75), and there is deliberately no
 * second increment here. §4(h)(3)'s one-message's-worth IS the message this
 * frame's `acked_seq` releases, so returning it again on top would credit the
 * same message twice and open a window wider than the peer granted -- the
 * mirror of the double-RETURN the receive side is warned against in
 * vms_pe_fsm.h. A 0x48 repeating an ack this circuit has already taken
 * releases nothing and correctly returns nothing.
 */
static void h_vc_rx_credit(struct pe_fsm *f, struct pe_vc *vc,
			   const struct pe_vc_rx *rx)
{
	vc->credit_rx++;
	vc_release_acked(f, vc, rx->recv_ack);
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
 *
 * The BYTES ARE THE RING'S OWN (INV-6): what goes out is the frame this port
 * really sent, re-stamped, never a frame rebuilt from a template.
 *
 * Returns 0 when the frame really left, -1 when it did not -- and then the
 * attempt is NOT counted, because `retransmits` counts transmissions that
 * happened.
 */
/*
 * RE-ADDRESS A HELD FRAME ONTO THE CIRCUIT'S CURRENT PATH (E83).
 *
 * A retransmission re-sends THE RING'S OWN BYTES (INV-6): the message is the
 * one this port really sent, never rebuilt from a template. But the link-layer
 * addressing is not part of the message -- it names the PATH -- and after a
 * failover the path is a different station. So the frame's own header is parsed
 * back out of the frame, its four address fields are replaced with the CURRENT
 * channel's real learned addresses, and the header is rebuilt through the
 * codec. Every other header field comes back out of the frame itself; nothing
 * is invented, and a frame already addressed for this path is left untouched.
 *
 * Without this, a circuit that failed over with something outstanding would
 * keep retransmitting to a station that is gone and then break on TIMVCFAIL --
 * the E83 defect back by a slower road.
 */
static int vc_readdress(struct pe_fsm *f, const struct pe_vc *vc,
			uint8_t *frame, uint32_t len)
{
	struct pe_channel *ch = vc_path(f, vc);
	struct vms_scs_addr a;
	struct vms_sca_hdr h;
	uint32_t written = 0u;

	if (ch == NULL || vc_fill_addr(f, ch, &a) != 0)
		return -1;
	if (vms_sca_hdr_parse(frame, len, &h) != VMS_CODEC_OK)
		return -1;
	if (pe_mac_eq(h.eth_dst, a.dst_mac))
		return 0;                       /* already on this path */
	pe_copy(h.eth_dst, a.dst_mac, VMS_ETH_ADDR_LEN);
	pe_copy(h.eth_src, a.src_mac, VMS_ETH_ADDR_LEN);
	pe_copy(h.dst_lavc, a.dst_logical, VMS_ETH_ADDR_LEN);
	pe_copy(h.src_lavc, a.src_logical, VMS_ETH_ADDR_LEN);
	return vms_sca_hdr_build(&h, frame, len, &written) == VMS_CODEC_OK
		       ? 0 : -1;
}

static int vc_resend_one(struct pe_fsm *f, struct pe_vc *vc,
			 struct pe_vc_unacked *e, uint32_t now)
{
	struct vms_frame_info fi;

	/* First, because the header rebuild rewrites abs 30 and the retransmit
	 * marking below must win. */
	if (vc_readdress(f, vc, e->frame, e->len) != 0)
		return -1;
	if (vms_frame_classify(e->frame, e->len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_scs_seq_mark_retransmit(e->frame, e->len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_frame_classify(e->frame, e->len, &fi) != VMS_CODEC_OK)
		return -1;
	if (vms_scs_seq_stamp(e->frame, e->len, &fi, vc->recv_seq, e->seq,
			      vc_incarnation(vc)) != VMS_CODEC_OK)
		return -1;
	if (pe_tx_from(f, e->frame, e->len) != 0)
		return -1;
	e->retransmits++;
	e->due_ms = now + vc_retransmit_ms(f);
	vc->retransmits++;
	return 0;
}

/*
 * GO-BACK-N (design SS3.2.5): re-send `first` and every outstanding entry
 * behind it, IN SEQUENCE ORDER.
 *
 * The receiver's window is 1, so everything after the hole was discarded; the
 * tail has to go again, and it has to go in order or the receiver discards it
 * a second time. A transmit that fails STOPS the run: pushing the rest past a
 * frame that never left would arrive as exactly the gap this loop exists to
 * repair, and the next ladder beat starts again from the same oldest entry.
 *
 * Returns how many frames really went out.
 */
static uint32_t vc_resend_from(struct pe_fsm *f, struct pe_vc *vc,
			       struct pe_vc_unacked *first, uint32_t now)
{
	struct pe_vc_unacked *e = first;
	uint32_t sent = 0u;

	while (e != NULL) {
		uint16_t seq = e->seq;

		if (vc_resend_one(f, vc, e, now) != 0)
			break;
		sent++;
		e = vc_ring_next_after(vc, seq);
	}
	return sent;
}

/*
 * THE RETRANSMIT LADDER on an OPEN circuit, and the two ways it ends.
 *
 * Everything hangs off the OLDEST unacked entry, because with a receive window
 * of 1 that entry is the hole the peer is stuck behind: its deadline is when
 * the tail goes again, and its attempt count is the ladder.
 *
 *   not due yet          nothing to do; re-arm and wait.
 *   due, ladder spent    PE_VC_RETRANSMIT_TRIES transmissions of the same
 *                        bytes at the same sequence have gone unacknowledged.
 *                        p. 2-31's "the guarantee of message delivery cannot
 *                        be satisfied" is now measured, so the circuit breaks
 *                        with PE_VC_DOWN_RETRANSMIT_EXHAUSTED.
 *   due, TIMVCFAIL past  the silence detector underneath (design SS3.2.5).
 *                        Tested AFTER exhaustion so the more specific reason
 *                        wins when a configuration makes the two coincide.
 *   due                  go back N: re-send from the oldest, in order.
 */
static void h_vc_retransmit(struct pe_fsm *f, struct pe_vc *vc,
			    const struct pe_vc_rx *rx)
{
	uint32_t now = pe_now(f);
	struct pe_vc_unacked *oldest;

	(void)rx;
	oldest = vc_ring_oldest(vc);
	if (oldest == NULL)
		return;
	if (!pe_reached(now, oldest->due_ms)) {
		vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));
		return;
	}
	if (oldest->retransmits >= PE_VC_RETRANSMIT_TRIES) {
		vc_break(f, vc, PE_VC_DOWN_RETRANSMIT_EXHAUSTED,
			 "%PEA0, retransmit limit reached, circuit re-formed");
		return;
	}
	if (vc->vcfail_armed && pe_reached(now, vc->vcfail_due_ms)) {
		vc_break(f, vc, PE_VC_DOWN_TIMVCFAIL,
			 "%PEA0, peer stopped acknowledging, circuit re-formed");
		return;
	}
	(void)vc_resend_from(f, vc, oldest, now);
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

/*
 * THE PATH FAILED; DID THE CIRCUIT? (E83.)
 *
 * The circuit is with the remote SYSTEM, not with one of its LAN addresses. If
 * ANOTHER channel to that same system is still LIVE -- verified, inside its own
 * §4(M) deadline, and advertising the same incarnation for us -- then the
 * guarantee p. 2-31 breaks a circuit for has not failed at all: the peer is
 * reachable and, from its side, nothing has happened. Breaking here is what
 * closed an ACCEPTED VMS$VAXcluster connection the peer still held, and the
 * whole cascade -- reconnect ladder, the peer's REJECT, the CNXMGRERR --
 * followed from it (see the E83 block above vc_for_path).
 *
 * NOTHING IS ASSUMED ALIVE. The alternate must satisfy pe_channel_is_live(),
 * which is two real measurements of a real channel; if it does not, this
 * returns 0 and the circuit is torn down exactly as before.
 *
 * Returns 1 when the circuit MOVED and must not be torn down.
 */
static int vc_failover(struct pe_fsm *f, struct pe_vc *vc)
{
	struct pe_channel *alt;
	int32_t idx;

	if (!vc->peer_sysid_valid || vc->state == (uint8_t)VMS_PE_VC_CLOSED)
		return 0;
	idx = pe_alt_path(f, vc->peer_sysid, vc->channel, pe_now(f));
	if (idx < 0)
		return 0;
	alt = pe_fsm_channel_at(f, (uint32_t)idx);
	if (alt == NULL || !vc_path_continues(vc, alt))
		return 0;
	vc_rebind_path(f, vc, (uint32_t)idx);
	pe_log(f, "%PEA0, channel lost, virtual circuit continues on another "
		  "channel to the same system");
	return 1;
}

/* The channel stopped being verified (a peer re-form, spec SS4(i).B, or the
 * SS4(M) listen timeout). Design SS3.4: the circuit rides the channel, so it
 * goes with it -- unless the SAME SYSTEM still has another live path, in which
 * case the circuit moves rather than dies (E83). It does NOT re-form here,
 * because there is no verified channel to form it over; the next CHANNEL_UP
 * starts it again. */
static void h_vc_channel_down(struct pe_fsm *f, struct pe_vc *vc,
			      const struct pe_vc_rx *rx)
{
	int was_up = (vc->state == (uint8_t)VMS_PE_VC_OPEN);

	(void)rx;
	if (vc_failover(f, vc))
		return;
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
		/* A channel verified for a circuit that already exists is a
		 * PATH, not a formation (E83): vc_for_path() has already bound
		 * it if this circuit needed it, and nothing else changes. */
		[PE_EV_CHANNEL_UP]      = h_vc_idle,
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
		[PE_EV_CHANNEL_UP]      = h_vc_idle,          /* a PATH (E83) */
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
		[PE_EV_CHANNEL_UP]      = h_vc_idle,          /* a PATH (E83) */
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
	enum pe_event ev;
	uint32_t ch_index;

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

	ev = vc_event_for(&rx);
	ch_index = (uint32_t)(ch - f->ch);
	vc = vc_by_channel(f, ch_index);
	if (vc == NULL)
		vc = vc_by_channel_system(f, ch);   /* the system's other path */
	if (vc == NULL) {
		/* The peer opened the conversation. Only its START may create
		 * a circuit -- anything else is data for a circuit that does
		 * not exist, and is counted. */
		if (ev != PE_EV_RX_START) {
			f->vc_rx_no_circuit++;
			return;
		}
		vc = vc_alloc(f, ch_index);
		if (vc == NULL)
			return;
		vc->echo_incarnation = ch->peer_incarnation;
		vc->echo_valid = ch->peer_incarnation_valid;
	} else if (vc->channel != (uint8_t)ch_index) {
		/*
		 * A frame from this system on a channel other than the one its
		 * circuit is bound to. Multi-path RECEIVE is normal and the
		 * frame belongs to that one circuit's sequence space, so it is
		 * delivered either way. The circuit only MOVES for a reason:
		 * the peer's own START (§4(h)(4a) -- it is re-forming, and has
		 * just told us which path it is using, so the STACK must go
		 * back that way), or its current path no longer being live.
		 */
		f->vc_rx_alt_path++;
		if (ev == PE_EV_RX_START ||
		    !pe_channel_is_live(vc_path(f, vc), pe_now(f)))
			vc_rebind_path(f, vc, ch_index);
	}
	pe_vc_dispatch(f, vc, ev, &rx);
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
		/* By SYSTEM, not by channel: a second verified channel to a node
		 * that already has a circuit is another PATH to it, never a
		 * second circuit (E83). */
		vc = vc_for_path(f, ch_index);
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
/*
 * E70: keep the port's OWN answer on the circuit it happened to, so a refusal
 * that reaches a SYSAP as one many-to-one SS$_ status can still be told apart
 * from the four others that map to the same number. `vc` may legitimately be
 * NULL -- a send to a system this port holds no circuit object for has nowhere
 * to record anything, and that absence is what pe_vc_send_refusal_get()
 * reports instead.
 */
static int vc_note_send_refusal(struct pe_vc *vc, int code)
{
	if (vc != NULL)
		vc->last_send_refusal = (int8_t)code;
	return code;
}

/* The same, addressed by system: for the assembly paths above
 * pe_vc_send_frame, which hold only the destination. */
static int pe_note_send_refusal(struct pe_fsm *f, vms_scs_sysid_t dst, int code)
{
	return vc_note_send_refusal(pe_fsm_vc_by_sysid(f, dst), code);
}

/* --------------------------------------------------------------------------
 * E82: THE EMIT-TIME WIRE-SAFETY GUARD, wired to this circuit
 *
 * Read vms_cluster_emit_guard.h for the whole argument. What lives HERE is
 * only the wiring, and it is deliberately three small pieces: what the
 * circuit really holds (guard_read_facts), what the console is told
 * (guard_announce), and the one decision (guard_refuses).
 *
 * WHY THIS IS THE POINT IN THE STACK. pe_vc_send_frame is the LOWEST place
 * every sequenced cluster frame passes -- CNXMAN's and the DLM arm's bodies
 * come down through SCS's msg_transmit_* into it, and so do SCS's own
 * connection-control frames -- and it is the ONLY place where the fully-built
 * frame and the circuit's live transport state (the sequence about to be
 * consumed, the peer's cumulative ack, the peer's own credit grant) are both
 * in hand. Judging higher up would mean judging a frame that does not exist
 * yet; judging lower down would mean judging bytes with no circuit behind
 * them.
 *
 * IT IS NOT ON THE RETRANSMIT PATH, on purpose: vc_resend_one() re-sends
 * bytes out of the unacked ring, and those bytes were judged when they were
 * ORIGINATED. Re-judging them would spend the check twice and could refuse a
 * frame the peer is already waiting for.
 * -------------------------------------------------------------------------- */

/*
 * The circuit's LIVE transport facts. Every one is a field `vc` holds right
 * now -- the sequence this send would consume, the cumulative acknowledgement
 * the PEER really sent, and the PEER's own CLUSTER_CREDITS grant read from its
 * START body (spec SS4(g) abs 95). Nothing here is defaulted: an unlearned
 * grant stays 0 and the credit rule is simply not judged (INV-6).
 *
 * `peer_ack_valid` is `peer_recv_ack != 0`, which is exactly "this peer has
 * acknowledged at least one of our sequences" -- vc_release_acked() returns
 * before writing that field for ack 0, whose grounded meaning is "nothing
 * acknowledged" (SS4(h)(3)/(4)). A circuit whose peer has acknowledged nothing
 * therefore has no baseline, and the guard says so rather than assuming one.
 */
static void guard_read_facts(const struct pe_fsm *f, const struct pe_vc *vc,
			     struct cm_guard_facts *facts)
{
	pe_bzero(facts, (uint32_t)sizeof(*facts));
	facts->send_seq = vc->send_seq;
	facts->peer_recv_ack = vc->peer_recv_ack;
	facts->send_credit_max = vc->send_credit_max;
	facts->peer_ack_valid = (uint8_t)(vc->peer_recv_ack != 0u ? 1u : 0u);
	facts->now_ms = pe_now(f);
}

/* Append a NUL-terminated string, never past `cap - 1`. Returns the new
 * length. No printf: this is a pure FSM TU on both substrates. */
static uint32_t guard_str_add(char *buf, uint32_t cap, uint32_t at,
			      const char *s)
{
	while (s != NULL && *s != '\0' && at + 1u < cap)
		buf[at++] = *s++;
	buf[at] = '\0';
	return at;
}

/* ...and the peer's own SCSNODE, which is 8 blank-padded ASCII bytes read off
 * its START body, not a C string. Trailing blanks are dropped. A peer whose
 * name was never learned contributes NOTHING -- the line simply does not name
 * a node, rather than naming a made-up one (INV-6). */
static uint32_t guard_str_add_peer(char *buf, uint32_t cap, uint32_t at,
				   const struct pe_vc *vc)
{
	uint32_t n = VMS_SCS_START_NODENAME_LEN;
	uint32_t i;

	if (!vc->peer_name_valid)
		return at;
	while (n > 0u && vc->peer_name[n - 1u] == (uint8_t)' ')
		n--;
	if (n == 0u)
		return at;
	at = guard_str_add(buf, cap, at, " to ");
	for (i = 0u; i < n && at + 1u < cap; i++)
		buf[at++] = (char)vc->peer_name[i];
	buf[at] = '\0';
	return at;
}

/*
 * ONE CONSOLE LINE, THROTTLED. The guard exists because a per-frame reflex
 * bugchecked VAX2; a per-frame console line answering a per-frame defect would
 * be the same mistake one layer up, so cm_guard_log_due() prints the FIRST
 * sighting of each vector and rate-limits every repeat.
 */
static void guard_announce(struct pe_fsm *f, struct pe_vc *vc,
			   const struct cm_guard_finding *found)
{
	char line[112];
	uint32_t at = 0u;

	if (!cm_guard_log_due(&vc->guard, found->cls, pe_now(f)))
		return;
	/*
	 * TWO LINES, because they are two different facts and the console may
	 * not blur them: a DROP frame did not go, a WARN frame DID. Saying
	 * "refused" about a frame that went out would be the reader's version
	 * of a fabricated field.
	 */
	at = guard_str_add(line, (uint32_t)sizeof(line), at,
			   found->severity == (uint8_t)CM_GUARD_SEV_DROP
			   ? "%CNXMAN, refused to emit an unsafe cluster frame ("
			   : "%CNXMAN, emitted a cluster frame outside the measured envelope (");
	at = guard_str_add(line, (uint32_t)sizeof(line), at,
			   cm_guard_class_name(found->cls));
	at = guard_str_add(line, (uint32_t)sizeof(line), at, ")");
	(void)guard_str_add_peer(line, (uint32_t)sizeof(line), at, vc);
	pe_log(f, line);
}

/*
 * THE DECISION. Non-zero means DO NOT PUT THIS FRAME ON THE WIRE.
 *
 * A WARN finding is counted and announced and the frame still goes: the guard
 * refuses only what the auditor's corpus proves no real VMS node has ever
 * emitted (vms_cluster_emit_guard.h SS2's two deliberate deviations).
 */
static int guard_refuses(struct pe_fsm *f, struct pe_vc *vc,
			 const uint8_t *frame, uint32_t len,
			 const struct vms_frame_info *fi,
			 struct cm_guard_frame *view)
{
	struct cm_guard_facts facts;
	struct cm_guard_finding found;
	int verdict;

	guard_read_facts(f, vc, &facts);
	verdict = cm_guard_check_tx(&vc->guard, &facts, frame, len, fi, view,
				    &found);
	if (!view->judged) {
		f->guard_skipped++;
		return 0;
	}
	f->guard_judged++;
	if (found.cls == (uint8_t)CM_GUARD_C_NONE)
		return 0;

	guard_announce(f, vc, &found);
	if (verdict == CM_GUARD_REFUSE) {
		f->guard_refused++;
		return 1;
	}
	f->guard_warned++;
	return 0;
}

int pe_vc_send_frame(struct pe_fsm *f, vms_scs_sysid_t dst,
		     const uint8_t *frame, uint32_t len)
{
	struct cm_guard_frame gview;
	struct vms_frame_info fi;
	struct pe_vc_unacked *e;
	struct pe_vc *vc;
	uint16_t seq;

	if (f == NULL || frame == NULL)
		return PE_VC_SEND_BADFRAME;
	vc = pe_fsm_vc_by_sysid(f, dst);
	if (vc == NULL || vc->state != (uint8_t)VMS_PE_VC_OPEN)
		return vc_note_send_refusal(vc, PE_VC_SEND_NOCIRCUIT);
	if (len > PE_VC_FRAME_MAX)
		return vc_note_send_refusal(vc, PE_VC_SEND_TOOBIG);
	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return vc_note_send_refusal(vc, PE_VC_SEND_BADFRAME);
	/* p. 2-43: no credit, no message. The grant is the PEER's, read from
	 * its formation body; this node never invents a window for itself. */
	if (vc->send_credit == 0u) {
		vc->send_refused_credit++;
		return vc_note_send_refusal(vc, PE_VC_SEND_NOCREDIT);
	}
	/*
	 * E82: the LAST gate before this frame becomes a sequence, a ring
	 * entry and a transmission. It is here rather than after the stamp so
	 * a refused frame consumes NOTHING -- no number, no ring slot -- which
	 * is the same no-hole guarantee the ordering below already makes, and
	 * the exact defect (a counter advanced by a send that never left) that
	 * produced the E76 bugcheck.
	 */
	if (guard_refuses(f, vc, frame, len, &fi, &gview))
		return vc_note_send_refusal(vc, PE_VC_SEND_UNSAFE);

	seq = vc->send_seq;
	e = vc_ring_alloc(vc);
	if (e == NULL) {
		vc->send_refused_ring++;
		return vc_note_send_refusal(vc, PE_VC_SEND_RINGFULL);
	}
	pe_copy(e->frame, frame, len);
	e->len = len;
	e->seq = seq;
	if (vms_scs_seq_stamp(e->frame, e->len, &fi, vc->recv_seq, seq,
			      vc_incarnation(vc)) != VMS_CODEC_OK) {
		vc_ring_free(vc, e);          /* nothing was sent, nothing */
		if (!vc->echo_valid)
			f->vc_no_incarnation++;
		/* was consumed */
		return vc_note_send_refusal(vc, PE_VC_SEND_BADFRAME);
	}

	vc->send_seq = seq_next(seq);
	vc->send_credit--;
	vc->msgs_tx++;
	e->due_ms = pe_now(f) + vc_retransmit_ms(f);
	if (!vc->vcfail_armed)
		vc_arm_vcfail(f, vc);
	vc_arm(f, vc, PE_TIMER_RETRANSMIT, vc_retransmit_ms(f));

	if (pe_tx_from(f, e->frame, e->len) != 0)   /* held for the ladder */
		return vc_note_send_refusal(vc, PE_VC_SEND_TXFAIL);
	/*
	 * E82: and ONLY now -- once the substrate has really taken the bytes --
	 * does the guard's observation ledger advance. A frame the interface
	 * refused leaves the ledger exactly where it was, so this node can
	 * never open its next dialogue at a number it never actually sent.
	 */
	cm_guard_sent(&vc->guard, &gview, pe_now(f));
	return PE_VC_SEND_OK;
}

int pe_vc_send_refusal_get(struct pe_fsm *f, vms_scs_sysid_t dst,
			   struct pe_vc_send_refusal *out)
{
	struct pe_vc *vc;

	if (f == NULL || out == NULL)
		return -1;
	pe_bzero(out, (uint32_t)sizeof(*out));
	vc = pe_fsm_vc_by_sysid(f, dst);
	if (vc == NULL)
		return 0;   /* vc_present stays 0: THAT is the answer */
	out->vc_present = 1u;
	out->code = (int32_t)vc->last_send_refusal;
	out->vc_state = vc->state;
	out->send_credit = vc->send_credit;
	out->send_credit_max = vc->send_credit_max;
	out->send_refused_credit = vc->send_refused_credit;
	out->send_refused_ring = vc->send_refused_ring;
	out->unacked = vc->unacked;
	/* E82: the guard's own answer, live off this circuit's ledger. */
	out->guard_class = vc->guard.last_class;
	out->guard_refused = vc->guard.refused;
	out->guard_warned = vc->guard.warned;
	return 0;
}

/* --------------------------------------------------------------------------
 * FC-P1.3: the body-level send services (vms_pe_fsm.h SS8c) -- the bridge
 * between SCS's "body" and pe_vc_send_frame's "frame".
 * -------------------------------------------------------------------------- */

/*
 * Build this circuit's abs 0-35 (addressing + the sequence envelope) into
 * `frame`. `recv_ack`/`send_seq` are the caller's choice -- pe_vc_send_msg
 * passes placeholders pe_vc_send_frame overwrites at transmit time;
 * pe_vc_send_dg, which never reaches pe_vc_send_frame, passes the REAL
 * values it wants on the wire. Abs 36-55 is left at whatever `frame`
 * already held -- both callers zero it first (vms_pe_fsm.h SS8c: an
 * explicit zero, never a template).
 */
static int pe_send_build_envelope(struct pe_fsm *f, vms_scs_sysid_t dst,
				  uint8_t msgtype, uint16_t recv_ack,
				  uint16_t send_seq, uint8_t *frame,
				  uint32_t cap)
{
	struct vms_scs_seq_envelope env;

	pe_bzero(&env, (uint32_t)sizeof(env));
	if (pe_vc_addr(f, dst, &env.addr) != 0)
		return -1;
	env.msgtype = msgtype;
	env.recv_ack = recv_ack;
	env.send_seq = send_seq;
	if (vms_scs_seq_envelope_build(&env, frame, cap, (uint32_t *)0) !=
	    VMS_CODEC_OK)
		return -1;
	return 0;
}

/*
 * The one body-level assembly both sequenced-message entries share: build this
 * circuit's abs 0-55, splice the caller's abs-56-onward content on, fix the SCA
 * length up to the WHOLE frame, and hand it to pe_vc_send_frame (which
 * re-stamps 32/34/44 with the circuit's real sequence and assigns/queues/
 * transmits it, SS3b). `len` is validated by the CALLER, because the two
 * entries have deliberately different length contracts.
 */
static int pe_send_msg_assemble(struct pe_fsm *f, vms_scs_sysid_t dst,
				const uint8_t *body, uint32_t len)
{
	uint8_t frame[PE_VC_FRAME_MAX];
	uint32_t total = PE_SEND_BODY_OFF + len;

	if (total > (uint32_t)sizeof(frame))
		return pe_note_send_refusal(f, dst, PE_VC_SEND_TOOBIG);

	pe_bzero(frame, (uint32_t)sizeof(frame));
	if (pe_send_build_envelope(f, dst, VMS_SCS_MT_MSG, 0u, 0u, frame,
				   (uint32_t)sizeof(frame)) != 0)
		return pe_note_send_refusal(f, dst, PE_VC_SEND_NOCIRCUIT);

	pe_copy(frame + PE_SEND_BODY_OFF, body, len);

	if (vms_scs_seq_envelope_fixup_len(frame, (uint32_t)sizeof(frame),
					   total) != VMS_CODEC_OK)
		return pe_note_send_refusal(f, dst, PE_VC_SEND_BADFRAME);

	return pe_vc_send_frame(f, dst, frame, total);
}

int pe_vc_send_msg(struct pe_fsm *f, vms_scs_sysid_t dst,
		   vms_conid_t dst_conid, const uint8_t *body, uint32_t len)
{
	(void)dst_conid;   /* not written to the wire -- see the .h doc comment */
	if (f == NULL || body == NULL)
		return PE_VC_SEND_BADFRAME;
	if (len != PE_SEND_BODY_LEN)
		return pe_note_send_refusal(f, dst, PE_VC_SEND_BADFRAME);
	return pe_send_msg_assemble(f, dst, body, len);
}

int pe_vc_send_msg_var(struct pe_fsm *f, vms_scs_sysid_t dst,
		       vms_conid_t dst_conid, const uint8_t *body, uint32_t len)
{
	(void)dst_conid;
	if (f == NULL || body == NULL)
		return PE_VC_SEND_BADFRAME;
	if (len < PE_SEND_BODY_MIN)
		return pe_note_send_refusal(f, dst, PE_VC_SEND_BADFRAME);
	if (len > PE_SEND_BODY_MAX)
		return pe_note_send_refusal(f, dst, PE_VC_SEND_TOOBIG);
	return pe_send_msg_assemble(f, dst, body, len);
}

int pe_vc_send_dg(struct pe_fsm *f, vms_scs_sysid_t dst,
		  const uint8_t *body, uint32_t len)
{
	uint8_t frame[PE_VC_FRAME_MAX];
	struct pe_vc *vc;
	uint32_t total;

	if (f == NULL || body == NULL || len != PE_SEND_BODY_LEN)
		return PE_VC_SEND_BADFRAME;

	vc = pe_fsm_vc_by_sysid(f, dst);
	if (vc == NULL || vc->state != (uint8_t)VMS_PE_VC_OPEN)
		return PE_VC_SEND_NOCIRCUIT;

	total = PE_SEND_BODY_OFF + len;
	if (total > (uint32_t)sizeof(frame))
		return PE_VC_SEND_TOOBIG;

	pe_bzero(frame, (uint32_t)sizeof(frame));
	/* send_seq = 0: "no sequence claimed" (see the .h honesty note).
	 * recv_ack = the circuit's real recv_seq: a true transport fact,
	 * free to report even outside the ring. */
	if (pe_send_build_envelope(f, dst, VMS_SCS_MT_MSG, vc->recv_seq, 0u,
				   frame, (uint32_t)sizeof(frame)) != 0)
		return PE_VC_SEND_NOCIRCUIT;

	pe_copy(frame + PE_SEND_BODY_OFF, body, len);

	if (vms_scs_seq_envelope_fixup_len(frame, (uint32_t)sizeof(frame),
					   total) != VMS_CODEC_OK)
		return PE_VC_SEND_BADFRAME;

	if (pe_tx_from(f, frame, total) != 0)
		return PE_VC_SEND_TXFAIL;
	vc->dg_tx++;
	return PE_VC_SEND_OK;
}

/* --------------------------------------------------------------------------
 * FC-P6.1: THE BLOCK-TRANSFER SERVICE (vms_pe_fsm.h SS8d)
 *
 * The third port service. Read SS8d first -- in particular why a block frame
 * consumes a real sequence, why it does NOT ride the unacked ring, and why the
 * +4/+6 header words are either a value this circuit OBSERVED or an explicit
 * counted zero.
 * -------------------------------------------------------------------------- */

/* ---- named buffers (SS3c) ---- */

static struct pe_blk_buf *blk_buf_slot(struct pe_fsm *f, uint32_t name)
{
	uint32_t i;

	if (name == 0u)
		return NULL;
	for (i = 0; i < PE_BLK_MAX_BUFFERS; i++) {
		if (f->blk_buf[i].name == name)
			return &f->blk_buf[i];
	}
	return NULL;
}

const struct pe_blk_buf *pe_blk_buf_lookup(const struct pe_fsm *f,
					   uint32_t name)
{
	uint32_t i;

	if (f == NULL || name == 0u)
		return NULL;
	for (i = 0; i < PE_BLK_MAX_BUFFERS; i++) {
		if (f->blk_buf[i].name == name)
			return &f->blk_buf[i];
	}
	return NULL;
}

/*
 * Mint the next name for slot `slot`: a monotone generation in the high bits
 * and the slot in the low 8, never 0. OVMX's own rule (SS3c) -- the wire
 * grounds only that the token is 32 bits both ends agree on. The generation is
 * what stops a released name from being handed straight back out, so a stale
 * transfer naming a dead buffer MISSES instead of landing in whatever took the
 * slot.
 */
static uint32_t blk_name_mint(struct pe_fsm *f, uint32_t slot)
{
	uint32_t name;

	do {
		f->blk_name_gen++;
		name = (f->blk_name_gen << 8) | ((slot + 1u) & 0xffu);
	} while (name == 0u || pe_blk_buf_lookup(f, name) != NULL);
	return name;
}

int pe_blk_buf_register(struct pe_fsm *f, uint8_t *base, uint32_t len,
			uint8_t access, uint32_t *name_out)
{
	uint32_t i;

	if (f == NULL || base == NULL || len == 0u || name_out == NULL)
		return PE_BLK_INVAL;
	if ((access & (PE_BLK_ACC_SRC | PE_BLK_ACC_DST)) == 0u)
		return PE_BLK_INVAL;

	for (i = 0; i < PE_BLK_MAX_BUFFERS; i++) {
		if (f->blk_buf[i].name != 0u)
			continue;
		f->blk_buf[i].name = blk_name_mint(f, i);
		f->blk_buf[i].base = base;
		f->blk_buf[i].len = len;
		f->blk_buf[i].access = access;
		*name_out = f->blk_buf[i].name;
		return PE_BLK_OK;
	}
	f->blk_no_slot++;
	return PE_BLK_NOSPACE;
}

int pe_blk_buf_release(struct pe_fsm *f, uint32_t name)
{
	struct pe_blk_buf *b;

	if (f == NULL)
		return PE_BLK_INVAL;
	b = blk_buf_slot(f, name);
	if (b == NULL)
		return PE_BLK_NOBUF;
	/* The memory is the caller's; only the NAME is dropped. */
	b->name = 0u;
	b->base = NULL;
	b->len = 0u;
	b->access = 0u;
	return PE_BLK_OK;
}

/* [off, off+n) inside a buffer of `len` bytes, with no arithmetic that can
 * wrap past it. The one function every transfer's bounds run through. */
static int blk_span_ok(uint32_t buf_len, uint32_t off, uint32_t n)
{
	if (off > buf_len)
		return 0;
	return (buf_len - off) >= n;
}

/* ---- the header, filled from real state only ---- */

/*
 * Fill one frame's 28-byte header. Every field traces to something the
 * executive holds: `x` carries what the SYSAP read out of the peer's own
 * message, `buf` is our registered buffer, `done` is how much of THIS transfer
 * has already gone out, and the +4/+6 words come from `vc`'s observed pair or
 * are an explicit zero the caller counts.
 */
static void blk_fill_hdr(const struct pe_fsm *f, const struct pe_vc *vc,
			 const struct pe_blk_xfer *x,
			 const struct pe_blk_buf *buf, uint32_t done,
			 struct vms_blk_hdr *h)
{
	(void)f;
	pe_bzero(h, (uint32_t)sizeof(*h));
	h->dest_conid = x->dest_conid;
	/* +4/+6: the value this circuit OBSERVED, or an explicit zero. Never a
	 * captured constant and never a rule this code invented -- see
	 * vms_cluster_codec_blk.h "THE TWO UNGROUNDED WORDS". */
	if (vc->obs_valid) {
		h->obs_w4 = vc->obs_w4;
		h->obs_w6 = vc->obs_w6;
	}
	h->bytes_remaining = x->length - done;
	h->src_name = buf->name;
	h->src_offset = x->local_offset + done;
	h->dst_name = x->remote_name;
	h->dst_offset = x->remote_offset + done;
}

/* The +4/+6 accounting, in one place: a frame built with no observed pair is
 * counted, so "how often did this port emit an honest zero there" is a number
 * and not a belief. */
static void blk_note_obs(struct pe_fsm *f, const struct pe_vc *vc)
{
	if (!vc->obs_valid)
		f->blk_obs_absent++;
}

/* ---- one frame out ---- */

/*
 * Give a block-transfer frame the circuit's real sequence position and
 * transmit it. Mirrors pe_vc_send_frame's ORDER, and for the same reason: the
 * sequence is consumed only by a frame that actually went out, so a refused or
 * failed transmit leaves no hole for the peer to break the circuit over. It
 * does NOT take a ring entry -- SS8d "NO RING".
 */
static int blk_stamp_and_send(struct pe_fsm *f, struct pe_vc *vc,
			      uint8_t *frame, uint32_t total)
{
	struct vms_frame_info fi;
	uint16_t seq = vc->send_seq;

	if (vms_frame_classify(frame, total, &fi) != VMS_CODEC_OK)
		return PE_BLK_BADFRAME;
	if (vms_scs_seq_stamp(frame, total, &fi, vc->recv_seq, seq,
			      vc_incarnation(vc)) != VMS_CODEC_OK) {
		if (!vc->echo_valid)
			f->vc_no_incarnation++;
		return PE_BLK_BADFRAME;
	}
	if (pe_tx_from(f, frame, total) != 0)
		return PE_BLK_TXFAIL;

	vc->send_seq = seq_next(seq);
	vc->blk_tx++;
	f->blk_tx_unringed++;
	return PE_BLK_OK;
}

/*
 * Build one standalone block-transfer frame into the scratch buffer: the
 * port's own abs 0-55 (SS8c's envelope builder), then the codec's 28-byte
 * header, then `n` data bytes, then the SCA length fixed up to the real total.
 * Transmits it. Nothing is kept in scratch across the return.
 */
static int blk_send_one(struct pe_fsm *f, struct pe_vc *vc,
			vms_scs_sysid_t dst, const struct vms_blk_hdr *h,
			const uint8_t *data, uint32_t n)
{
	uint8_t *frame = f->scratch;
	const uint32_t cap = (uint32_t)sizeof(f->scratch);
	uint32_t total;

	if (n > VMS_BLK_DATA_MAX)
		return PE_BLK_TOOBIG;
	total = VMS_BLK_DATA_OFF + n;
	if (total > cap)
		return PE_BLK_TOOBIG;

	pe_bzero(frame, cap);
	if (pe_send_build_envelope(f, dst, VMS_SCS_MT_MSG, 0u, 0u, frame,
				   cap) != 0)
		return PE_BLK_NOCIRCUIT;
	if (vms_blk_hdr_build(h, frame, cap) != VMS_CODEC_OK)
		return PE_BLK_BADFRAME;
	if (n != 0u)
		pe_copy(frame + VMS_BLK_DATA_OFF, data, n);
	if (vms_scs_seq_envelope_fixup_len(frame, cap, total) != VMS_CODEC_OK)
		return PE_BLK_BADFRAME;

	return blk_stamp_and_send(f, vc, frame, total);
}

/* ---- the callers' shared preflight ---- */

/*
 * Everything a transfer must have before a byte moves, resolved ONCE: the open
 * circuit, our own named buffer, and the peer-supplied identifiers we refuse to
 * invent. The chunk size is clamped here too, so no caller downstream has to.
 */
struct blk_setup {
	struct pe_vc           *vc;
	const struct pe_blk_buf *buf;
	uint32_t                chunk;
};

static int blk_setup_send(struct pe_fsm *f, const struct pe_blk_xfer *x,
			  uint32_t tail_reserve, struct blk_setup *s)
{
	if (f == NULL || x == NULL || x->length == 0u)
		return PE_BLK_INVAL;
	if (tail_reserve > x->length)
		return PE_BLK_INVAL;

	/* INV-6: the destination connection ID and the peer's buffer name are
	 * the PEER's values. If the SYSAP could not read them out of the
	 * message that asked for this transfer, there is nothing honest to put
	 * in those fields and the transfer is refused. */
	if (x->dest_conid == 0u || x->remote_name == 0u)
		return PE_BLK_NONAME;

	s->vc = pe_fsm_vc_by_sysid(f, x->peer);
	if (s->vc == NULL || s->vc->state != (uint8_t)VMS_PE_VC_OPEN)
		return PE_BLK_NOCIRCUIT;

	s->buf = pe_blk_buf_lookup(f, x->local_name);
	if (s->buf == NULL)
		return PE_BLK_NOBUF;
	if ((s->buf->access & PE_BLK_ACC_SRC) == 0u)
		return PE_BLK_PERM;
	if (!blk_span_ok(s->buf->len, x->local_offset, x->length))
		return PE_BLK_RANGE;

	s->chunk = (x->chunk != 0u && x->chunk < VMS_BLK_DATA_MAX)
		   ? x->chunk : VMS_BLK_DATA_MAX;
	return PE_BLK_OK;
}

int pe_blk_send(struct pe_fsm *f, const struct pe_blk_xfer *x,
		uint32_t tail_reserve, uint32_t *frames_out)
{
	struct blk_setup s;
	struct vms_blk_hdr h;
	uint32_t done = 0u;
	uint32_t stream;
	uint32_t frames = 0u;
	int rc = blk_setup_send(f, x, tail_reserve, &s);

	if (frames_out != NULL)
		*frames_out = 0u;
	if (rc != PE_BLK_OK)
		return rc;

	stream = x->length - tail_reserve;
	while (done < stream) {
		uint32_t n = stream - done;

		if (n > s.chunk)
			n = s.chunk;
		blk_fill_hdr(f, s.vc, x, s.buf, done, &h);
		rc = blk_send_one(f, s.vc, x->peer, &h,
				  s.buf->base + x->local_offset + done, n);
		if (rc != PE_BLK_OK)
			break;
		blk_note_obs(f, s.vc);   /* counted only for a frame that WENT */
		s.vc->blk_bytes_tx += n;
		done += n;
		frames++;
	}
	if (frames_out != NULL)
		*frames_out = frames;
	return rc;
}

int pe_blk_send_read_end(struct pe_fsm *f, const struct pe_blk_xfer *x,
			 uint32_t tail_len, const uint8_t *body,
			 uint32_t body_len)
{
	struct blk_setup s;
	struct vms_blk_hdr h;
	uint8_t *frame;
	uint32_t cap;
	uint32_t end_len;
	uint32_t total = 0u;
	int rc;

	if (body == NULL || body_len == 0u)
		return PE_BLK_INVAL;
	rc = blk_setup_send(f, x, tail_len, &s);
	if (rc != PE_BLK_OK)
		return rc;
	if (tail_len > VMS_BLK_DATA_MAX)
		return PE_BLK_TOOBIG;

	frame = f->scratch;
	cap = (uint32_t)sizeof(f->scratch);
	/* The end message's own frame: the port's abs 0-55 plus the caller's
	 * abs-56-onward body. Its length is what the trailer is appended PAST,
	 * and what a receiver must NOT use to bound the frame (TRAP 1). */
	end_len = PE_SEND_BODY_OFF + body_len;
	if (end_len > cap ||
	    (cap - end_len) < (VMS_BLK_HDR_LEN + tail_len))
		return PE_BLK_TOOBIG;

	pe_bzero(frame, cap);
	if (pe_send_build_envelope(f, x->peer, VMS_SCS_MT_MSG, 0u, 0u, frame,
				   cap) != 0)
		return PE_BLK_NOCIRCUIT;
	pe_copy(frame + PE_SEND_BODY_OFF, body, body_len);

	/* The trailer describes the transfer's FINAL chunk: everything before
	 * it has already been streamed by pe_blk_send(). */
	blk_fill_hdr(f, s.vc, x, s.buf, x->length - tail_len, &h);
	if (vms_blk_trailer_build(&h, s.buf->base + x->local_offset +
					(x->length - tail_len),
				  tail_len, frame, cap, end_len,
				  &total) != VMS_CODEC_OK)
		return PE_BLK_BADFRAME;

	/* The SCA length covers the WHOLE frame including the trailer -- the
	 * recorded READ-END SCA contents 118/194/448/630/1142 are each the
	 * 90-content end message plus 28 plus the tail. The inner message's own
	 * declared length is untouched and still names only itself. */
	if (vms_scs_seq_envelope_fixup_len(frame, cap, total) != VMS_CODEC_OK)
		return PE_BLK_BADFRAME;

	rc = blk_stamp_and_send(f, s.vc, frame, total);
	if (rc == PE_BLK_OK) {
		blk_note_obs(f, s.vc);   /* counted only for a frame that WENT */
		s.vc->blk_bytes_tx += tail_len;
	}
	return rc;
}

/* --------------------------------------------------------------------------
 * FC-P6.5: REQUEST DATA -- the SEND side (vms_pe_fsm.h SS8d, design SS3.2.6/E41)
 * -------------------------------------------------------------------------- */

/*
 * Everything a REQUEST DATA must have before its 28 bytes are composed. The
 * mirror of blk_setup_send, and it differs in exactly one place that matters:
 * our buffer is the transfer's DESTINATION, so it is checked for
 * PE_BLK_ACC_DST. Asking a peer to fill a buffer we would refuse to write into
 * is a request we must not make.
 */
static int blk_setup_request(struct pe_fsm *f, const struct pe_blk_xfer *x,
			     struct blk_setup *s)
{
	if (f == NULL || x == NULL || x->length == 0u)
		return PE_BLK_INVAL;
	/* INV-6, the same refusal pe_blk_send makes: the connection ID and the
	 * peer's buffer name are the PEER's values, read off its own message. */
	if (x->dest_conid == 0u || x->remote_name == 0u)
		return PE_BLK_NONAME;

	s->vc = pe_fsm_vc_by_sysid(f, x->peer);
	if (s->vc == NULL || s->vc->state != (uint8_t)VMS_PE_VC_OPEN)
		return PE_BLK_NOCIRCUIT;

	s->buf = pe_blk_buf_lookup(f, x->local_name);
	if (s->buf == NULL)
		return PE_BLK_NOBUF;
	if ((s->buf->access & PE_BLK_ACC_DST) == 0u)
		return PE_BLK_PERM;
	if (!blk_span_ok(s->buf->len, x->local_offset, x->length))
		return PE_BLK_RANGE;

	s->chunk = 0u;   /* a request carries no data: nothing to chunk */
	return PE_BLK_OK;
}

/*
 * The REQUEST's 28 bytes. blk_fill_hdr cannot be reused because the ROLES ARE
 * SWAPPED -- our buffer is the destination and the peer's is the source -- and
 * a shared filler taking a "which way round" flag would put the one field that
 * decides where the bytes land behind a boolean. Everything here is still read
 * from real state: `x` carries what the SYSAP read off the peer's message, the
 * destination name is the buffer WE registered, and +4/+6 are this circuit's
 * observed pair or an explicit zero (SS3b).
 */
static void blk_fill_request_hdr(const struct pe_vc *vc,
				 const struct pe_blk_xfer *x,
				 const struct pe_blk_buf *buf,
				 struct vms_blk_hdr *h)
{
	pe_bzero(h, (uint32_t)sizeof(*h));
	h->dest_conid = x->dest_conid;
	if (vc->obs_valid) {
		h->obs_w4 = vc->obs_w4;
		h->obs_w6 = vc->obs_w6;
	}
	h->bytes_remaining = x->length;
	h->src_name = x->remote_name;        /* the PEER's buffer, from its msg */
	h->src_offset = x->remote_offset;
	h->dst_name = buf->name;             /* OURS, and the port minted it    */
	h->dst_offset = x->local_offset;
}

int pe_blk_send_request(struct pe_fsm *f, const struct pe_blk_xfer *x)
{
	struct blk_setup s;
	struct vms_blk_hdr h;
	int rc = blk_setup_request(f, x, &s);

	if (rc != PE_BLK_OK)
		return rc;

	blk_fill_request_hdr(s.vc, x, s.buf, &h);
	rc = blk_send_one(f, s.vc, x->peer, &h, NULL, 0u);
	if (rc == PE_BLK_OK)
		blk_note_obs(f, s.vc);   /* counted only for a frame that WENT */
	return rc;
}

/* ---- receive ---- */

/* The observed +4/+6 pair, learned from a frame that really arrived on this
 * circuit. This is the ONLY writer of pe_vc.obs_*. */
static void blk_learn_obs(struct pe_vc *vc, const struct vms_blk_hdr *h)
{
	vc->obs_w4 = h->obs_w4;
	vc->obs_w6 = h->obs_w6;
	vc->obs_valid = 1u;
}

/*
 * ONE parsed block-transfer view, taken into the buffer it names. Shared by
 * BOTH receive arms -- the standalone frame and TRAP 1's trailer -- so the
 * discriminator, the bounds check, the observed-word learning and the upward
 * report are written once and cannot diverge between the two shapes.
 *
 * Returns 1 when the view named a buffer of ours (whether or not its bytes
 * were acceptable: a range failure is still OUR frame and is counted, never
 * passed on), 0 when it named nothing of ours.
 */
static int blk_rx_take(struct pe_fsm *f, struct pe_vc *vc,
		       const struct vms_blk_view *view)
{
	struct pe_blk_buf *buf = blk_buf_slot(f, view->hdr.dst_name);

	/* THE DISCRIMINATOR: does this frame name a buffer WE registered? */
	if (buf == NULL) {
		f->blk_rx_unnamed++;
		return 0;
	}
	if ((buf->access & PE_BLK_ACC_DST) == 0u ||
	    !blk_span_ok(buf->len, view->hdr.dst_offset, view->data_len)) {
		f->blk_rx_range++;
		return 1;
	}

	blk_learn_obs(vc, &view->hdr);
	if (view->data_len != 0u) {
		pe_copy(buf->base + view->hdr.dst_offset, view->data,
			view->data_len);
		vc->blk_bytes_rx += view->data_len;
	}
	vc->blk_rx++;

	/* Reported AFTER the bytes are in the buffer, describing what actually
	 * landed (vms_pe.h SS4, block_data). No listener is legitimate. */
	if (f->upper != NULL && f->upper->block_data != NULL)
		f->upper->block_data(f->upper->ctx, vc->peer_sysid, buf->name,
				     view->hdr.dst_offset, view->data_len,
				     view->hdr.bytes_remaining);
	return 1;
}

/* --------------------------------------------------------------------------
 * FC-P6.5: REQUEST DATA -- the RESPONDER (vms_pe_fsm.h SS8d, design SS3.2.6/E41)
 *
 * "a header-only 28-byte block frame naming the host's buffer as *source* and
 * the server's as *destination*, which the host's port answers -- with no SYSAP
 * involvement -- by transmitting the named buffer's contents under the same
 * header."
 * -------------------------------------------------------------------------- */

/*
 * One answer frame's header: the REQUEST's OWN 28 bytes with the two offsets
 * and the down-counter advanced by what has already gone out. So the FIRST
 * answer frame is BYTE-IDENTICAL to the request -- the vms291 WRITE pair,
 * "only the presence of data distinguishes them" -- and every later one differs
 * in exactly the three fields the transfer really moved.
 *
 * +0, +4, +6 and BOTH BUFFER NAMES are echoed verbatim from bytes this port
 * received. Nothing here is composed, including the two ungrounded words: they
 * are not this circuit's remembered pair but the requester's own, off the frame
 * being answered.
 */
static void blk_answer_hdr(const struct vms_blk_hdr *req, uint32_t done,
			   struct vms_blk_hdr *h)
{
	*h = *req;
	h->bytes_remaining = req->bytes_remaining - done;
	h->src_offset = req->src_offset + done;
	h->dst_offset = req->dst_offset + done;
}

/*
 * Transmit the requested span out of `buf`, in READ's chunking (largest chunk
 * first, VMS_BLK_DATA_MAX). The bytes are the named buffer's REAL contents --
 * this is the whole point of the service, and there is no other source for
 * them. Returns 0, or -1 with the frames already sent left sent.
 */
static int blk_answer_send(struct pe_fsm *f, struct pe_vc *vc,
			   const struct vms_blk_hdr *req,
			   const struct pe_blk_buf *buf)
{
	struct vms_blk_hdr h;
	uint32_t total = req->bytes_remaining;
	uint32_t done = 0u;

	while (done < total) {
		uint32_t n = total - done;

		if (n > VMS_BLK_DATA_MAX)
			n = VMS_BLK_DATA_MAX;
		blk_answer_hdr(req, done, &h);
		if (blk_send_one(f, vc, vc->peer_sysid, &h,
				 buf->base + req->src_offset + done,
				 n) != PE_BLK_OK)
			return -1;
		/* blk_note_obs is NOT called: +4/+6 were ECHOED off the
		 * request, so there is no honest absence to count here. */
		vc->blk_bytes_tx += n;
		done += n;
	}
	return 0;
}

/*
 * A header-only block frame arrived. Answer it if its SOURCE names a buffer of
 * ours; otherwise count it and leave the frame alone.
 *
 * Returns 1 when the frame was ours (answered or refused -- a refusal is still
 * our frame and must not be re-read as an SCS message), 0 when it named nothing
 * of ours.
 */
static int blk_req_answer(struct pe_fsm *f, struct pe_vc *vc,
			  const struct vms_blk_view *view)
{
	const struct vms_blk_hdr *req = &view->hdr;
	struct pe_blk_buf *buf = blk_buf_slot(f, req->src_name);

	/* THE DISCRIMINATOR -- the same positive executive fact the delivery
	 * half uses, in the other role. A request for a buffer this port never
	 * minted is DROPPED: it is never answered out of some other buffer,
	 * which would put one SYSAP's memory on the wire under another's name. */
	if (buf == NULL) {
		f->blk_req_unknown_buffer++;
		return 0;
	}
	f->blk_req_rx++;
	blk_learn_obs(vc, req);   /* it really arrived on this circuit */

	if ((buf->access & PE_BLK_ACC_SRC) == 0u ||
	    req->bytes_remaining == 0u ||
	    !blk_span_ok(buf->len, req->src_offset, req->bytes_remaining)) {
		/* Not a legal source, or a span outside it. REFUSED whole --
		 * never clamped to what would fit, because a short answer under
		 * a full byte count is a transfer the requester would complete
		 * as if it had all the bytes. */
		f->blk_req_refused++;
		return 1;
	}
	if (blk_answer_send(f, vc, req, buf) != 0) {
		f->blk_req_refused++;
		return 1;
	}
	f->blk_req_answered++;
	return 1;
}

int pe_blk_rx_try(struct pe_fsm *f, struct pe_vc *vc,
		  const struct vms_frame_info *fi, const uint8_t *frame,
		  uint32_t len)
{
	struct vms_blk_view view;

	if (f == NULL || vc == NULL || frame == NULL)
		return 0;
	/* Cheap structural precondition -- NOT a class test (see the codec). */
	if (!vms_blk_frame_structural_ok(frame, len))
		return 0;
	if (vms_blk_frame_parse(frame, len, fi, &view) != VMS_CODEC_OK)
		return 0;

	/*
	 * THE TWO HALVES OF THE SERVICE, split on the one thing the wire
	 * distinguishes them by (design SS3.2.6/E41): a frame with data behind
	 * its header is a DELIVERY into the buffer it names as destination; a
	 * frame with none is a REQUEST DATA for the buffer it names as source,
	 * and this port answers it itself.
	 */
	if (view.data_len == 0u)
		return blk_req_answer(f, vc, &view);

	/* From here a frame that names one of our buffers IS a block transfer
	 * for this node and is consumed either way -- never passed on to be
	 * misread as an SCS message. */
	return blk_rx_take(f, vc, &view);
}

int pe_blk_rx_trailer_try(struct pe_fsm *f, struct pe_vc *vc,
			  const uint8_t *frame, uint32_t len)
{
	struct vms_blk_view view;
	uint32_t inner = 0u;

	if (f == NULL || vc == NULL || frame == NULL)
		return 0;

	/* The inner message's OWN declared bound, from the codec that owns the
	 * arithmetic. Equal to `len` means the frame is exactly its inner
	 * message: no trailer, nothing to do. */
	if (vms_scs_inner_frame_len(frame, len, &inner) != VMS_CODEC_OK)
		return 0;
	if (inner >= len)
		return 0;
	/*
	 * AND THE BOUND HAS TO BE ONE A MESSAGE COULD REALLY HAVE. Every SCS
	 * message reaches at least its SYSAP body (abs 72, VMS_OFF_SYSAP_BODY);
	 * a frame declaring an inner length shorter than that is one whose
	 * inner-length word was never filled in, and probing the rest of it as
	 * a trailer would read an ordinary message's own bytes as a block
	 * header. Skipped -- and NOT counted as an unnamed block frame, because
	 * it is not a block frame at all.
	 */
	if (inner < VMS_OFF_SYSAP_BODY)
		return 0;

	if (vms_blk_trailer_parse(frame, len, inner, &view) != VMS_CODEC_OK)
		return 0;

	if (!blk_rx_take(f, vc, &view))
		return 0;
	f->blk_rx_trailer++;
	return 1;
}

const char *pe_blk_status_name(enum pe_blk_status s)
{
	switch (s) {
	case PE_BLK_OK:        return "ok";
	case PE_BLK_NOBUF:     return "no-such-buffer";
	case PE_BLK_NOSPACE:   return "buffer-table-full";
	case PE_BLK_RANGE:     return "outside-buffer";
	case PE_BLK_NOCIRCUIT: return "no-circuit";
	case PE_BLK_BADFRAME:  return "bad-frame";
	case PE_BLK_TOOBIG:    return "too-big";
	case PE_BLK_TXFAIL:    return "transmit-failed";
	case PE_BLK_PERM:      return "buffer-access";
	case PE_BLK_NONAME:    return "peer-name-absent";
	case PE_BLK_INVAL:     return "invalid-argument";
	default:               return "?";
	}
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

	/* FC-P1.6: the go-back-N gap counter and the most recent teardown
	 * reason, both real pe_vc counters (INV-6) -- rx_gaps is incremented
	 * only where the receive path actually discards an ahead-of-window
	 * frame, and last_down_reason is written only where vc_notify_down()
	 * actually fires (never guessed here). */
	out->rx_gaps = vc->rx_gaps;
	out->down_reason = vc->last_down_reason;
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
	case PE_VC_DOWN_RETRANSMIT_EXHAUSTED: return "retransmit limit";
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

/*
 * The pure half of the port-wide view (FC-P0.9, integration note E11): every
 * field here is a straight read of a counter this FSM itself increments --
 * n_channels/n_vcs the high-water marks pe_channel_alloc/pe_vc_alloc leave
 * behind, rx_frames every call to pe_fsm_rx, rx_drops_badclass the two ways a
 * received SCA frame fails to become a discovery or SCS envelope (classified
 * into neither family, or classified and then failed to decode). Nothing here
 * is a placeholder: an FSM that has seen nothing projects all-zero.
 */
void pe_fsm_view_project(const struct pe_fsm *f, struct vms_pe_view *out)
{
	if (out == NULL)
		return;
	pe_bzero(out, (uint32_t)sizeof(*out));
	if (f == NULL)
		return;

	out->n_channels = f->n_channels;
	out->n_vcs = f->n_vcs;
	out->rx_frames = f->rx_frames;
	out->rx_drops_badclass = f->rx_unclassified + f->rx_parse_failed;
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
