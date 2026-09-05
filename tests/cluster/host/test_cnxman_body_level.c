/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_body_level.c - FC-P3.15's own done-condition, rung R1: PROVE
 * no CNXMAN translation unit writes any byte below body offset 0 (abs 72).
 *
 * Design: docs/design-faithful-cluster-executive.md sec 3.2.4 ruling E1 --
 * "each layer owns exactly its own header"; abs [0,72) is the port's
 * (vms_pe.h) and SCS's (vms_scs.h) alone. A CNXMAN TU that reached below
 * body offset 0 would be exactly the category error the ruling forbids: "a
 * SYSAP that fills send_seq is the same category error as a daemon that
 * fills a lock id."
 *
 * TWO INDEPENDENT PROOFS, so a regression in either shape is caught:
 *
 *   1. STRUCTURAL. Every CNXMAN FSM's scratch buffer is now sized
 *      VMS_CM_BODY_LEN (132), not VMS_CM_FRAME_LEN (204): there is
 *      LITERALLY NO MEMORY in a `struct cnxman_barrier`/`struct cnxman_coord`
 *      that could represent abs [0,72) as a body-relative index. A write
 *      "below body offset 0" is not merely forbidden by convention here --
 *      it is a buffer overrun the codec's own bounds-checked vms_wire_buf_t
 *      (buf_span_ok(), vms_cluster_codec.c) refuses before it happens
 *      (VMS_CODEC_E_RANGE), never a silent out-of-bounds write.
 *
 *   2. SOURCE-LEVEL. The four CNXMAN translation units (vms_cnxman_barrier_
 *      fsm.c, vms_cnxman_coord_fsm.c, vms_cnxman_phase2.c, vms_cnxman_csb.c)
 *      contain NO call to a primitive that could write abs [0,72): no
 *      vms_wire_put_*() call (the raw wire-buffer primitive vms_cluster_
 *      codec.h exports), no vms_sca_hdr_build() call (the port/SCA header
 *      builder), and no reference to the demoted `struct vms_cm_link` (test-
 *      only since this item, tests/cluster/host/vms_frame_compose.h). This
 *      is a real, teeth-bearing scan -- not documentation: reintroducing any
 *      of the three into one of these files fails this test.
 *
 * The one exception this scan allows, and why it is safe: vms_cnxman_csb.c
 * ITSELF writes body[0:8] (via cnxman_envelope_stamp(), the ONE function the
 * design ruling names for that span) using its own two-byte LE helper
 * (csb_stamp_put_le16, offsets 0/2/4/6 -- see its header's "8. The SYSAP
 * envelope stamper"), not vms_wire_put_*. Sub-proof 3 below checks THOSE
 * four offsets directly, so the stamper's own span is verified rather than
 * merely trusted.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cluster_test.h"
#include "vms_cluster.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_coord_fsm.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * Proof 1: STRUCTURAL -- the scratch buffers are body-sized, not frame-sized
 * ========================================================================== */
static void test_scratch_buffers_are_body_sized(void)
{
	printf("-- proof 1: every CNXMAN FSM's scratch buffer is body-sized "
	       "(no memory below body offset 0 to write into) --\n");

	ct_check_eq_u32((uint32_t)sizeof(((struct cnxman_barrier *)0)->scratch),
			VMS_CM_BODY_LEN,
			"cnxman_barrier.scratch is VMS_CM_BODY_LEN (132), not "
			"VMS_CM_FRAME_LEN (204)");
	ct_check_eq_u32((uint32_t)sizeof(((struct cnxman_coord *)0)->scratch),
			VMS_CM_BODY_LEN,
			"cnxman_coord.scratch is VMS_CM_BODY_LEN (132), not "
			"VMS_CM_FRAME_LEN (204)");
	ct_check(VMS_CM_BODY_LEN < VMS_CM_FRAME_LEN,
		 "  (sanity: body really is narrower than a frame, so this "
		 "proof has teeth)");
}

/* ==========================================================================
 * Proof 2: SOURCE-LEVEL -- no forbidden primitive appears in the four
 * CNXMAN translation units
 * ========================================================================== */

/* One file's full text, read once. */
struct src_file {
	char     *text;
	uint32_t  len;
};

static int read_source(const char *path, struct src_file *out)
{
	FILE *f = fopen(path, "rb");
	long  n;

	out->text = NULL;
	out->len = 0;
	if (f == NULL)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	n = ftell(f);
	if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	out->text = (char *)malloc((size_t)n + 1);
	if (out->text == NULL) {
		fclose(f);
		return -1;
	}
	if (fread(out->text, 1, (size_t)n, f) != (size_t)n) {
		free(out->text);
		out->text = NULL;
		fclose(f);
		return -1;
	}
	out->text[n] = '\0';
	out->len = (uint32_t)n;
	fclose(f);
	return 0;
}

/* Does `text` contain `needle` immediately followed by optional whitespace
 * then '(' -- i.e. a real CALL, not a mention inside a comment/string (a
 * comment referencing "vms_wire_put_le16's own convention" has an apostrophe
 * after the identifier, never '(', so it does not match). */
static int contains_call(const char *text, const char *needle)
{
	size_t nlen = strlen(needle);
	const char *p = text;

	while ((p = strstr(p, needle)) != NULL) {
		const char *q = p + nlen;

		while (*q == ' ' || *q == '\t')
			q++;
		if (*q == '(')
			return 1;
		p += nlen;
	}
	return 0;
}

static int contains_token(const char *text, const char *needle)
{
	return strstr(text, needle) != NULL;
}

static void check_file_clean(const char *label, const char *path)
{
	struct src_file f;
	char what[256];

	if (read_source(path, &f) != 0) {
		snprintf(what, sizeof(what),
			 "%s: could not open %s -- the scan did not run "
			 "(treat as a failure, not a skip)", label, path);
		ct_check(0, what);
		return;
	}

	snprintf(what, sizeof(what),
		 "%s: no vms_wire_put_*() call (no raw wire-buffer write "
		 "outside a codec TU)", label);
	ct_check(!contains_call(f.text, "vms_wire_put_u8") &&
		 !contains_call(f.text, "vms_wire_put_le16") &&
		 !contains_call(f.text, "vms_wire_put_le32") &&
		 !contains_call(f.text, "vms_wire_put_be16") &&
		 !contains_call(f.text, "vms_wire_put_bytes") &&
		 !contains_call(f.text, "vms_wire_put_zero"), what);

	snprintf(what, sizeof(what),
		 "%s: no vms_sca_hdr_build() call (abs [0,32) is the port's)",
		 label);
	ct_check(!contains_call(f.text, "vms_sca_hdr_build"), what);

	snprintf(what, sizeof(what),
		 "%s: no reference to the demoted struct vms_cm_link "
		 "(test-only, tests/cluster/host/vms_frame_compose.h)", label);
	ct_check(!contains_token(f.text, "struct vms_cm_link"), what);

	free(f.text);
}

static void test_no_forbidden_primitives(void)
{
	printf("\n-- proof 2: source scan -- no CNXMAN TU calls a primitive "
	       "that could write abs [0,72) --\n");

	check_file_clean("vms_cnxman_barrier_fsm.c",
			 OVMX_KCORE_DIR "/vms_cnxman_barrier_fsm.c");
	check_file_clean("vms_cnxman_coord_fsm.c",
			 OVMX_KCORE_DIR "/vms_cnxman_coord_fsm.c");
	check_file_clean("vms_cnxman_phase2.c",
			 OVMX_KCORE_DIR "/vms_cnxman_phase2.c");
	check_file_clean("vms_cnxman_csb.c",
			 OVMX_KCORE_DIR "/vms_cnxman_csb.c");
	/* FC-P3.3's join FSM is the fifth CNXMAN TU and is held to the same
	 * rule. It does name `struct vms_mscp_link` -- FC-P3.4's own seam for
	 * an MSCP command's abs [0,72) -- but passes it ALL ZERO and transmits
	 * only frame[72:108], so SCS fills abs 56-71 from the real CDT and the
	 * port fills abs 0-55 from the real circuit. The scan below still
	 * proves it writes no wire buffer of its own. */
	check_file_clean("vms_cnxman_join_fsm.c",
			 OVMX_KCORE_DIR "/vms_cnxman_join_fsm.c");
}

/* ==========================================================================
 * Proof 3: the ONE named exception (cnxman_envelope_stamp) really does stay
 * inside body[0:8] -- its own four offsets, checked directly.
 * ========================================================================== */
static void test_stamper_offsets_are_inside_body_0_8(void)
{
	struct src_file f;
	const char *needles[4] = {
		"CSB_STAMP_OFF_SEND_MSG", "CSB_STAMP_OFF_ACK_MSG",
		"CSB_STAMP_OFF_TXN", "CSB_STAMP_OFF_TOKEN"
	};
	const uint32_t expect[4] = { 0u, 2u, 4u, 6u };
	uint32_t i;

	printf("\n-- proof 3: the stamper's own four offsets are inside "
	       "body[0:8], not beyond it --\n");

	if (read_source(OVMX_KCORE_DIR "/vms_cnxman_csb.c", &f) != 0) {
		ct_check(0, "could not open vms_cnxman_csb.c");
		return;
	}

	for (i = 0; i < 4u; i++) {
		const char *p = strstr(f.text, needles[i]);
		char what[160];
		unsigned long got;

		snprintf(what, sizeof(what), "%s is #defined in this file",
			 needles[i]);
		ct_check(p != NULL, what);
		if (p == NULL)
			continue;
		/* "#define CSB_STAMP_OFF_X <digits>u" -- walk to the digits. */
		p += strlen(needles[i]);
		while (*p == ' ' || *p == '\t')
			p++;
		got = strtoul(p, NULL, 10);
		snprintf(what, sizeof(what),
			 "%s == %u (strictly inside body[0:8])",
			 needles[i], (unsigned)expect[i]);
		ct_check((uint32_t)got == expect[i] && got < 8u, what);
	}
	free(f.text);
}

int main(void)
{
	printf("test_cnxman_body_level: FC-P3.15's own done-condition -- no "
	       "CNXMAN TU writes below body offset 0\n");

	test_scratch_buffers_are_body_sized();
	test_no_forbidden_primitives();
	test_stamper_offsets_are_inside_body_0_8();

	return ct_summary("test_cnxman_body_level");
}
