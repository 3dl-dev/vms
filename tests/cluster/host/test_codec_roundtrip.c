// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_roundtrip.c - parse -> typed fields -> build, byte-exact, one
 * specimen per frame class (FC-P0.6 done-condition, rung R1).
 *
 * WHAT MAKES THIS AN HONEST ROUND TRIP. The builder is handed ONLY the typed
 * struct the parser produced -- it never sees the specimen buffer -- so the
 * comparison cannot pass by copying. And it compares exactly the class's
 * declared HARVEST SPAN and nothing beyond it: the number of leading bytes the
 * codec can currently reconstruct from named fields. Bytes outside that span
 * are not asserted, and the test prints the coverage so the gap is visible
 * rather than implied.
 *
 * Two guards keep the span honest:
 *   - the harvest span must be entirely CITED in the specimen (a specimen may
 *     not "prove" a byte its provenance header never claimed);
 *   - the span must lie inside the frame.
 *
 * Every later harvest item (FC-P0.7 HELLO, P1.1 VC, P2.1 SCS, P3.1 CM, P4.5
 * DLM, P6.2 MSCP) raises its class's harvest_len and re-runs this same test.
 */

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec.h"

#include <string.h>

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static void report_diff(const struct vms_fixture *f, const uint8_t *got,
			uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (got[i] != f->bytes[i]) {
			printf("       first difference at abs %u: "
			       "built %02x, specimen %02x\n",
			       i, got[i], f->bytes[i]);
			return;
		}
	}
}

static void roundtrip_one(const struct vms_fixture *f)
{
	const struct vms_frame_class_info *ci;
	struct vms_frame_info fi;
	struct vms_sca_hdr hdr;
	uint8_t built[VMS_SCA_HDR_LEN];
	uint32_t written = 0;
	char what[192];

	(void)vms_frame_classify(f->bytes, f->wire_len, &fi);
	ci = vms_frame_class_lookup(fi.cls);
	if (!ci || fi.cls == VMS_FCLS_UNKNOWN)
		return;   /* negative controls are covered by the classifier */

	snprintf(what, sizeof(what),
		 "%s: harvest span [0,%u) is entirely cited in the specimen",
		 f->name, ci->harvest_len);
	ct_check(vms_fixture_is_cited(f, 0, ci->harvest_len), what);

	snprintf(what, sizeof(what), "%s: harvest span fits the frame", f->name);
	ct_check(ci->harvest_len <= f->wire_len, what);

	snprintf(what, sizeof(what), "%s: header parses", f->name);
	ct_check(vms_sca_hdr_parse(f->bytes, f->wire_len, &hdr) == VMS_CODEC_OK,
		 what);

	memset(built, 0, sizeof(built));
	snprintf(what, sizeof(what), "%s: header builds", f->name);
	ct_check(vms_sca_hdr_build(&hdr, built, sizeof(built), &written)
		 == VMS_CODEC_OK, what);

	snprintf(what, sizeof(what), "%s: build wrote the whole harvest span",
		 f->name);
	ct_check_eq_u32(written, ci->harvest_len, what);

	snprintf(what, sizeof(what),
		 "%s: rebuilt [0,%u) is BYTE-EXACT vs the specimen",
		 f->name, ci->harvest_len);
	if (memcmp(built, f->bytes, ci->harvest_len) != 0)
		report_diff(f, built, ci->harvest_len);
	ct_check(memcmp(built, f->bytes, ci->harvest_len) == 0, what);

	/* The length field the frame ASSERTS must survive the round trip. */
	snprintf(what, sizeof(what), "%s: SCA content length preserved", f->name);
	ct_check_eq_u32(vms_sca_content_len(&hdr), fi.sca_content, what);
}

static void test_roundtrip_all(void)
{
	int i;

	printf("-- parse/build round trip, per specimen\n");
	for (i = 0; i < g_n; i++)
		roundtrip_one(&g_fx[i]);
}

/*
 * One specimen per class is the done-condition; assert it directly rather than
 * trusting that the corpus happens to be complete.
 *
 * VMS_FCLS_SCS_APPLMSG (FC-P2.7, design sec3.2.7, E48) is the one documented
 * exception: its only real specimens are the MSCP END lengths (86/90/102/
 * 110), which trace to the vms291 lab-2 mount capture -- a HOST-ONLY pcap
 * artifact never committed and not in the clean-room manifest
 * (docs/design-mscp-direction.md "Host-only artifacts, never in git"), so a
 * fixture citing it would be false provenance (Rule 8). Its harvest_len is
 * VMS_SCA_HDR_LEN (32) -- the same generic shared-header span every other
 * SCS class round-trips here -- so nothing about THIS class's own fields
 * goes unverified: tests/cluster/host/test_codec_applmsg.c is the item's
 * real R1 proof, built through the shipping MSCP end-message builders
 * instead of a fixture file.
 */
static void test_one_specimen_per_class(void)
{
	uint8_t cls;

	printf("-- one round-tripped specimen per registered class\n");
	for (cls = VMS_FCLS_UNKNOWN + 1; cls < VMS_FCLS__COUNT; cls++) {
		const struct vms_frame_class_info *ci =
			vms_frame_class_lookup(cls);
		char what[160];
		int i, n = 0;

		if (!ci)
			continue;
		if (cls == VMS_FCLS_SCS_APPLMSG) {
			printf("  skip  class '%s': no clean-room-manifest "
			      "capture exists yet -- see this loop's own "
			      "doc comment; proved in test_codec_applmsg.c\n",
			      ci->name);
			continue;
		}
		for (i = 0; i < g_n; i++) {
			struct vms_frame_info fi;

			(void)vms_frame_classify(g_fx[i].bytes,
						 g_fx[i].wire_len, &fi);
			if (fi.cls == cls)
				n++;
		}
		snprintf(what, sizeof(what),
			 "class '%s': %d round-tripped specimen(s)", ci->name, n);
		ct_check(n >= 1, what);
	}
}

static void test_build_bounds(void)
{
	struct vms_sca_hdr hdr;
	uint8_t small[VMS_SCA_HDR_LEN - 1];
	uint32_t written = 12345;

	printf("-- build refuses a buffer that cannot hold the header\n");
	memset(&hdr, 0, sizeof(hdr));
	ct_check(vms_sca_hdr_build(&hdr, small, sizeof(small), &written)
		 == VMS_CODEC_E_RANGE, "short output buffer -> E_RANGE");
	ct_check(vms_sca_hdr_build(&hdr, NULL, 100, &written)
		 == VMS_CODEC_E_INVAL, "NULL output buffer -> E_INVAL");
	ct_check(vms_sca_hdr_build(NULL, small, sizeof(small), &written)
		 == VMS_CODEC_E_INVAL, "NULL header -> E_INVAL");
}

/* Print, per class, how much of the frame the codec can rebuild today. */
static void report_coverage(void)
{
	int i;

	printf("-- harvest coverage (honest gap report, not an assertion)\n");
	for (i = 0; i < g_n; i++) {
		const struct vms_frame_class_info *ci;
		struct vms_frame_info fi;

		(void)vms_frame_classify(g_fx[i].bytes, g_fx[i].wire_len, &fi);
		ci = vms_frame_class_lookup(fi.cls);
		if (!ci || fi.cls == VMS_FCLS_UNKNOWN)
			continue;
		printf("       %-40s class %-14s harvest %3u / wire %4u"
		       "  cited %4u  origin %s\n",
		       g_fx[i].name, ci->name, ci->harvest_len,
		       g_fx[i].wire_len, vms_fixture_cited_bytes(&g_fx[i]),
		       vms_fixture_origin_name(g_fx[i].origin));
	}
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_roundtrip: specimen round trip (FC-P0.6)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}
	test_roundtrip_all();
	test_one_specimen_per_class();
	test_build_bounds();
	report_coverage();
	return ct_summary("test_codec_roundtrip");
}
