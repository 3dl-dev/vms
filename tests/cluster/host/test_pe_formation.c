/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_formation.c - replay a captured channel formation at the FC-P0.8
 * channel FSM and assert the frames it emits have the reference JOINER's shape.
 *
 * Plan FC-P0.8's second done-condition, rung R1: "replays a captured channel
 * formation and emits the reference joiner's frames (shape)". Design SS3.9
 * spells out why this rung exists: "feed the captured VAX frames to one
 * simulated OVMX instance and assert its emitted frames against the reference
 * joiner's" -- the whole CN=3 campaign was one re-fire per ~30 minutes against a
 * live lab, and this runs the same scenarios in microseconds.
 *
 * TWO SOURCES OF STIMULUS, and the difference matters:
 *
 *   - The clean-room FIXTURES (hello-directed.spec, hello-padded.spec) are
 *     manifest-hashed specimens composed from the wire spec's own GROUNDED
 *     field tables, citing the captures the spec cites. The loader refuses a
 *     specimen whose capture is not in docs/clean-room/reference-captures.sha256.
 *     These are the two frames the reference member actually sends at a joiner:
 *     the b2 INIT and the SS4(k) padded size probe.
 *   - The rest of the formation is built with the SAME FC-P0.7 codec the FSM
 *     emits through, from a typed peer identity. It is a codec-built frame, not
 *     a capture extract, and is labelled as such -- no test here claims to be
 *     replaying bytes it does not have.
 *
 * EVERY assertion on an emitted frame goes through the codec: classify, parse,
 * read the class-gated abs-30 accessor. A test that compared bytes at
 * hand-written offsets would be re-implementing the thing under test.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "pe_fake_ops.h"

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	}
	return NULL;
}

/* ------------------------------------------------------------------ *
 * The node under test
 * ------------------------------------------------------------------ */

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t vax2_hw[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

static struct pe_fsm  g_fsm;      /* ~13 KB: static, not a stack frame */
static struct pe_ops  g_ops;
static struct fake_pe g_fake;
static uint8_t        g_buf[VMS_HELLO_PADDED_MAX_FRAME];

/*
 * Bring the port up with `sysid`, so that a given fixture's DESTINATION
 * cluster-LOGICAL address is this node's. SS4(a).0: abs 16 is the addressing
 * field; the Ethernet destination is only where the frame is delivered, and the
 * two are different addresses on any node whose HW MAC is not a DECnet one --
 * which is exactly the case OVMX is.
 */
static void port_up(uint16_t sysid, uint16_t max_sca_len)
{
	struct pe_identity id;

	fake_pe_ops_init(&g_ops, &g_fake);
	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = max_sca_len;

	(void)pe_fsm_init(&g_fsm, &id, sysid, &g_ops);
	pe_fsm_start(&g_fsm);
}

static enum pe_channel_action feed(const uint8_t *frame, uint32_t len)
{
	return pe_fsm_rx(&g_fsm, frame, len);
}

static enum pe_channel_action feed_fixture(const struct vms_fixture *f)
{
	return feed(f->bytes, f->wire_len);
}

/* One HELLO from a codec-built peer. */
static enum pe_channel_action feed_peer(const struct fake_peer *p, uint16_t sysid,
					int directed, uint8_t word,
					uint16_t incarnation, uint16_t padded)
{
	uint8_t dst_lavc[6];
	uint32_t len;

	if (directed)
		vms_cluster_lavc_addr_build(sysid, dst_lavc);
	else
		memcpy(dst_lavc, group1, 6);
	len = fake_peer_hello(p, directed ? ovmx_hw : group1, dst_lavc, word,
			      incarnation, padded, g_buf, sizeof(g_buf));
	if (len == 0)
		return PE_CH_ACT_NONE;
	return feed(g_buf, len);
}

/* ------------------------------------------------------------------ *
 * 1. The GROUNDED b2 INIT: OVMX answers b3, addressed the SS4(a).0 way
 * ------------------------------------------------------------------ */
static void test_answer_the_captured_b2(void)
{
	const struct vms_fixture *fx = fixture("hello-directed-vax2-to-vax1");
	struct fake_pe_decoded d;
	uint8_t our_lavc[6], peer_lavc[6];

	printf("-- SS4(a).1: the captured b2 INIT is answered with b3\n");
	if (fx == NULL) {
		ct_check(0, "fixture hello-directed-vax2-to-vax1 present");
		return;
	}

	/* This node stands in the specimen's DESTINATION slot (SCSSYSTEMID
	 * 1025, SS3's decoder ring) -- the joiner the member is INITing -- while
	 * keeping its own, different hardware MAC. That mismatch between abs 0
	 * and abs 16 is precisely the case SS4(a).0 says a 2-node lab cannot
	 * exhibit. */
	port_up(1025, 1500);
	vms_cluster_lavc_addr_build(1025, our_lavc);
	vms_cluster_lavc_addr_build(1026, peer_lavc);

	(void)feed_fixture(fx);
	ct_check_eq_u32(g_fake.n_frames, 1, "exactly one frame answers the b2");

	d = fake_pe_decode(&g_fake, 0);
	ct_check(d.ok, "the answer classifies and parses as a HELLO");
	ct_check_eq_u32(d.len, VMS_HELLO_FRAME_LEN, "134 bytes (SS4(b))");
	ct_check_eq_u32(d.fi.cls, (unsigned)VMS_FCLS_HELLO, "frame class hello");
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B3, "abs 30 carries b3");

	ct_check(memcmp(d.h.hdr.eth_dst, vax2_hw, 6) == 0,
		 "delivered to the member's HARDWARE MAC (abs 0)");
	ct_check(memcmp(d.h.hdr.dst_lavc, peer_lavc, 6) == 0,
		 "addressed to the member's cluster-LOGICAL address (abs 16)");
	ct_check(memcmp(d.h.hdr.dst_lavc, d.h.hdr.eth_dst, 6) != 0,
		 "abs 16 is NOT a mirror of abs 0 -- the SS4(a).0 failure mode");
	ct_check(memcmp(d.h.hdr.src_lavc, our_lavc, 6) == 0,
		 "our own LOGICAL address at abs 24");
	ct_check(memcmp(d.h.hdr.eth_src, ovmx_hw, 6) == 0,
		 "our REAL hardware MAC as the Ethernet source");
	ct_check(memcmp(d.h.hw_mac, ovmx_hw, 6) == 0,
		 "and again at abs 120, as SS4(b) grounds");
	ct_check_eq_u32(d.h.incarnation, 1,
			"our own directed HELLO carries incarnation 1 (SS4(i).B)");
	ct_check_eq_u32(d.h.poller_sweep, PE_POLLER_SWEEP_DIRECTED,
			"poller sweep 31 on a directed HELLO (SS4(b))");
	ct_check(d.h.disc.namelen == 6 && memcmp(d.h.disc.name, "OVMX  ", 6) == 0,
		 "our own SCSNODE, not the specimen's");

	/* SS5.3 is OPEN: this executive holds no cluster token, so a zero goes
	 * out and is COUNTED. The specimen's ee053 95b is NOT harvested off the
	 * wire and replayed -- that is the fabrication this design forbids. */
	ct_check(d.h.disc.nonce[0] == 0 && d.h.disc.nonce[1] == 0 &&
		 d.h.disc.nonce[2] == 0 && d.h.disc.nonce[3] == 0,
		 "no credential is invented: the nonce goes out zero");
	ct_check(g_fsm.nonce_absent > 0,
		 "and the absence is counted, not hidden (design SS5.3)");
}

/* ------------------------------------------------------------------ *
 * 2. The GROUNDED SS4(k) padded probe: OVMX reciprocates
 * ------------------------------------------------------------------ */
static void test_reciprocate_the_captured_padded_hello(void)
{
	const struct vms_fixture *fx =
		fixture("hello-padded-vax1-channel-size-verify");
	struct fake_pe_decoded plain, padded;

	printf("-- SS4(k): a padded HELLO is answered b4 AND reciprocated "
	       "padded\n");
	if (fx == NULL) {
		ct_check(0, "fixture hello-padded-vax1-channel-size-verify present");
		return;
	}

	/* The specimen is addressed to SS3's VAX2 LOGICAL address. */
	port_up(1026, 1500);
	(void)feed_fixture(fx);

	/* SS4(k)'s golden-vs-ci3 contrast: the frame present in the successful
	 * formation and absent in the stalled one is the joiner's OWN padded
	 * HELLO on the reverse channel. Zero of those is the join wall. */
	ct_check_eq_u32(g_fake.n_frames, 2,
			"the b4 CONFIRM and the reciprocal padded HELLO");

	plain = fake_pe_decode(&g_fake, 0);
	ct_check(plain.ok && plain.fi.cls == (uint8_t)VMS_FCLS_HELLO,
		 "first: a plain 134-byte HELLO");
	ct_check_eq_u32(plain.chan_word, PE_PFW_VERIFY_B4,
			"carrying b4 -- the ~0.2 ms CONFIRM SS4(a).1 grounds");

	padded = fake_pe_decode(&g_fake, 1);
	ct_check(padded.ok && padded.fi.cls == (uint8_t)VMS_FCLS_HELLO_PADDED,
		 "second: our OWN padded HELLO on the reverse channel");
	ct_check_eq_u32(padded.len, 1514, "at the probed size (1500 SCA + 14)");
	ct_check_eq_u32(padded.fi.sca_content, 1500,
			"NISCS_MAX_PKTSZ 1498 + 2, byte-exact against SYSGEN");
	ct_check_eq_u32(padded.chan_word, PE_PFW_VERIFY_B3,
			"and it carries b3: a padded HELLO is a verify REQUEST");
	ct_check_eq_u32(padded.h.poller_sweep, PE_POLLER_SWEEP_DIRECTED,
			"it is a DIRECTED HELLO, pad and all");

	/* Nothing is claimed until the peer confirms it (INV-6). */
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->verified_pktsz, 0,
			"no size is CLAIMED until a b4 confirms the probe");
	{
		struct fake_peer vax1;

		fake_peer_init(&vax1, 1025, vax1_hw, "VAX1");
		(void)feed_peer(&vax1, 1026, 1, PE_PFW_VERIFY_B4, 1, 0);
	}
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->verified_pktsz, 1500,
			"the plain b4 CONFIRMS the padded probe (SS4(a).1)");
}

/* ------------------------------------------------------------------ *
 * 3. The whole formation, from OVMX's side
 * ------------------------------------------------------------------ */
static void test_full_formation_replay(void)
{
	struct fake_peer member;
	struct fake_pe_decoded d;
	unsigned padded_tx = 0, i;

	printf("-- the SS4(a).1 bootstrap end to end, from the joiner's seat\n");
	port_up(1030, 1500);
	fake_peer_init(&member, 1025, vax1_hw, "VAX1");

	/* (1) the member's periodic multicast HELLO: first sight. */
	(void)feed_peer(&member, 1030, 0, PE_PFW_MULTICAST, 0, 0);
	ct_check_eq_u32(g_fake.n_frames, 1,
			"first sight draws exactly one directed HELLO");
	d = fake_pe_decode(&g_fake, 0);
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B3,
			"and it is a b3 REQUEST, never a b2 INIT");

	/* (2) the member INITs:  member b2 -> joiner b3. */
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B2, 1, 0);
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->state,
			(unsigned)VMS_PE_CH_B2, "the INIT is recorded");

	/* (3) the member CONFIRMs: the channel is usable. */
	ct_check_eq_u32(feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0),
			(unsigned)PE_CH_ACT_VERIFIED,
			"the b4 makes the channel usable");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->state,
			(unsigned)VMS_PE_CH_B4, "state B4 reached");

	/* (4) the size verification the member then confirms. */
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0);
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->verified_pktsz, 1500,
			"1500-byte channel proven");

	/* (5) the indefinite b3<->b4 oscillation, member-initiated half. */
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B3, 1, 0);
	d = fake_pe_decode(&g_fake, g_fake.n_frames - 1);
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B4, "b3 is acked with b4");

	/* (6) and our half, on the cadence beat. */
	g_fake.now_ms += 2000;
	(void)pe_fsm_tick(&g_fsm, NULL, 0);
	d = fake_pe_decode(&g_fake, g_fake.n_frames - 1);
	ct_check_eq_u32(d.chan_word, PE_PFW_VERIFY_B3,
			"we re-initiate with a fresh b3 on the poller sweep");

	/* The reference joiner's headline signature: b2 = 0 of everything it
	 * sent, and exactly one padded HELLO on this direction. */
	ct_check_eq_u32(fake_pe_count_word(&g_fake, PE_PFW_VERIFY_B2), 0,
			"OVMX originated ZERO b2 across the whole formation");
	for (i = 0; i < g_fake.n_frames; i++) {
		struct fake_pe_decoded p = fake_pe_decode(&g_fake, i);

		if (p.ok && p.fi.cls == (uint8_t)VMS_FCLS_HELLO_PADDED)
			padded_tx++;
	}
	ct_check_eq_u32(padded_tx, 1,
			"exactly ONE padded HELLO on this direction (SS4(k))");
	ct_check_eq_u32(g_fake.frames_dropped, 0, "every frame was captured");
}

/* ------------------------------------------------------------------ *
 * 4. The cadence (SS4(q)) and the listen timeout (SS4(M))
 * ------------------------------------------------------------------ */
static void test_cadence_and_timeout(void)
{
	struct fake_peer member;
	struct fake_pe_decoded d;
	struct pe_channel_rec rec[4];
	uint32_t n;

	printf("-- SS4(q) cadence and SS4(M) listen timeout, on the injected "
	       "clock\n");
	port_up(1030, 1500);
	ct_check_eq_u32(g_fake.timers_armed, 1, "start arms the beat");
	ct_check_eq_u32(g_fake.last_arm_ms, PE_HELLO_INTERVAL_DEFAULT_MS,
			"at the documented cadence");

	(void)pe_fsm_tick(&g_fsm, NULL, 0);
	ct_check_eq_u32(g_fake.n_frames, 1, "each beat emits one multicast HELLO");
	d = fake_pe_decode(&g_fake, 0);
	ct_check(memcmp(d.h.hdr.eth_dst, group1, 6) == 0, "to the cluster group");
	ct_check_eq_u32(d.chan_word, PE_PFW_MULTICAST, "carrying a0 (SS4(a))");
	ct_check_eq_u32(d.h.incarnation, 0, "incarnation 0 on multicast (SS4(b))");
	ct_check_eq_u32(d.h.poller_sweep, 0, "poller sweep 0 on multicast");
	ct_check(d.h.disc.nonce[0] == 0, "nonce 0 on multicast -- GROUNDED");
	ct_check_eq_u32(g_fake.timers_armed, 2, "and the beat re-arms itself");

	/* A member appears, verifies, then goes quiet. */
	fake_peer_init(&member, 1025, vax1_hw, "VAX1");
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B2, 1, 0);
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0);

	/* SS4(M): the longest healthy silence measured anywhere was 3.153 s. */
	g_fake.now_ms += 3200;
	n = pe_fsm_tick(&g_fsm, rec, 4);
	ct_check_eq_u32(n, 0, "3.2 s of silence is inside the healthy band");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->state,
			(unsigned)VMS_PE_CH_B4, "the channel is still usable");

	g_fake.now_ms += PE_LISTEN_TIMEOUT_DEFAULT_MS;
	n = pe_fsm_tick(&g_fsm, rec, 4);
	ct_check_eq_u32(n, 1, "past the listen timeout the channel reports");
	ct_check_eq_u32(rec[0].action, (unsigned)PE_CH_ACT_LOST, "action LOST");
	/* The slot survives with state CLOSED: the station's identity and its
	 * counters are real history, and a station that comes back is then
	 * recognised rather than re-discovered. What does NOT survive is
	 * anything the ladder proved. */
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->state,
			(unsigned)VMS_PE_CH_CLOSED, "the channel is CLOSED");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->verified_pktsz, 0,
			"and claims no verified size any more");
}

/* ------------------------------------------------------------------ *
 * 5. The SS4(k) down-step ladder when nothing acks
 * ------------------------------------------------------------------ */
static void test_size_ladder_steps_down(void)
{
	struct fake_peer member;
	unsigned i, rung;
	uint16_t seen[64];
	unsigned n_seen = 0;

	printf("-- SS4(k): 1500 x4 -> 1069 x4 -> 853 x4 -> 745 x4, 6.010 s "
	       "apart\n");
	port_up(1030, 1500);
	fake_peer_init(&member, 1025, vax1_hw, "VAX1");
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B2, 1, 0);
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0);
	fake_pe_clear_frames(&g_fake);

	/* Nothing ever ACKS the probe, but the member keeps beaconing, so the
	 * SS4(M) listen timeout never fires and the ladder is what is under
	 * test. One retransmit interval per step. */
	for (i = 0; i < 20; i++) {
		g_fake.now_ms += PE_PROBE_RETRANSMIT_MS;
		(void)feed_peer(&member, 1030, 0, PE_PFW_MULTICAST, 0, 0);
		(void)pe_fsm_tick(&g_fsm, NULL, 0);
	}

	for (i = 0; i < g_fake.n_frames; i++) {
		struct fake_pe_decoded d = fake_pe_decode(&g_fake, i);

		if (d.ok && d.fi.cls == (uint8_t)VMS_FCLS_HELLO_PADDED &&
		    n_seen < 64)
			seen[n_seen++] = d.fi.sca_content;
	}

	/* The FIRST probe went out when the channel reached b4, before this
	 * loop cleared the recorder, so the ladder observed here is three at
	 * 1500 and four at each rung below it. */
	ct_check_eq_u32(n_seen, 15, "15 further probes: 3 + 4 + 4 + 4");
	for (i = 0, rung = 0; i < n_seen; i++) {
		unsigned want_rung = (i < 3) ? 0 : ((i - 3) / 4 + 1);

		if (want_rung >= PE_PROBE_LADDER_N)
			break;
		rung = want_rung;
		if (seen[i] != pe_probe_ladder[rung]) {
			printf("       probe %u: got %u, want %u\n", i,
			       (unsigned)seen[i],
			       (unsigned)pe_probe_ladder[rung]);
			break;
		}
	}
	ct_check(i == n_seen, "every probe sits on the OBSERVED size ladder");
	ct_check_eq_u32(seen[0], 1500, "the first rung is NISCS_MAX_PKTSZ + 2");
	ct_check_eq_u32(seen[n_seen - 1], 745, "the last rung is 745");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->probe_exhausted, 1,
			"past the last rung the FSM STOPS, rather than "
			"inventing a next step");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->verified_pktsz, 0,
			"and no size is ever claimed (INV-6)");
}

/* An interface whose MTU clamps the probe below the top of the ladder. */
static void test_size_ladder_respects_the_mtu(void)
{
	struct fake_peer member;
	struct fake_pe_decoded d;

	printf("-- the probe never exceeds what the interface can carry\n");
	port_up(1030, 900);
	fake_peer_init(&member, 1025, vax1_hw, "VAX1");
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B2, 1, 0);
	fake_pe_clear_frames(&g_fake);
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0);

	d = fake_pe_decode(&g_fake, g_fake.n_frames - 1);
	ct_check(d.ok && d.fi.cls == (uint8_t)VMS_FCLS_HELLO_PADDED,
		 "a probe still goes out");
	ct_check_eq_u32(d.fi.sca_content, 853,
			"at the highest ladder rung the MTU allows");

	/* An interface too small for even the lowest rung claims nothing. */
	port_up(1030, 700);
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B2, 1, 0);
	fake_pe_clear_frames(&g_fake);
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0);
	ct_check_eq_u32(g_fake.n_frames, 0,
			"below the ladder's last rung, no probe is invented");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->verified_pktsz, 0,
			"and no size is claimed");
}

/* ------------------------------------------------------------------ *
 * 6. The SS4(O.30) last gasp -- both directions
 * ------------------------------------------------------------------ */
static void test_last_gasp(void)
{
	struct fake_peer member;
	struct fake_pe_decoded periodic, gasp;
	struct pe_identity id;

	printf("-- SS4(O.30): the clean-leave last gasp, sent and received\n");

	/* RECEIVED: the peer announced its departure, so the channel goes at
	 * once and the caller can take p. 7-29's immediate path. */
	port_up(1030, 1500);
	fake_peer_init(&member, 1025, vax1_hw, "VAX1");
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B2, 1, 0);
	(void)feed_peer(&member, 1030, 1, PE_PFW_VERIFY_B4, 1, 0);
	ct_check_eq_u32(feed_peer(&member, 1030, 0, PE_PFW_LAST_GASP, 0, 0),
			(unsigned)PE_CH_ACT_DEPARTED,
			"a received last gasp is an ANNOUNCED departure");
	ct_check_eq_u32(pe_fsm_channel_at(&g_fsm, 0)->state,
			(unsigned)VMS_PE_CH_CLOSED,
			"and the channel is CLOSED at once, not left to time out");

	/* SENT: byte-for-byte the periodic multicast HELLO except abs 30. This
	 * node holds no cluster token (design SS5.3), so the nonce is honestly
	 * zero and counted -- the marker is what carries the departure. */
	port_up(1030, 1500);
	fake_pe_clear_frames(&g_fake);
	(void)pe_fsm_tick(&g_fsm, NULL, 0);
	ct_check_eq_u32(pe_fsm_send_last_gasp(&g_fsm), 0, "the last gasp builds");
	ct_check_eq_u32(g_fake.n_frames, 2, "a periodic HELLO, then the gasp");

	periodic = fake_pe_decode(&g_fake, 0);
	gasp = fake_pe_decode(&g_fake, 1);
	ct_check(periodic.ok && gasp.ok, "both decode through the codec");
	ct_check_eq_u32(gasp.len, VMS_HELLO_FRAME_LEN,
			"the gasp is a 120-byte-content multicast HELLO");
	ct_check_eq_u32(periodic.chan_word, PE_PFW_MULTICAST, "periodic: a0");
	ct_check_eq_u32(gasp.chan_word, PE_PFW_LAST_GASP, "gasp: b1");
	ct_check(memcmp(gasp.h.hdr.eth_dst, group1, 6) == 0,
		 "to the cluster group, like the periodic one");
	ct_check(memcmp(gasp.h.hdr.src_lavc, periodic.h.hdr.src_lavc, 6) == 0 &&
		 memcmp(gasp.h.hw_mac, periodic.h.hw_mac, 6) == 0 &&
		 gasp.h.incarnation == periodic.h.incarnation &&
		 gasp.h.poller_sweep == periodic.h.poller_sweep,
		 "and identical to it in every identity field");
	ct_check_eq_u32(g_fsm.last_gasps_built, 1, "counted from the real send");

	/* With a token configured it goes out; nothing is invented without one. */
	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.join_nonce[0] = 0xee;
	id.join_nonce[1] = 0x05;
	id.join_nonce[2] = 0x39;
	id.join_nonce[3] = 0x5b;
	id.join_nonce_valid = 1;
	fake_pe_ops_init(&g_ops, &g_fake);
	(void)pe_fsm_init(&g_fsm, &id, 1030, &g_ops);
	ct_check_eq_u32(pe_fsm_send_last_gasp(&g_fsm), 0, "builds with a token");
	gasp = fake_pe_decode(&g_fake, 0);
	ct_check(gasp.h.disc.nonce[0] == 0xee && gasp.h.disc.nonce[3] == 0x5b,
		 "the CONFIGURED token is carried at abs 68 (SS4(O.30))");
	ct_check_eq_u32(g_fsm.nonce_absent, 0,
			"and nothing is counted as absent");
}

/* ------------------------------------------------------------------ *
 * 7. Honest refusals
 * ------------------------------------------------------------------ */
static void test_refusals(void)
{
	struct fake_peer stranger;
	uint8_t frame[64];

	printf("-- what the port refuses, and counts\n");
	port_up(1030, 1500);

	/* A frame for a different node on the same LAN. */
	fake_peer_init(&stranger, 1025, vax1_hw, "VAX1");
	(void)feed_peer(&stranger, 1099, 1, PE_PFW_VERIFY_B2, 1, 0);
	ct_check_eq_u32(g_fsm.rx_not_for_us, 1, "somebody else's frame: counted");
	ct_check_eq_u32(g_fake.n_frames, 0, "and never answered");

	/* Not SCA at all. */
	memset(frame, 0, sizeof(frame));
	frame[12] = 0x08;
	frame[13] = 0x00;
	(void)pe_fsm_rx(&g_fsm, frame, sizeof(frame));
	ct_check_eq_u32(g_fsm.rx_not_sca, 1, "a non-0x6007 frame: counted");

	/* A SOLICIT: OVMX has no MSCP server, so it answers nothing (SS4(p)). */
	{
		uint32_t len = fake_peer_solicit(&stranger, group1, g_buf,
						 sizeof(g_buf));

		ct_check(len > 0, "a SOLICIT specimen builds");
		(void)pe_fsm_rx(&g_fsm, g_buf, len);
	}
	ct_check_eq_u32(g_fsm.rx_solicit, 1, "the SOLICIT is counted");
	ct_check_eq_u32(g_fake.n_frames, 0, "and answered with NOTHING");

	/* A transmit that fails is recorded, not swallowed. */
	g_fake.send_fails = 1;
	(void)feed_peer(&stranger, 1030, 1, PE_PFW_VERIFY_B3, 1, 0);
	ct_check(g_fsm.tx_errors > 0, "a failed exec_lan_xmit is counted");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_pe_formation: channel formation replay (FC-P0.8)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_answer_the_captured_b2();
	test_reciprocate_the_captured_padded_hello();
	test_full_formation_replay();
	test_cadence_and_timeout();
	test_size_ladder_steps_down();
	test_size_ladder_respects_the_mtu();
	test_last_gasp();
	test_refusals();

	return ct_summary("test_pe_formation");
}
