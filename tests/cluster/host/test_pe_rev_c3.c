// SPDX-License-Identifier: GPL-2.0
/*
 * test_pe_rev_c3.c - the PORT against a REAL OpenVMS VAX V5.5-2H4 node's own
 * HELLO bytes (rd vms-0f8), rung R1.
 *
 * WHY THIS TEST EXISTS. After rd vms-147 fixed the group->multicast
 * derivation, Node A (OVMX x86_64, cluster group 257) and Node C (an
 * unmodified OpenVMS VAX V5.5-2H4 volume, group 257) were finally addressed to
 * each other and each other's frames reached the executive -- Node A's own
 * SHOW CLUSTER/LOCAL_PORTS climbed rx 0 -> 553 in an eight-minute in-browser
 * run. CN=2 still did not form, because ALL 553 counted `badclass`: OVMX's
 * discovery codec was grounded entirely on OpenVMS VAX V7.3 captures, and
 * V5.5-2H4 speaks a second revision of the same HELLO.
 *
 * So the stimulus here is NOT a synthesised peer. It is the exact 128 bytes
 * that real V5.5 machine put on the wire, loaded from the clean-room-hashed
 * specimen, and pushed straight into pe_fsm_rx(). The assertions are the two
 * halves of the fix:
 *
 *   1. RECEIVE -- the frame stops being badclass. It classifies, it parses,
 *      a channel appears, and the peer IDENTITY the join FSM needs (SCSNODE,
 *      SCSSYSTEMID, hardware MAC) comes out of the real bytes.
 *   2. TRANSMIT -- what the port then puts on the wire toward that peer is in
 *      THE PEER'S revision, carrying THIS node's own identity. The revision's
 *      three differing marker words are echoed (they are format markers, read
 *      off the peer's real frame); nothing that asserts anything ABOUT THIS
 *      NODE is ever echoed. That is the INV-6 line, and it is asserted
 *      field by field below.
 *
 * Plus the no-regression control: the same port, given a V7.3 peer instead,
 * stays on the sec 4(a)/4(b) revision and emits the 134-byte frame it always
 * did.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "pe_fake_ops.h"

/* Node A's identity in the browser demo: SCSNODE OVMXA, SCSSYSTEMID 1987. */
#define NODEA_SYSID 1987u
#define VAXC_SYSID  1989u
#define DEMO_GROUP  257u

static const uint8_t nodea_hw[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t vaxc_hw[6]  = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x0c };
static const uint8_t vax1_hw[6]  = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };

struct env {
	struct pe_fsm  fsm;
	struct pe_ops  ops;
	struct fake_pe fake;
	uint8_t        mcast[VMS_ETH_ADDR_LEN];
	uint8_t        buf[VMS_HELLO_PADDED_MAX_FRAME];
};

static struct env g_env;      /* ~13 KB of FSM: static, not a stack frame */
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

/* The demo's Node A, exactly as inject-cluster-config.sh configures it. */
static void env_init(struct env *e)
{
	struct pe_identity id;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);
	vms_cluster_hello_mcast_build(DEMO_GROUP, e->mcast);

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, nodea_hw, VMS_ETH_ADDR_LEN);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMXA ", VMS_HELLO_NODENAME_MAX);
	id.scsnode_len = 5;
	memcpy(id.mcast, e->mcast, VMS_ETH_ADDR_LEN);
	id.mcast_valid = 1;
	/* abs 22 of every frame this node emits: the SAME group the mcast
	 * address above encodes, DEMO_GROUP (rd vms-b34). */
	id.cluster_group = (uint16_t)DEMO_GROUP;
	id.cluster_group_valid = 1u;
	id.max_sca_len = 1500;

	(void)pe_fsm_init(&e->fsm, &id, NODEA_SYSID, &e->ops);
	pe_fsm_start(&e->fsm);
}

/* ------------------------------------------------------------------ *
 * 1. RECEIVE: the real V5.5 bytes stop being badclass
 * ------------------------------------------------------------------ */
static void test_real_v55_hello_is_received(void)
{
	const struct vms_fixture *f = fixture("hello-c3-vaxc-v55-multicast");
	struct env *e = &g_env;
	struct vms_pe_view view;
	struct pe_channel *ch;
	uint16_t sysid = 0;

	printf("-- a REAL OpenVMS VAX V5.5-2H4 HELLO through pe_fsm_rx()\n");
	ct_check(f != NULL, "the V5.5 specimen loads from the clean-room corpus");
	if (f == NULL)
		return;
	ct_check_eq_u32(f->wire_len, VMS_HELLO_C3_FRAME_LEN,
			"  the specimen is 128 bytes on the wire");

	env_init(e);
	(void)pe_fsm_rx(&e->fsm, f->bytes, f->wire_len);

	ct_check_eq_u32(e->fsm.rx_frames, 1, "the port saw the frame");
	ct_check_eq_u32(e->fsm.rx_not_sca, 0, "  it is an 0x6007 SCA frame");
	ct_check_eq_u32(e->fsm.rx_unclassified, 0,
			"  UNCLASSIFIED == 0 (this was 553/553 before the fix)");
	ct_check_eq_u32(e->fsm.rx_parse_failed, 0, "  and it decoded");
	ct_check_eq_u32(e->fsm.rx_not_for_us, 0,
			"  addressed to OUR group's multicast address");
	ct_check_eq_u32(e->fsm.rx_hello_c03, 1,
			"  counted as a class-0x03-revision HELLO");

	/* This is the number the demo page prints as `badclass`. */
	pe_fsm_view_project(&e->fsm, &view);
	ct_check_eq_u32(view.rx_drops_badclass, 0,
			"the port-wide badclass drop count is 0");
	ct_check_eq_u32(view.n_channels, 1, "a channel exists for the station");

	/* The peer identity the join FSM acts on -- every value off the wire. */
	ch = pe_fsm_channel_at(&e->fsm, 0);
	ct_check(ch != NULL, "the channel is addressable");
	if (ch == NULL)
		return;
	ct_check(memcmp(ch->remote_mac, vaxc_hw, VMS_ETH_ADDR_LEN) == 0,
		 "  remote HW MAC == VAXC's, off the frame's Ethernet source");
	ct_check_eq_u32(ch->remote_name_len, 6, "  remote name length 6");
	ct_check(memcmp(ch->remote_name, "VAXC  ", 6) == 0,
		 "  remote SCSNODE == \"VAXC  \"");
	ct_check(ch->remote_lavc_valid, "  a cluster-LOGICAL address was learned");
	ct_check(ch->remote_sysid_valid, "  and a SCSSYSTEMID with it");
	sysid = (uint16_t)ch->remote_sysid;
	ct_check_eq_u32(sysid, VAXC_SYSID, "  SCSSYSTEMID == 1989");
	ct_check(ch->state != (uint8_t)VMS_PE_CH_CLOSED,
		 "  and the channel FSM left CLOSED -- the station is SEEN");
}

/* ------------------------------------------------------------------ *
 * 2. LEARN: the port now knows which revision this cluster speaks
 * ------------------------------------------------------------------ */
static void test_revision_is_learned_not_assumed(void)
{
	const struct vms_fixture *f = fixture("hello-c3-vaxc-v55-multicast");
	struct env *e = &g_env;
	struct pe_channel *ch;

	printf("-- the revision is LEARNED off the peer's own frame\n");
	if (f == NULL)
		return;

	env_init(e);
	ct_check(!e->fsm.wire_rev.valid,
		 "before any peer is heard: nothing learned, the grounded "
		 "sec 4(b) default is in use");

	(void)pe_fsm_rx(&e->fsm, f->bytes, f->wire_len);
	ch = pe_fsm_channel_at(&e->fsm, 0);
	ct_check(ch != NULL, "channel exists");
	if (ch == NULL)
		return;

	ct_check(ch->peer_rev.valid, "the CHANNEL learned its peer's revision");
	ct_check_eq_u32(ch->peer_rev.rev, VMS_HELLO_REV_C03, "  == C03");
	ct_check_eq_u32(ch->peer_rev.trailer_9205, 0x0590, "  abs 94 likewise");
	ct_check_eq_u32(ch->peer_rev.trailer_2600, 0x0021, "  abs 126 likewise");

	ct_check(e->fsm.wire_rev.valid, "and the PORT adopted it for multicast");
	ct_check_eq_u32(e->fsm.wire_rev.rev, VMS_HELLO_REV_C03, "  == C03");
}

/* Decode the frame at `index` and report which revision it is. */
static int emitted_revision(const struct fake_pe *fk, uint32_t index,
			    struct fake_pe_decoded *out)
{
	*out = fake_pe_decode(fk, index);
	return out->ok;
}

/* ------------------------------------------------------------------ *
 * 3. TRANSMIT: the reply is in the peer's revision, with OUR identity
 * ------------------------------------------------------------------ */
static void test_reply_is_in_the_peers_revision(void)
{
	const struct vms_fixture *f = fixture("hello-c3-vaxc-v55-multicast");
	struct env *e = &g_env;
	struct fake_pe_decoded d;
	uint8_t nodea_lavc[VMS_ETH_ADDR_LEN];
	uint32_t i, c03 = 0, c05 = 0;

	printf("-- what the port puts on the wire toward that peer\n");
	if (f == NULL)
		return;

	env_init(e);
	(void)pe_fsm_rx(&e->fsm, f->bytes, f->wire_len);
	fake_pe_clear_frames(&e->fake);

	/* One cadence beat: the port advertises and works its channels. */
	e->fake.now_ms += 4000u;
	(void)pe_fsm_tick(&e->fsm, NULL, 0);

	ct_check(e->fake.n_frames > 0, "the port transmitted");
	for (i = 0; i < e->fake.n_frames; i++) {
		if (!emitted_revision(&e->fake, i, &d))
			continue;
		if (d.h.revision == (uint8_t)VMS_HELLO_REV_C03)
			c03++;
		else
			c05++;
	}
	ct_check(c03 > 0 && c05 == 0,
		 "every frame emitted is in the peer's C03 revision");

	/* Field by field on the first one: format markers echoed, identity
	 * NEVER echoed. This is the INV-6 line. */
	ct_check(emitted_revision(&e->fake, 0, &d), "the first frame re-decodes");
	if (!d.ok)
		return;
	ct_check_eq_u32(d.len, VMS_HELLO_C3_FRAME_LEN,
			"  128 bytes on the wire, not 134");
	ct_check_eq_u32(d.fi.cls, VMS_FCLS_HELLO_C3,
			"  and it classifies as the C03 class");
	ct_check_eq_u32(d.fi.sca_content, VMS_HELLO_C3_SCA_LEN,
			"  SCA content 114");
	ct_check_eq_u32(d.fi.len_check, VMS_SCA_LEN_EXACT,
			"  self-consistent under the sec 2 length identity");

	/* abs 22 is THIS node's own group (rd vms-b34) -- it agrees with the
	 * peer because they are in the same cluster, NOT because it is echoed:
	 * test_group_is_ours_never_the_peers() below holds the revision fixed
	 * and changes only the group to prove it. */
	ct_check_eq_u32(d.h.hdr.cluster_group, DEMO_GROUP,
			"  abs 22 is OUR OWN cluster group (257), not an echo");
	ct_check_eq_u32(d.h.trailer_9205, 0x0590,
			"  FORMAT MARKER abs 94 echoed from the peer's frame");
	ct_check_eq_u32(d.h.trailer_2600, 0x0021, "  abs 126 likewise");

	vms_cluster_lavc_addr_build(NODEA_SYSID, nodea_lavc);
	ct_check(memcmp(d.h.hdr.src_lavc, nodea_lavc, VMS_ETH_ADDR_LEN) == 0,
		 "  IDENTITY abs 24 is OUR SCSSYSTEMID 1987, never the peer's");
	ct_check(memcmp(d.h.hdr.eth_src, nodea_hw, VMS_ETH_ADDR_LEN) == 0,
		 "  Ethernet source is OUR MAC");
	ct_check(memcmp(d.h.hw_mac, nodea_hw, VMS_ETH_ADDR_LEN) == 0,
		 "  abs 120 is OUR hardware MAC");
	ct_check_eq_u32(d.h.disc.namelen, 5, "  name length is OURS (5)");
	ct_check(memcmp(d.h.disc.name, "OVMXA", 5) == 0,
		 "  and the SCSNODE is OVMXA, not VAXC");
}

/* ------------------------------------------------------------------ *
 * 4. The sec 4(k) size probe is DECLINED, not extrapolated
 * ------------------------------------------------------------------ */
static void test_padded_probe_declined_for_c03(void)
{
	const struct vms_fixture *f = fixture("hello-c3-vaxc-v55-multicast");
	struct env *e = &g_env;
	struct vms_frame_info fi;
	struct vms_hello_frame real;
	struct fake_peer vaxc;
	struct pe_channel *ch;
	struct fake_pe_decoded d;
	uint8_t nodea_lavc[VMS_ETH_ADDR_LEN];
	uint32_t i, len, over_128 = 0;

	printf("-- the sec 4(k) padded size probe against a C03 peer\n");
	if (f == NULL)
		return;

	env_init(e);
	(void)pe_fsm_rx(&e->fsm, f->bytes, f->wire_len);

	/*
	 * Drive the channel to VERIFIED, which is where sec 4(k) step 2 fires.
	 * The b4 has to be DIRECTED, and no directed C03 frame exists in any
	 * capture -- so it is built through the codec from the revision THE
	 * REAL FRAME declared, the same standing every other stimulus in this
	 * harness has. Not one marker word is typed in here.
	 */
	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK &&
		 vms_hello_parse(f->bytes, f->wire_len, &fi, &real) == VMS_CODEC_OK,
		 "the real V5.5 frame re-parses, to source the revision from");
	fake_peer_init(&vaxc, VAXC_SYSID, vaxc_hw, "VAXC");
	vaxc.cluster_group = (uint16_t)DEMO_GROUP;   /* the demo's own cluster */
	fake_peer_set_revision(&vaxc, &real);
	ct_check_eq_u32(vaxc.revision, VMS_HELLO_REV_C03,
			"  the stimulus peer speaks the revision the capture did");

	vms_cluster_lavc_addr_build(NODEA_SYSID, nodea_lavc);
	fake_pe_clear_frames(&e->fake);
	for (i = 0; i < 3; i++) {
		e->fake.now_ms += 4000u;
		(void)pe_fsm_tick(&e->fsm, NULL, 0);
		len = fake_peer_hello(&vaxc, nodea_hw, nodea_lavc, 0xb4u,
				      1u, 0u, e->buf, sizeof(e->buf));
		ct_check(len == VMS_HELLO_C3_FRAME_LEN,
			 "  the peer's directed b4 is a 128-byte C03 frame");
		(void)pe_fsm_rx(&e->fsm, e->buf, len);
	}

	ch = pe_fsm_channel_at(&e->fsm, 0);
	ct_check(ch != NULL, "channel exists");
	if (ch == NULL)
		return;
	ct_check_eq_u32(ch->state, VMS_PE_CH_B4,
			"  the plain b3/b4 channel verify still completes");
	ct_check(ch->probe_rev_unsupported,
		 "the port RECORDED that it declined to probe this revision");
	ct_check_eq_u32(ch->probe_sca_len, 0, "  no probe is in flight");
	ct_check_eq_u32(ch->padded_tx, 0, "  and none was ever sent");
	ct_check_eq_u32(ch->verified_pktsz, 0,
			"  so no packet size is CLAIMED for this channel");

	for (i = 0; i < e->fake.n_frames; i++) {
		if (emitted_revision(&e->fake, i, &d) &&
		    d.len > VMS_HELLO_C3_FRAME_LEN)
			over_128++;
	}
	ct_check_eq_u32(over_128, 0,
			"  nothing larger than a 128-byte HELLO went out");
	ct_check(e->fake.n_frames > 0,
		 "  but the port did keep speaking (plain HELLOs, not silence)");
}

/* ------------------------------------------------------------------ *
 * 5. NO REGRESSION: a V7.3 peer keeps the sec 4(a)/4(b) revision
 * ------------------------------------------------------------------ */
static void test_v73_peer_is_unchanged(void)
{
	struct env *e = &g_env;
	struct fake_peer vax1;
	struct fake_pe_decoded d;
	uint32_t len, i, c05 = 0, other = 0;

	printf("-- control: the same port against a V7.3 peer\n");
	env_init(e);
	fake_peer_init(&vax1, 1025, vax1_hw, "VAX1");
	vax1.cluster_group = (uint16_t)DEMO_GROUP;   /* same cluster as Node A */
	len = fake_peer_hello(&vax1, e->mcast, e->mcast, 0xa0u, 0u, 0u,
			      e->buf, sizeof(e->buf));
	ct_check(len == VMS_HELLO_FRAME_LEN,
		 "the V7.3 peer's HELLO is 134 bytes");
	(void)pe_fsm_rx(&e->fsm, e->buf, len);

	ct_check_eq_u32(e->fsm.rx_unclassified, 0, "  it classifies");
	ct_check_eq_u32(e->fsm.rx_hello_c03, 0,
			"  and is NOT counted as the C03 revision");
	ct_check(e->fsm.wire_rev.valid && e->fsm.wire_rev.rev == VMS_HELLO_REV_C05,
		 "  the port learned the C05 revision from it");

	fake_pe_clear_frames(&e->fake);
	e->fake.now_ms += 4000u;
	(void)pe_fsm_tick(&e->fsm, NULL, 0);
	for (i = 0; i < e->fake.n_frames; i++) {
		if (!emitted_revision(&e->fake, i, &d))
			continue;
		if (d.h.revision == (uint8_t)VMS_HELLO_REV_C05)
			c05++;
		else
			other++;
	}
	ct_check(c05 > 0 && other == 0,
		 "  and every frame it emits is still the C05 revision");
}

/*
 * A port that has heard NOBODY must emit exactly what it emitted before this
 * item: the whole no-regression claim rests on this default.
 */
static void test_default_before_any_peer(void)
{
	struct env *e = &g_env;
	struct fake_pe_decoded d;

	printf("-- the default, before any peer has been heard\n");
	env_init(e);
	e->fake.now_ms += 4000u;
	(void)pe_fsm_tick(&e->fsm, NULL, 0);

	ct_check(e->fake.n_frames > 0, "the port advertises unprompted");
	ct_check(emitted_revision(&e->fake, 0, &d), "  the frame re-decodes");
	if (!d.ok)
		return;
	ct_check_eq_u32(d.h.revision, VMS_HELLO_REV_C05,
			"  in the grounded sec 4(a)/4(b) revision");
	ct_check_eq_u32(d.len, VMS_HELLO_FRAME_LEN, "  134 bytes");
	ct_check_eq_u32(d.h.hdr.cluster_group, DEMO_GROUP,
			"  abs 22 == this node's OWN group, with no peer heard");
	ct_check_eq_u32(d.h.trailer_9205, 0x0592, "  abs 94 == 0x0592");
	ct_check_eq_u32(d.h.trailer_2600, 0x0026, "  abs 126 == 0x0026");
}

/*
 * NEVER CRASH A PEER. Node C is a real VAX; a malformed or truncated frame
 * from it -- or from anything else on that LAN -- must leave the port counting,
 * not faulting, and must never invent a channel out of a frame it could not
 * read.
 */
static void test_truncations_never_fault(void)
{
	const struct vms_fixture *f = fixture("hello-c3-vaxc-v55-multicast");
	struct env *e = &g_env;
	uint8_t clip[VMS_HELLO_FRAME_LEN];
	uint32_t i, b;

	printf("-- never crash a peer: every truncation and bit-flip\n");
	if (f == NULL)
		return;

	env_init(e);
	for (i = 0; i <= f->wire_len; i++) {
		memcpy(clip, f->bytes, i);
		(void)pe_fsm_rx(&e->fsm, clip, i);
	}
	/* Only the ONE complete frame may have produced a channel. */
	ct_check_eq_u32(e->fsm.n_channels, 1,
			"129 truncations later: exactly one channel, from the "
			"one complete frame");
	ct_check_eq_u32(e->fsm.rx_frames, f->wire_len + 1u,
			"  every one of them was counted");

	env_init(e);
	for (b = 0; b < f->wire_len; b++) {
		memcpy(clip, f->bytes, f->wire_len);
		clip[b] ^= 0xffu;
		(void)pe_fsm_rx(&e->fsm, clip, f->wire_len);
	}
	ct_check_eq_u32(e->fsm.rx_frames, f->wire_len,
			"  and every single-byte corruption was counted too");
	ct_check(e->fsm.rx_not_sca + e->fsm.rx_unclassified +
		 e->fsm.rx_not_for_us + e->fsm.rx_parse_failed > 0u,
		 "  corruption really is being REFUSED (not a vacuous pass)");

	/* INV-6 on the corrupted corpus: no channel may carry a SCSSYSTEMID
	 * that was not read out of a genuine cluster-LOGICAL address. */
	for (i = 0; i < e->fsm.n_channels; i++) {
		struct pe_channel *ch = pe_fsm_channel_at(&e->fsm, i);

		if (ch == NULL || !ch->remote_sysid_valid)
			continue;
		if (!vms_cluster_lavc_is_logical(ch->remote_lavc))
			break;
	}
	ct_check(i == e->fsm.n_channels,
		 "  and no channel learned a sysid from a non-LOGICAL address");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_pe_rev_c3: the port vs a real V5.5-2H4 HELLO (rd vms-0f8)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_real_v55_hello_is_received();
	test_revision_is_learned_not_assumed();
	test_reply_is_in_the_peers_revision();
	test_padded_probe_declined_for_c03();
	test_v73_peer_is_unchanged();
	test_default_before_any_peer();
	test_truncations_never_fault();

	return ct_summary("test_pe_rev_c3");
}
