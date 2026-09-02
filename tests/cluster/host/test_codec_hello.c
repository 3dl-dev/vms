// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_hello.c - HELLO/SOLICIT codec entries, rung R1 (FC-P0.7).
 *
 * Three groups:
 *   1. Fixture round trip: parse each hello/solicit fixture into the typed
 *      struct, build back from ONLY the typed struct (never the fixture
 *      buffer), and assert every CITED byte is reproduced exactly. Uncited
 *      spans (the abs 47-67 "version-ish"/"unknown" bytes, the abs 96-101
 *      live timer, a few sec-4(b) trailer words the spec prints as hex but
 *      does not fix the byte order of) are skipped, exactly as
 *      vms_fixture_is_cited() gates test_codec_roundtrip.c -- this is the
 *      honest-gap discipline FC-P0.6 established, applied per-byte because
 *      the discovery family's cited spans are not one contiguous prefix.
 *   2. A hand-built HELLO (ported from tests/vmsscs/test_scs_hello.c):
 *      every field lands at its documented offset, INCLUDING the two the
 *      strawman replayed as compiled-in constants (cap_span, reserved_64) --
 *      here they come from the caller, proving the codec asserts no
 *      semantics of its own for them (the honest-software-field fix). Also
 *      ports the last-gasp byte-diff assertion (spec sec 4(O.30)) as two
 *      vms_hello_build() calls differing only in word30/nonce -- no
 *      separate "last gasp" function exists; there is no separate wire
 *      shape to build.
 *   3. The sysid<->LOGICAL-LAVC-address helpers (spec sec 4(a): abs 24 is
 *      aa:00:04:00:<LE16(sysid)>, never the raw HW MAC).
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_hello.h"

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

static void roundtrip_hello_class(const char *fname)
{
	const struct vms_fixture *f = fixture(fname);
	struct vms_frame_info fi;
	struct vms_hello_frame h;
	uint8_t built[VMS_HELLO_PADDED_MAX_FRAME];
	uint32_t written = 0;
	char what[192];

	ct_check(f != NULL, fname);
	if (f == NULL) {
		printf("       (fixture missing -- corpus regression)\n");
		return;
	}

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");

	snprintf(what, sizeof(what), "%s: parses as a HELLO family frame", fname);
	ct_check(vms_hello_parse(f->bytes, f->wire_len, &fi, &h) == VMS_CODEC_OK,
		 what);

	memset(built, 0xAA, sizeof(built)); /* poison: catch an unwritten byte */
	if (fi.cls == VMS_FCLS_HELLO_PADDED) {
		uint16_t sca_len = (uint16_t)(f->wire_len - VMS_ETH_HDR_LEN);

		snprintf(what, sizeof(what), "%s: builds as a padded HELLO", fname);
		ct_check(vms_hello_build_padded(&h, sca_len, built, sizeof(built),
						&written) == VMS_CODEC_OK, what);
		ct_check_eq_u32(written, f->wire_len,
				"padded build wrote the full on-wire length");
	} else {
		snprintf(what, sizeof(what), "%s: builds as a plain HELLO", fname);
		ct_check(vms_hello_build(&h, built, sizeof(built), &written)
			 == VMS_CODEC_OK, what);
		ct_check_eq_u32(written, VMS_HELLO_FRAME_LEN,
				"plain build wrote VMS_HELLO_FRAME_LEN");
	}

	assert_cited_bytes_match(f, built, f->wire_len, fname);
}

static void roundtrip_solicit(void)
{
	const char *fname = "solicit-vax3-satellite-boot";
	const struct vms_fixture *f = fixture(fname);
	struct vms_frame_info fi;
	struct vms_solicit_frame s;
	uint8_t built[256];
	uint32_t written = 0;

	ct_check(f != NULL, fname);
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "SOLICIT classifies without error");
	ct_check(fi.cls == VMS_FCLS_SOLICIT, "  classified as VMS_FCLS_SOLICIT");

	ct_check(vms_solicit_parse(f->bytes, f->wire_len, &fi, &s) == VMS_CODEC_OK,
		 "SOLICIT parses");
	ct_check_eq_u32(s.devspec_len, 9,
			"  devspec_len == 9 (\"_$2$DUA0:\", GROUNDED sec 4c)");
	ct_check(memcmp(s.devspec, "_$2$DUA0:", 9) == 0,
		 "  devspec == \"_$2$DUA0:\" verbatim");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_solicit_build(&s, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "SOLICIT builds");
	ct_check_eq_u32(written, f->wire_len, "SOLICIT build wrote the full frame");

	/* SOLICIT is fully cited in its fixture (spec sec 4c grounds every
	 * byte of the one specimen) -- this is a genuine whole-frame
	 * byte-exact proof, not a gapped one. */
	assert_cited_bytes_match(f, built, f->wire_len, fname);
}

static void test_fixture_roundtrips(void)
{
	printf("-- fixture round trip: parse -> typed fields -> build\n");
	roundtrip_hello_class("hello-multicast-vax1");
	roundtrip_hello_class("hello-directed-vax2-to-vax1");
	roundtrip_hello_class("hello-padded-vax1-channel-size-verify");
	roundtrip_solicit();
}

/* ---- group 2: hand-built HELLO, every field at its offset ------------- */

static void fill_test_hello(struct vms_hello_frame *h)
{
	static const uint8_t mcast[6]        = { 0xAB, 0x00, 0x04, 0x01, 0x01, 0x01 };
	static const uint8_t test_hw_mac[6]  = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
	static const uint8_t cap_span[VMS_DISC_CAPSPAN_LEN] = {
		'O', 'V', 'M', 'X', ' ', 'v', '0', '.', '1', 0, 0, 0, 0, 0, 0, 0, 0
	};
	static const uint8_t reserved64[VMS_DISC_RESERVED64_LEN] = { 0, 0, 0, 0 };
	uint8_t src_logical[6];

	memset(h, 0, sizeof(*h));

	vms_cluster_lavc_addr_build(1030, src_logical); /* sysid 1030 */

	memcpy(h->hdr.eth_dst, mcast, 6);
	memcpy(h->hdr.eth_src, test_hw_mac, 6);
	h->hdr.sca_len_field = 0x0076; /* 120-byte content, GROUNDED sec 2 */
	memcpy(h->hdr.dst_lavc, mcast, 6);
	h->hdr.connect_flag = 0x0001;
	memcpy(h->hdr.src_lavc, src_logical, 6);
	h->hdr.word30 = 0x00a0; /* multicast per-frame word */

	h->disc.namelen = 4;
	memcpy(h->disc.name, "OVMX  ", 6);
	memcpy(h->disc.cap_span, cap_span, sizeof(cap_span));
	memcpy(h->disc.reserved_64, reserved64, sizeof(reserved64));
	memset(h->disc.nonce, 0, sizeof(h->disc.nonce)); /* zero on multicast */

	h->incarnation = 0x0000;
	h->trailer_9205 = 0x0592; /* LE16 of wire bytes 92 05 */
	{
		uint64_t tick = 0x11223344ull;
		int i;

		for (i = 0; i < 6; i++)
			h->timer_tick[i] = (uint8_t)((tick >> (8 * i)) & 0xffu);
	}
	{
		static const uint8_t tail_const[VMS_HELLO_TAILCONST_LEN] = {
			0xbc, 0x00, 0x03, 0x58, 0x51, 0x41, 0x00, 0x00, 0x00, 0x00
		};
		memcpy(h->tail_const, tail_const, sizeof(tail_const));
	}
	memcpy(h->hw_mac, test_hw_mac, 6);
	h->trailer_2600 = 0x0026;
	h->poller_sweep = 0x0000; /* multicast */
	h->trailer_0064 = 0x0064;
	h->trailer_0000 = 0x0000;
}

static void test_handbuilt_hello_field_placement(void)
{
	struct vms_hello_frame h;
	uint8_t out[VMS_HELLO_FRAME_LEN];
	uint32_t written = 0;

	printf("-- hand-built HELLO: every field lands at its documented offset "
	       "(ported test_scs_hello.c)\n");
	fill_test_hello(&h);
	memset(out, 0xAA, sizeof(out));
	ct_check(vms_hello_build(&h, out, sizeof(out), &written) == VMS_CODEC_OK,
		 "vms_hello_build succeeds");
	ct_check_eq_u32(written, VMS_HELLO_FRAME_LEN, "writes exactly 134 bytes");

	ct_check(memcmp(out + 0, h.hdr.eth_dst, 6) == 0,
		 "Ethernet dst == cluster multicast addr");
	ct_check(memcmp(out + 6, h.hdr.eth_src, 6) == 0,
		 "Ethernet src == OVMX HW MAC");
	ct_check(out[12] == 0x60 && out[13] == 0x07, "Ethertype == 0x6007");
	ct_check(out[14] == 0x76 && out[15] == 0x00,
		 "SCA length field == 0x0076 (-> total 120)");
	ct_check(memcmp(out + 16, h.hdr.dst_lavc, 6) == 0,
		 "dest/group logical addr");
	ct_check(out[22] == 0x01 && out[23] == 0x00, "connect flag == 0x0001");
	ct_check(memcmp(out + 24, h.hdr.src_lavc, 6) == 0,
		 "src-logical addr == cluster-LOGICAL aa:00:04:00:<sysid>");
	ct_check(memcmp(out + 24, h.hdr.eth_src, 6) != 0,
		 "src-logical (abs 24) is DISTINCT from the raw HW MAC");
	ct_check(out[30] == 0xa0 && out[31] == 0x00,
		 "per-frame word == 0xa000 (multicast)");
	ct_check(out[32] == 0x08 && out[33] == 0x00 && out[34] == 0x00 &&
		 out[35] == 0x80, "discovery prefix == 08 00 00 80");
	ct_check(out[36] == 0x05, "message-class byte == 0x05 (HELLO)");
	ct_check(out[37] == 0x01 && out[38] == 0x00 && out[39] == 0x00,
		 "discovery suffix == 01 00 00");
	ct_check(out[40] == 4, "node-name length prefix == caller value (4)");
	ct_check(memcmp(out + 41, "OVMX  ", 6) == 0,
		 "node name == 'OVMX  ' (space-padded, caller-supplied)");
	ct_check(memcmp(out + 47, h.disc.cap_span, VMS_DISC_CAPSPAN_LEN) == 0,
		 "capability/version span == the CALLER'S value, never a "
		 "compiled-in replayed capture constant");
	ct_check(memcmp(out + 64, h.disc.reserved_64, VMS_DISC_RESERVED64_LEN)
		 == 0, "abs 64-67 == the caller's value");
	{
		static const uint8_t zero4[4] = { 0, 0, 0, 0 };

		ct_check(memcmp(out + 68, zero4, 4) == 0,
			 "join nonce == 0x00000000 (multicast HELLO)");
	}
	{
		uint8_t zero20[20];

		memset(zero20, 0, sizeof(zero20));
		ct_check(memcmp(out + 72, zero20, 20) == 0,
			 "abs 72-91 == zero padding (builder-owned, not caller "
			 "poison-fillable)");
	}
	ct_check(out[92] == 0x00 && out[93] == 0x00,
		 "directed-HELLO flag == 0x0000 (multicast)");
	ct_check(out[94] == 0x92 && out[95] == 0x05, "trailer == 0x9205");
	{
		static const uint8_t timer_le[6] = { 0x44, 0x33, 0x22, 0x11, 0x00, 0x00 };

		ct_check(memcmp(out + 96, timer_le, 6) == 0,
			 "timer/tick (abs 96-101) == caller-supplied 48-bit "
			 "value, LE");
	}
	{
		static const uint8_t tail_const[10] = {
			0xbc, 0x00, 0x03, 0x58, 0x51, 0x41, 0x00, 0x00, 0x00, 0x00
		};

		ct_check(memcmp(out + 102, tail_const, 10) == 0,
			 "tail constant (abs 102-111)");
	}
	{
		uint8_t zero8[8];

		memset(zero8, 0, sizeof(zero8));
		ct_check(memcmp(out + 112, zero8, 8) == 0,
			 "abs 112-119 == zero padding");
	}
	ct_check(memcmp(out + 120, h.hw_mac, 6) == 0,
		 "sender's real HW LAN MAC");
	ct_check(out[126] == 0x26 && out[127] == 0x00, "trailer == 0x2600");
	ct_check(out[128] == 0x00 && out[129] == 0x00,
		 "poller-sweep marker == 0x0000 (multicast)");
	ct_check(out[130] == 0x64 && out[131] == 0x00, "trailer == 0x0064");
	ct_check(out[132] == 0x00 && out[133] == 0x00, "trailer == 0x0000");
}

static void test_hello_error_paths(void)
{
	struct vms_hello_frame h;
	uint8_t out[VMS_HELLO_FRAME_LEN];
	uint8_t small[VMS_HELLO_FRAME_LEN - 1];
	uint32_t written = 0;

	printf("-- HELLO error paths\n");
	fill_test_hello(&h);
	ct_check(vms_hello_build(NULL, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL frame struct rejected");
	ct_check(vms_hello_build(&h, NULL, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL output buffer rejected");
	ct_check(vms_hello_build(&h, small, sizeof(small), &written)
		 == VMS_CODEC_E_RANGE, "output buffer too small -> E_RANGE");
	ct_check(vms_hello_build_padded(&h, VMS_HELLO_SCA_LEN - 1, out,
					sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "padded total below VMS_HELLO_SCA_LEN rejected");
	ct_check(vms_hello_build_padded(&h, VMS_HELLO_PADDED_MAX_SCA + 1, out,
					sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "padded total above the sec-4(k) ceiling rejected");
	ct_check(vms_hello_parse(NULL, sizeof(out), NULL, &h)
		 == VMS_CODEC_E_INVAL, "NULL frame_info rejected");
}

/*
 * The port-level clean-leave "last gasp" (spec sec 4(O.30)) is byte-for-byte
 * a plain multicast HELLO differing at EXACTLY two fields: word30
 * (0xa0->0xb1) and the nonce (0->the cluster token). There is no separate
 * wire shape and therefore no separate builder -- ported from
 * tests/vmsscs/test_scs_hello.c's differential assertion, ONE level up:
 * against vms_hello_build() directly instead of a dedicated
 * scs_hello_build_lastgasp_frame().
 */
static void test_lastgasp_is_a_plain_hello_diff(void)
{
	struct vms_hello_frame ref, lg;
	uint8_t out_ref[VMS_HELLO_FRAME_LEN], out_lg[VMS_HELLO_FRAME_LEN];
	uint32_t written = 0;
	static const uint8_t lg_nonce[4] = { 0xee, 0x05, 0x39, 0x5b };
	int i, diff_ok = 1;

	printf("-- last gasp (sec 4(O.30)) is a plain HELLO differing at "
	       "EXACTLY word30 + nonce\n");
	fill_test_hello(&ref);
	lg = ref;
	lg.hdr.word30 = 0x00b1; /* SCS_HELLO_PFW_LASTGASP-equivalent value */
	memcpy(lg.disc.nonce, lg_nonce, 4);

	ct_check(vms_hello_build(&ref, out_ref, sizeof(out_ref), &written)
		 == VMS_CODEC_OK, "reference multicast HELLO builds");
	ct_check(vms_hello_build(&lg, out_lg, sizeof(out_lg), &written)
		 == VMS_CODEC_OK, "last-gasp-shaped HELLO builds");

	for (i = VMS_ETH_HDR_LEN; i < (int)VMS_HELLO_FRAME_LEN; i++) {
		int expected_diff = (i == 30) || (i >= 68 && i <= 71);

		if ((out_ref[i] != out_lg[i]) != expected_diff) {
			diff_ok = 0;
			break;
		}
	}
	ct_check(diff_ok,
		 "byte-diff is EXACTLY abs-30 and abs-68..71, nothing else");
}

/* ---- group 3: sysid <-> LOGICAL LAVC address (spec sec 4a) ------------ */

static void test_lavc_address_helpers(void)
{
	uint8_t addr[VMS_ETH_ADDR_LEN];
	uint16_t sysid = 0;
	static const uint8_t vax1_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
	static const uint8_t not_logical[6]  = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };

	printf("-- sysid <-> aa:00:04:00:<LE16(sysid)> logical LAVC address\n");
	vms_cluster_lavc_addr_build(1025, addr); /* VAX1's SCSSYSTEMID */
	ct_check(memcmp(addr, vax1_logical, 6) == 0,
		 "sysid 1025 -> aa:00:04:00:01:04 (GROUNDED, sec 4a/4a.0)");

	ct_check(vms_cluster_lavc_is_logical(vax1_logical) == 1,
		 "aa:00:04:00:.. recognised as a logical address");
	ct_check(vms_cluster_lavc_is_logical(not_logical) == 0,
		 "a raw HW MAC is NOT a logical address (sec 4a.0)");

	ct_check(vms_cluster_lavc_sysid(vax1_logical, &sysid) == VMS_CODEC_OK,
		 "sysid extracts from a logical address");
	ct_check_eq_u32(sysid, 1025, "  == 1025");

	ct_check(vms_cluster_lavc_sysid(not_logical, &sysid) == VMS_CODEC_E_INVAL,
		 "sysid extraction refuses a non-logical address (INV-6: no "
		 "sysid fabricated from a HW MAC)");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_hello: HELLO/SOLICIT codec entries (FC-P0.7)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_fixture_roundtrips();
	test_handbuilt_hello_field_placement();
	test_hello_error_paths();
	test_lastgasp_is_a_plain_hello_diff();
	test_lavc_address_helpers();

	return ct_summary("test_codec_hello");
}
