// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_vc.c - virtual-circuit formation + sequenced-message envelope
 * codec entries, rung R1 (FC-P1.1).
 *
 * Four groups:
 *   1. Fixture round trip (fixtures/scs-start*.spec, scs-credit.spec):
 *      parse -> typed struct -> build back from ONLY the typed struct,
 *      assert every CITED byte matches -- same discipline
 *      test_codec_hello.c established.
 *   2. Hand-built whole-frame byte-exact proof against the REAL captured
 *      VAX1 frames this item ported from tests/vmsscs/test_scs_start.c /
 *      test_scs_vc.c (formation-ci1-joinwindow.pcap raw frames 23/27/34).
 *      This is the REQUIRED targeted unit test the strawman's own comment
 *      calls out; the fixture round trip in group 1 does not replace it
 *      because the fixtures deliberately leave the two live timestamp
 *      quadwords and the credit-return's inferred secondary counter
 *      UNCITED.
 *   3. The incarnation-field accessor (spec sec 4(i).B, the established-
 *      join GATE): the builder echoes the caller-supplied incarnation
 *      verbatim, decoupled from send_seq, across N in {1,2,3}; the parser
 *      recovers it from a received frame.
 *   4. The sequenced-message envelope (msgtype 0x4b/0x5b, recv_ack@32,
 *      send_seq@34) and error paths.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_vc.h"

#include <string.h>

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

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/* Test identities, carried over verbatim from tests/vmsscs/test_scs_start.c /
 * test_scs_vc.c so the ported whole-frame assertions stay apples-to-apples. */
static const uint8_t ovmx_mac[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t ovmx_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x06, 0x04 };
static const uint8_t vax1_mac[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
static const uint8_t vax2_log[6] = { 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04 };
static const uint8_t vax2_hw[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };

/* ---- group 1: fixture round trip, cited bytes only ------------------- */

static void assert_cited_bytes_match(const struct vms_fixture *f,
				     const uint8_t *built, uint32_t n,
				     const char *label)
{
	uint32_t i, checked = 0, mismatches = 0;
	char what[224];

	for (i = 0; i < n; i++) {
		if (!vms_fixture_is_cited(f, i, 1))
			continue;
		checked++;
		if (built[i] != f->bytes[i]) {
			if (mismatches == 0)
				printf("       %s: first cited mismatch at abs "
				       "%u: built %02x, specimen %02x\n",
				       label, i, built[i], f->bytes[i]);
			mismatches++;
		}
	}
	snprintf(what, sizeof(what),
		 "%s: every CITED byte in [0,%u) is byte-exact (%u checked)",
		 label, n, checked);
	ct_check(mismatches == 0 && checked > 0, what);
}

static void roundtrip_start_fixture(const char *fname)
{
	const struct vms_fixture *f = fixture(fname);
	struct vms_frame_info fi;
	struct vms_scs_start_frame sf;
	uint8_t built[VMS_SCS_START_FRAME_LEN];
	uint32_t written = 0;
	char what[192];
	vms_codec_status_t st;

	ct_check(f != NULL, fname);
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(fi.cls == VMS_FCLS_SCS_START, "classified as VMS_FCLS_SCS_START");

	snprintf(what, sizeof(what), "%s: parses", fname);
	ct_check(vms_scs_start_parse(f->bytes, f->wire_len, &fi, &sf)
		 == VMS_CODEC_OK, what);

	memset(built, 0xAA, sizeof(built));
	if (sf.is_ack) {
		snprintf(what, sizeof(what), "%s: builds as the round-2 ACK", fname);
		st = vms_scs_start_build_ack(&sf, built, sizeof(built), &written);
		ct_check(st == VMS_CODEC_OK, what);
		ct_check_eq_u32(written, VMS_SCS_START_ACK_FRAME_LEN,
				"ACK build wrote 60 bytes");
	} else {
		snprintf(what, sizeof(what), "%s: builds as START/STACK", fname);
		st = vms_scs_start_build(&sf, built, sizeof(built), &written);
		ct_check(st == VMS_CODEC_OK, what);
		ct_check_eq_u32(written, VMS_SCS_START_FRAME_LEN,
				"START build wrote 120 bytes");
	}

	assert_cited_bytes_match(f, built, f->wire_len, fname);
}

static void roundtrip_credit_fixture(void)
{
	const char *fname = "scs-credit-return-short";
	const struct vms_fixture *f = fixture(fname);
	struct vms_frame_info fi;
	struct vms_scs_credit_frame cf;
	uint8_t built[VMS_SCS_CREDIT_FRAME_LEN];
	uint32_t written = 0;

	ct_check(f != NULL, fname);
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(fi.cls == VMS_FCLS_SCS_CREDIT, "classified as VMS_FCLS_SCS_CREDIT");

	ct_check(vms_scs_credit_parse(f->bytes, f->wire_len, &fi, &cf)
		 == VMS_CODEC_OK, "parses");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_scs_credit_build(&cf, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "builds");
	ct_check_eq_u32(written, VMS_SCS_CREDIT_FRAME_LEN, "wrote 60 bytes (runt-padded)");

	assert_cited_bytes_match(f, built, f->wire_len, fname);
}

static void test_fixture_roundtrips(void)
{
	printf("-- fixture round trip: parse -> typed fields -> build\n");
	roundtrip_start_fixture("scs-start-vax2-config-round0");
	roundtrip_start_fixture("scs-start-ack-round2");
	roundtrip_credit_fixture();
}

/* ---- group 2: whole-frame byte-exact vs REAL captured VAX1 frames ---- */

/* formation-ci1-joinwindow.pcap raw frame 23: VAX1->VAX2 round-0 START,
 * SCSSYSTEMID 1025, node "VAX1    ". Ported byte-for-byte from
 * tests/vmsscs/test_scs_start.c's real_start_vax1. */
static const uint8_t real_start_vax1[120] = {
	0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
	0x60, 0x07, 0x68, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
	0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x00,
	0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x40, 0x02, 0xd8, 0x00,
	0x56, 0x4d, 0x53, 0x20, 0x56, 0x37, 0x2e, 0x33, 0x66, 0x15, 0x66, 0x7a,
	0x93, 0x00, 0xbc, 0x00, 0x56, 0x41, 0x58, 0x20, 0x06, 0x00, 0x00, 0x0a,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x00, 0x56, 0x41, 0x58, 0x31,
	0x20, 0x20, 0x20, 0x20, 0x80, 0x98, 0xb1, 0x55, 0x96, 0x00, 0xbc, 0x00,
};
/* raw frame 27: VAX1's round-2 ACK. */
static const uint8_t real_ack_vax1[60] = {
	0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
	0x60, 0x07, 0x2c, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
	0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x41, 0x13, 0x00, 0x00, 0x01, 0x00,
	0x01, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
};
/* formation-ci1-joinwindow.pcap SCA idx 34: VAX1->VAX2 credit-ack,
 * acked_seq=2, secondary=1. Ported from tests/vmsscs/test_scs_vc.c. */
static const uint8_t real_credit_vax1[60] = {
	0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9, 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,
	0x60, 0x07, 0x27, 0x00, 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x00,
	0xaa, 0x00, 0x04, 0x00, 0x01, 0x04, 0x48, 0x13, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x12, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void fill_real_start_frame(struct vms_scs_start_frame *f)
{
	memset(f, 0, sizeof(*f));
	memcpy(f->addr.dst_mac, vax2_hw, 6);
	memcpy(f->addr.src_mac, vax1_mac, 6);
	memcpy(f->addr.dst_logical, vax2_log, 6);
	memcpy(f->addr.src_logical, vax1_mac, 6);
	f->recv_ack = 0;
	f->send_seq = 1;
	f->incarnation = 1;
	f->config_round = 0;
	f->scssystemid = 1025;
	memcpy(f->software_version, "VMS V7.3", 8);
	f->incarnation_time = 0x0093667a661566ULL; /* le64(real+66): boot time */
	memcpy(f->hardware_type, "VAX ", 4);
	f->credits = 10;
	memcpy(f->node_name, "VAX1    ", 8);
	/* The two live quadwords are read straight from the reference frame:
	 * this codec has no template to fall back to, so the whole-frame
	 * byte-exact proof must supply the real captured values itself. */
	f->incarnation_time = le64(real_start_vax1 + 80);
	f->message_time = le64(real_start_vax1 + 112);
}

static void test_whole_frame_start_byte_exact(void)
{
	struct vms_scs_start_frame f;
	uint8_t out[VMS_SCS_START_FRAME_LEN];
	uint32_t written = 0;

	printf("-- whole-frame byte-exact: 0x41 START vs real VAX1 capture\n");
	fill_real_start_frame(&f);

	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_start_build(&f, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "vms_scs_start_build succeeds");
	ct_check_eq_u32(written, sizeof(real_start_vax1), "wrote 120 bytes");
	ct_check(memcmp(out, real_start_vax1, sizeof(real_start_vax1)) == 0,
		 "reproduces the real VAX1 round-0 START byte-for-byte (120 bytes)");
}

static void test_whole_frame_ack_byte_exact(void)
{
	struct vms_scs_start_frame f;
	uint8_t out[VMS_SCS_START_ACK_FRAME_LEN];
	uint32_t written = 0;

	printf("-- whole-frame byte-exact: 0x41 round-2 ACK vs real VAX1 capture\n");
	memset(&f, 0, sizeof(f));
	memcpy(f.addr.dst_mac, vax2_hw, 6);
	memcpy(f.addr.src_mac, vax1_mac, 6);
	memcpy(f.addr.dst_logical, vax2_log, 6);
	memcpy(f.addr.src_logical, vax1_mac, 6);
	f.recv_ack = 0;
	f.send_seq = 1;
	f.incarnation = 1;

	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_start_build_ack(&f, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "vms_scs_start_build_ack succeeds");
	ct_check_eq_u32(written, sizeof(real_ack_vax1), "wrote 60 bytes");
	ct_check(memcmp(out, real_ack_vax1, sizeof(real_ack_vax1)) == 0,
		 "reproduces the real VAX1 round-2 ACK byte-for-byte (60 bytes)");
}

static void test_parse_real_start_and_ack(void)
{
	struct vms_frame_info fi;
	struct vms_scs_start_frame v;

	printf("-- parse real captured 0x41 START/ACK frames\n");
	ct_check(vms_frame_classify(real_start_vax1, sizeof(real_start_vax1), &fi)
		 == VMS_CODEC_OK, "real START classifies");
	ct_check(fi.cls == VMS_FCLS_SCS_START, "real START classified SCS_START");
	ct_check(vms_scs_start_parse(real_start_vax1, sizeof(real_start_vax1),
				     &fi, &v) == VMS_CODEC_OK,
		 "parses real VAX1 START");
	ct_check(!v.is_ack, "not flagged as an ACK");
	ct_check_eq_u32(v.config_round, 0, "config_round == 0 (GROUNDED)");
	ct_check_eq_u32(v.scssystemid, 1025, "SCSSYSTEMID == 1025 (VAX1, GROUNDED)");
	ct_check_eq_u32(v.send_seq, 1, "send_seq == 1 (GROUNDED joiner-phase value)");
	ct_check_eq_u32(v.recv_ack, 0, "leading counter == 0 (GROUNDED)");
	ct_check(memcmp(v.software_version, "VMS V7.3", 8) == 0,
		 "software_version recovered verbatim");
	ct_check(memcmp(v.node_name, "VAX1    ", 8) == 0,
		 "node_name recovered verbatim");
	ct_check_eq_u32(v.credits, 10, "credits == 10 (SYSGEN CLUSTER_CREDITS)");

	ct_check(vms_frame_classify(real_ack_vax1, sizeof(real_ack_vax1), &fi)
		 == VMS_CODEC_OK, "real ACK classifies");
	ct_check(vms_scs_start_parse(real_ack_vax1, sizeof(real_ack_vax1), &fi, &v)
		 == VMS_CODEC_OK, "parses real VAX1 ACK");
	ct_check(v.is_ack, "flagged as an ACK");
	ct_check_eq_u32(v.config_round, 2, "config_round == 2 (GROUNDED)");
}

static void test_whole_frame_credit_byte_exact(void)
{
	struct vms_scs_credit_frame c;
	uint8_t out[VMS_SCS_CREDIT_FRAME_LEN];
	uint32_t written = 0;

	printf("-- whole-frame byte-exact: 0x48 credit-return vs real VAX1 capture\n");
	memset(&c, 0, sizeof(c));
	memcpy(c.addr.dst_mac, vax2_hw, 6);
	memcpy(c.addr.src_mac, vax1_mac, 6);
	memcpy(c.addr.dst_logical, vax2_log, 6);
	memcpy(c.addr.src_logical, vax1_mac, 6);
	c.acked_seq = 2;
	c.secondary_seq = 1;

	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_credit_build(&c, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "vms_scs_credit_build succeeds");
	ct_check_eq_u32(written, sizeof(real_credit_vax1), "wrote 60 bytes (runt-padded)");
	ct_check(memcmp(out, real_credit_vax1, sizeof(real_credit_vax1)) == 0,
		 "reproduces the real VAX1 0x48 credit-return byte-for-byte (60 bytes, runt-padded)");
}

static void test_credit_field_map(void)
{
	struct vms_scs_credit_frame c;
	uint8_t out[VMS_SCS_CREDIT_FRAME_LEN];
	uint32_t written = 0;

	printf("-- 0x48 credit-return: field map (spec sec 4h(3))\n");
	memset(&c, 0, sizeof(c));
	memcpy(c.addr.dst_mac, vax1_mac, 6);
	memcpy(c.addr.src_mac, ovmx_mac, 6);
	memcpy(c.addr.dst_logical, vax1_mac, 6);
	memcpy(c.addr.src_logical, ovmx_logical, 6);
	c.acked_seq = 7;
	c.secondary_seq = 3;

	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_credit_build(&c, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "build ok (OVMX->VAX1)");

	ct_check(memcmp(out + 0, vax1_mac, 6) == 0, "Ethernet dst == peer MAC");
	ct_check(memcmp(out + 6, ovmx_mac, 6) == 0, "Ethernet src == OVMX HW MAC");
	ct_check(out[12] == 0x60 && out[13] == 0x07, "ethertype 0x6007");
	ct_check(out[14] == 0x27 && out[15] == 0x00,
		 "SCA length 0x0027 (total 41, GROUNDED)");
	ct_check(memcmp(out + 16, vax1_mac, 6) == 0,
		 "SCA dst-logical == peer_logical (abs 16)");
	ct_check(le16(out + 22) == 0x0001,
		 "connect flag == 0x0001 (abs 22, GROUNDED)");
	ct_check(memcmp(out + 24, ovmx_logical, 6) == 0,
		 "SCA src-logical == cluster-LOGICAL addr, NOT HW MAC (abs 24, vms-9f3)");
	ct_check(memcmp(out + 24, ovmx_mac, 6) != 0,
		 "src-logical (abs 24) DISTINCT from raw HW MAC (vms-9f3)");
	ct_check(out[30] == 0x48 && out[31] == 0x13,
		 "opcode 0x48, format 0x13 (abs 30/31, GROUNDED)");
	ct_check(le16(out + 32) == 7, "acked seq (abs 32, GROUNDED)");
	ct_check(le16(out + 34) == 0, "send-seq == 0 (abs 34, GROUNDED 622/622)");
	ct_check(le16(out + 36) == 1, "const 0x0001 (abs 36, GROUNDED 622/622)");
	ct_check(le16(out + 38) == 18, "NISCS_LAN_OVRHD (abs 38, GROUNDED)");
	ct_check(le16(out + 40) == 7, "acked-seq mirror (abs 40, GROUNDED 622/622)");
	ct_check(le16(out + 42) == 0, "zero (abs 42)");
	ct_check(le16(out + 44) == 3, "secondary counter (abs 44, INFERRED)");
	ct_check(le16(out + 46) == 0, "zero (abs 46)");
	ct_check(le16(out + 48) == 7, "acked-seq 3rd repeat (abs 48, GROUNDED 616/622)");
	ct_check(le16(out + 50) == 0, "zero (abs 50)");
	ct_check(le16(out + 52) == 1, "const 0x0001 (abs 52, INFERRED)");
	ct_check(out[54] == 0, "pad byte zero (abs 54)");
	ct_check_eq_u32(written, 60, "credit frame is runt-padded to 60 bytes on the wire");
}

/* ---- group 3: the incarnation-field accessor (spec sec 4(i).B GATE) -- */

static void test_incarnation_echo(void)
{
	struct vms_scs_start_frame f;
	uint8_t out[VMS_SCS_START_FRAME_LEN];
	uint16_t n;

	printf("-- incarnation echo: 0x41 START abs 36 == member-advertised N "
	       "(spec 4i.B)\n");
	memset(&f, 0, sizeof(f));
	memcpy(f.addr.dst_mac, vax1_mac, 6);
	memcpy(f.addr.src_mac, ovmx_mac, 6);
	memcpy(f.addr.dst_logical, vax1_mac, 6);
	memcpy(f.addr.src_logical, ovmx_logical, 6);
	f.scssystemid = 1030;
	memcpy(f.node_name, "OVMX    ", 8);
	f.config_round = 0;
	f.send_seq = 1;
	f.recv_ack = 0;

	for (n = 1; n <= 3; n++) {
		uint32_t written = 0;

		f.incarnation = n;
		memset(out, 0xAA, sizeof(out));
		ct_check(vms_scs_start_build(&f, out, sizeof(out), &written)
			 == VMS_CODEC_OK, "build START with incarnation N");
		ct_check_eq_u32(le16(out + 36), n,
				"abs 36 echoes the member-advertised N");
		ct_check_eq_u32(le16(out + 34), 1,
				"send_seq (abs 34) stays 1, independent of N");
		ct_check_eq_u32(le16(out + 44), 1,
				"send_seq mirror (abs 44) stays 1, independent of N");
		ct_check_eq_u32(le16(out + 32), 0,
				"recv_ack (abs 32) stays 0, independent of N");
	}

	/* N and send_seq are wholly decoupled -- a large residual member
	 * send_seq (spec 4i.A) must never leak into the incarnation field. */
	{
		uint32_t written = 0;

		f.incarnation = 2;
		f.send_seq = 11974; /* the af2 residual-VC continuation value */
		memset(out, 0xAA, sizeof(out));
		ct_check(vms_scs_start_build(&f, out, sizeof(out), &written)
			 == VMS_CODEC_OK, "build with N=2 and a large send_seq");
		ct_check_eq_u32(le16(out + 36), 2, "abs 36 == incarnation 2, NOT send_seq");
		ct_check_eq_u32(le16(out + 34), 11974, "send_seq carries its own value");
		ct_check_eq_u32(le16(out + 44), 11974, "mirror == send_seq, not incarnation");
	}

	/* The accessor round-trips through the parser too. */
	{
		struct vms_frame_info fi;
		struct vms_scs_start_frame parsed;
		uint32_t written = 0;

		f.incarnation = 3;
		f.send_seq = 1;
		memset(out, 0xAA, sizeof(out));
		ct_check(vms_scs_start_build(&f, out, sizeof(out), &written)
			 == VMS_CODEC_OK, "build with N=3 for the parse round trip");
		ct_check(vms_frame_classify(out, written, &fi) == VMS_CODEC_OK,
			 "classifies");
		ct_check(vms_scs_start_parse(out, written, &fi, &parsed)
			 == VMS_CODEC_OK, "parses");
		ct_check_eq_u32(parsed.incarnation, 3,
				"parser recovers the incarnation accessor");
	}
}

static void test_live_timestamps_are_never_a_template(void)
{
	struct vms_scs_start_frame f;
	uint8_t out[VMS_SCS_START_FRAME_LEN];
	uint32_t written = 0;
	const uint64_t t1 = 0x00bc04d17ccba480ULL;
	const uint64_t t2 = 0x00bc0552abcdef00ULL;

	printf("-- incarnation_time/message_time: exactly what the caller "
	       "supplies, no template fallback (vms-2f3 honesty rule)\n");
	memset(&f, 0, sizeof(f));
	memcpy(f.addr.dst_mac, vax1_mac, 6);
	memcpy(f.addr.src_mac, ovmx_mac, 6);
	memcpy(f.addr.dst_logical, vax1_mac, 6);
	memcpy(f.addr.src_logical, ovmx_logical, 6);
	f.scssystemid = 1030;
	memcpy(f.software_version, "OVMX V01", 8);
	memcpy(f.node_name, "OVMX    ", 8);
	f.send_seq = 1;
	f.incarnation = 1;

	/* Unlike the strawman scs_start.c, 0 is not a magic "leave the
	 * template" sentinel -- this codec has no template. It writes
	 * exactly what it is given, honestly, every time. */
	f.incarnation_time = 0;
	f.message_time = 0;
	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_start_build(&f, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "build with both quadwords 0");
	ct_check(le64(out + 80) == 0, "incarnation_time 0 lands as literal 0, not a template");
	ct_check(le64(out + 112) == 0, "message_time 0 lands as literal 0, not a template");

	f.incarnation_time = t1;
	f.message_time = t2;
	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_start_build(&f, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "build with live quadwords");
	ct_check(le64(out + 80) == t1, "incarnation_time lands byte-exact at abs 80");
	ct_check(le64(out + 112) == t2, "message_time lands byte-exact at abs 112, INDEPENDENT");
	ct_check(memcmp(out + 72, "OVMX V01", 8) == 0,
		 "software_version (abs 72) untouched by the quadword writes");
	ct_check(memcmp(out + 104, "OVMX    ", 8) == 0,
		 "node_name (abs 104) untouched by the quadword writes");
}

/* ---- group 4: the sequenced-message envelope + error paths ----------- */

static void test_seq_envelope_build_parse(void)
{
	struct vms_scs_seq_envelope e, parsed;
	struct vms_frame_info fi;
	uint8_t out[VMS_SCS_SEQ_ENVELOPE_LEN];
	uint32_t written = 0;

	printf("-- sequenced-message envelope: msgtype 0x4b/0x5b, recv_ack@32, "
	       "send_seq@34\n");

	memset(&e, 0, sizeof(e));
	memcpy(e.addr.dst_mac, vax1_mac, 6);
	memcpy(e.addr.src_mac, ovmx_mac, 6);
	memcpy(e.addr.dst_logical, vax1_mac, 6);
	memcpy(e.addr.src_logical, ovmx_logical, 6);
	e.msgtype = VMS_SCS_MT_MSG; /* 0x4b */
	e.recv_ack = 5;
	e.send_seq = 9;

	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_seq_envelope_build(&e, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "builds a 0x4b envelope");
	ct_check_eq_u32(written, VMS_SCS_SEQ_ENVELOPE_LEN, "wrote 36 bytes");
	ct_check(out[30] == 0x4b && out[31] == 0x13, "msgtype 0x4b, format 0x13");
	ct_check_eq_u32(le16(out + 32), 5, "recv_ack lands at abs 32");
	ct_check_eq_u32(le16(out + 34), 9, "send_seq lands at abs 34");

	ct_check(vms_frame_classify(out, written, &fi) == VMS_CODEC_OK,
		 "classifies");
	ct_check(vms_scs_seq_envelope_parse(out, written, &fi, &parsed)
		 == VMS_CODEC_OK, "parses back");
	ct_check_eq_u32(parsed.msgtype, VMS_SCS_MT_MSG, "msgtype round-trips");
	ct_check_eq_u32(parsed.recv_ack, 5, "recv_ack round-trips");
	ct_check_eq_u32(parsed.send_seq, 9, "send_seq round-trips");

	/* 0x5b connection-setup is the other class this item's scope covers. */
	e.msgtype = VMS_SCS_MT_SETUP;
	memset(out, 0xAA, sizeof(out));
	ct_check(vms_scs_seq_envelope_build(&e, out, sizeof(out), &written)
		 == VMS_CODEC_OK, "builds a 0x5b envelope");
	ct_check(out[30] == 0x5b, "msgtype 0x5b");

	/* Anything outside {0x4b, 0x5b} is refused: this item's scope is
	 * exactly the two sequenced classes, not every SCS message. */
	e.msgtype = VMS_SCS_MT_START;
	ct_check(vms_scs_seq_envelope_build(&e, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "0x41 refused (not this item's scope)");
	e.msgtype = VMS_SCS_MT_CREDIT;
	ct_check(vms_scs_seq_envelope_build(&e, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "0x48 refused (not this item's scope)");
}

static void test_seq_envelope_refuses_wrong_class(void)
{
	struct vms_frame_info fi;
	struct vms_scs_seq_envelope parsed;

	printf("-- sequenced-message envelope refuses a non-sequenced frame\n");
	ct_check(vms_frame_classify(real_start_vax1, sizeof(real_start_vax1), &fi)
		 == VMS_CODEC_OK, "real START classifies");
	ct_check(vms_scs_seq_envelope_parse(real_start_vax1,
					    sizeof(real_start_vax1), &fi, &parsed)
		 == VMS_CODEC_E_CLASS,
		 "reading a 0x41 START through the seq envelope accessor is refused");

	ct_check(vms_frame_classify(real_credit_vax1, sizeof(real_credit_vax1), &fi)
		 == VMS_CODEC_OK, "real credit-return classifies");
	ct_check(vms_scs_seq_envelope_parse(real_credit_vax1,
					    sizeof(real_credit_vax1), &fi, &parsed)
		 == VMS_CODEC_E_CLASS,
		 "reading a 0x48 credit-return through the seq envelope accessor is refused");
}

static void test_error_paths(void)
{
	struct vms_scs_start_frame f;
	struct vms_scs_credit_frame c;
	struct vms_scs_seq_envelope e;
	uint8_t out[VMS_SCS_START_FRAME_LEN];
	uint8_t small[8];
	uint32_t written = 0;
	struct vms_frame_info fi_bad;

	printf("-- error paths: NULL args, undersized buffers, class refusal\n");
	memset(&f, 0, sizeof(f));
	memset(&c, 0, sizeof(c));
	memset(&e, 0, sizeof(e));
	memset(&fi_bad, 0, sizeof(fi_bad));

	ct_check(vms_scs_start_build(NULL, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL start params rejected");
	ct_check(vms_scs_start_build(&f, NULL, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL start output rejected");
	ct_check(vms_scs_start_build(&f, small, sizeof(small), &written)
		 == VMS_CODEC_E_RANGE, "undersized start buffer -> E_RANGE");

	ct_check(vms_scs_start_build_ack(NULL, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL ack params rejected");
	ct_check(vms_scs_start_build_ack(&f, small, sizeof(small), &written)
		 == VMS_CODEC_E_RANGE, "undersized ack buffer -> E_RANGE");

	ct_check(vms_scs_credit_build(NULL, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL credit params rejected");
	ct_check(vms_scs_credit_build(&c, NULL, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL credit output rejected");
	ct_check(vms_scs_credit_build(&c, small, sizeof(small), &written)
		 == VMS_CODEC_E_RANGE, "undersized credit buffer -> E_RANGE");

	ct_check(vms_scs_seq_envelope_build(NULL, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL envelope params rejected");
	e.msgtype = VMS_SCS_MT_MSG;
	ct_check(vms_scs_seq_envelope_build(&e, small, sizeof(small), &written)
		 == VMS_CODEC_E_RANGE, "undersized envelope buffer -> E_RANGE");

	fi_bad.cls = VMS_FCLS_SCS_START;
	ct_check(vms_scs_start_parse(NULL, sizeof(out), &fi_bad, &f)
		 == VMS_CODEC_E_INVAL, "NULL frame rejected by start parse");
	ct_check(vms_scs_start_parse(out, sizeof(out), NULL, &f)
		 == VMS_CODEC_E_INVAL, "NULL frame_info rejected by start parse");
	fi_bad.cls = VMS_FCLS_SCS_CREDIT;
	ct_check(vms_scs_start_parse(real_credit_vax1, sizeof(real_credit_vax1),
				     &fi_bad, &f) == VMS_CODEC_E_CLASS,
		 "start parse refuses a mismatched class");

	fi_bad.cls = VMS_FCLS_SCS_START;
	ct_check(vms_scs_credit_parse(real_start_vax1, sizeof(real_start_vax1),
				      &fi_bad, &c) == VMS_CODEC_E_CLASS,
		 "credit parse refuses a mismatched class");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_vc: VC formation + sequenced-message codec entries "
	       "(FC-P1.1)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_fixture_roundtrips();
	test_whole_frame_start_byte_exact();
	test_whole_frame_ack_byte_exact();
	test_parse_real_start_and_ack();
	test_whole_frame_credit_byte_exact();
	test_credit_field_map();
	test_incarnation_echo();
	test_live_timestamps_are_never_a_template();
	test_seq_envelope_build_parse();
	test_seq_envelope_refuses_wrong_class();
	test_error_paths();

	return ct_summary("test_codec_vc");
}
