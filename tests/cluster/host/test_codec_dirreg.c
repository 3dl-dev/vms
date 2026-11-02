/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_codec_dirreg.c - the cat-0x02 op-0x0d record, decoded and rebuilt
 * against EVERY real record in four captures (rd vms-20c, FC-P5.5).
 *
 * WHAT IT PINS. The field map in vms_cluster_codec_dlm.h's "THE op-0x0d
 * RECORD'S FIELD MAP" block: a real sender writes the envelope, 02 0d, the
 * 0x0001 + rebuild-sequence pair, the resource identity at body[44:48+L] and
 * the directory hash of that identity at body[128:132], and nothing else.
 * For every record this test
 *
 *   1. parses it with vms_dlm_dir_reg_parse_body (the shipping parser);
 *   2. rebuilds it with vms_dlm_dir_reg_build from what was parsed, and
 *      requires the rebuilt body to equal the real one BYTE FOR BYTE over the
 *      cited span -- body[8:10], body[44:48+L], body[128:132] -- and to be
 *      zero everywhere else except the connection manager's body[0:8] and
 *      body[12:16], which the builder leaves to it;
 *   3. requires body[128:132] to equal the PRODUCT's vms_dlm_name_hash() of
 *      the decoded identity: a decode that mis-shifted the name by one byte
 *      (the mistake that bugchecked two real VAXes with LOCKMGRERR,
 *      ovmx-760-lockmgrerr-20260730.pcap) cannot pass this, because the hash
 *      would be of a different identity.
 *
 * THE CAPTURES VARY WHAT CAN VARY. rejoin-rebuild.pcap (vms-20c): two real
 * VAXes, the survivor registering with a rejoiner. run-20261009-vaxlab3.pcap
 * (vms-c6e): three nodes, two real VAX senders, a VAX and OVMX receiving.
 * cn3.pcap (vms-1ac): a real VAX registering with two OVMX joiners. R2.pcap
 * (vms-ec2): a third pair of real nodes. Names of 7..30 bytes, access modes
 * 0/1/3, a non-zero UIC group. WHAT THEY CANNOT VARY: no record names a
 * sub-resource (the message carries no parent span to vary), and none is a
 * lock HOLDER describing a lock -- the message has no such fields, which is
 * the point of rd vms-20c's re-scope.
 *
 * NEGATIVE CONTROLS: a one-byte shift of the name, an answer (cat 0x82), an
 * op-0x01, a wrong body[12:14], a short body and impossible lengths are all
 * refused or caught; and the RULE C gate still refuses an op-0x0d body, so
 * nothing here makes OVMX address one at a real VAX.
 *
 * Clean-room: our own lab captures only.
 */
#include "cluster_test.h"
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN */
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_hash.h"

#include <stdlib.h>
#include <string.h>

#define FRAME_LEN 204u
#define BODY_OFF  72u
#define BODY_LEN  132u

struct cap {
	const char *path;      /* under tests/lab/captures */
	uint32_t want;         /* op-0x0d requests in it    */
};

static const struct cap caps[] = {
	{ "vms-20c-rebuild-1789489597/rejoin-rebuild.pcap",      307u },
	{ "vms-c6e-heldout-20261008/run-20261009-vaxlab3.pcap",  480u },
	{ "vms-1ac-cn3-achieved-20260925/cn3.pcap",              481u },
	{ "vms-ec2-vc-reorder-20261005/R2.pcap",                 324u },
};

static uint8_t *slurp(const char *rel, size_t *n)
{
	char path[1024];
	FILE *f;
	uint8_t *buf;
	long sz;

	snprintf(path, sizeof(path), "%s/%s", OVMX_CAPTURE_DIR, rel);
	f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (uint8_t *)malloc((size_t)sz);
	if (buf != NULL && fread(buf, 1u, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return buf;
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* An SCS-MSG frame carrying a cat-0x02 op-0x0d with body[12:14] = 0x0001. */
static int is_op0d_request(const uint8_t *fr, uint32_t len)
{
	const uint8_t *b = fr + BODY_OFF;

	return len == FRAME_LEN && fr[12] == 0x60 && fr[13] == 0x07 &&
	       fr[31] == 0x13 && fr[55] == 0x02 && b[8] == 0x02 &&
	       b[9] == 0x0d && b[12] == 0x01 && b[13] == 0x00;
}

struct tally {
	uint32_t records, parsed, span_identical, rest_zero, hash_is_product,
		 seq_matches, shift_caught;
	uint32_t len_min, len_max, mode_mask, group_nonzero;
};

static void one_record(const uint8_t *real, struct tally *t)
{
	struct vms_dlm_dir_reg r;
	uint8_t built[FRAME_LEN];
	uint32_t wr = 0u, h = 0u, o;
	const uint8_t *nb;
	int ok_span, ok_rest;

	t->records++;
	if (vms_dlm_dir_reg_parse_body(real, BODY_LEN, &r) != VMS_CODEC_OK)
		return;
	t->parsed++;
	if (r.id.name_len < t->len_min)
		t->len_min = r.id.name_len;
	if (r.id.name_len > t->len_max)
		t->len_max = r.id.name_len;
	t->mode_mask |= 1u << (r.id.mode & 7u);
	t->group_nonzero += r.id.group != 0u;
	t->seq_matches += r.dlm_seq == (uint16_t)(real[14] | (real[15] << 8));

	memset(built, 0xa5, sizeof(built));      /* the builder must write it all */
	if (vms_dlm_dir_reg_build(&r.id, built, sizeof(built), &wr) != VMS_CODEC_OK ||
	    wr != FRAME_LEN)
		return;
	nb = built + BODY_OFF;
	ok_span = memcmp(nb + 8, real + 8, 2u) == 0 &&
		  memcmp(nb + 44, real + 44, 4u + r.id.name_len) == 0 &&
		  memcmp(nb + 128, real + 128, 4u) == 0;
	ok_rest = 1;
	for (o = 0u; o < BODY_LEN; o++) {
		int cited = (o >= 8u && o < 10u) ||
			    (o >= 44u && o < 48u + r.id.name_len) || o >= 128u;
		if (!cited && nb[o] != 0u)
			ok_rest = 0;
	}
	t->span_identical += ok_span;
	t->rest_zero += ok_rest;

	if (vms_dlm_name_hash(r.id.group, r.id.mode, r.id.name, r.id.name_len,
			      &h) == VMS_DLM_HASH_OK && h == le32(real + 128))
		t->hash_is_product++;

	/* NEGATIVE CONTROL: the LOCKMGRERR mis-shift. Read the name one byte
	 * late and the identity no longer hashes to the wire value. */
	if (r.id.name_len >= 2u &&
	    vms_dlm_name_hash(r.id.group, r.id.mode, real + 49, r.id.name_len,
			      &h) == VMS_DLM_HASH_OK && h != le32(real + 128))
		t->shift_caught++;
}

static void test_every_real_record(void)
{
	struct tally all;
	size_t ci;

	memset(&all, 0, sizeof(all));
	all.len_min = 255u;
	printf("-- every real op-0x0d record: decode, rebuild, compare\n");
	for (ci = 0u; ci < sizeof(caps) / sizeof(caps[0]); ci++) {
		struct tally t;
		size_t n = 0u, off;
		uint8_t *d = slurp(caps[ci].path, &n);
		char what[256];

		memset(&t, 0, sizeof(t));
		t.len_min = 255u;
		snprintf(what, sizeof(what), "%s readable", caps[ci].path);
		ct_check(d != NULL && n > 24u, what);
		if (d == NULL)
			continue;
		for (off = 24u; off + 16u <= n;) {
			uint32_t caplen = le32(d + off + 8u);

			off += 16u;
			if (off + caplen > n)
				break;
			if (is_op0d_request(d + off, caplen))
				one_record(d + off + BODY_OFF, &t);
			off += caplen;
		}
		free(d);
		snprintf(what, sizeof(what), "%s: op-0x0d requests", caps[ci].path);
		ct_check_eq_u32(t.records, caps[ci].want, what);
		ct_check_eq_u32(t.parsed, t.records, "    every one parses");
		ct_check_eq_u32(t.span_identical, t.records,
				"    rebuilt body[8:10] + [44:48+L] + [128:132] "
				"byte-identical to the real record");
		ct_check_eq_u32(t.rest_zero, t.records,
				"    and zero everywhere the sender did not write");
		ct_check_eq_u32(t.hash_is_product, t.records,
				"    body[128:132] == vms_dlm_name_hash(identity)");
		ct_check_eq_u32(t.seq_matches, t.records,
				"    dlm_seq == body[14:16]");
		ct_check_eq_u32(t.shift_caught, t.records,
				"    a one-byte name shift is caught by the hash");
		all.records += t.records;
		all.parsed += t.parsed;
		if (t.len_min < all.len_min)
			all.len_min = t.len_min;
		if (t.len_max > all.len_max)
			all.len_max = t.len_max;
		all.mode_mask |= t.mode_mask;
		all.group_nonzero += t.group_nonzero;
	}
	ct_check_eq_u32(all.records, 1592u, "four captures: 1592 records in all");
	/* The corpus varies these, so a decoder that hard-coded any of them
	 * could not pass the counts above. */
	ct_check_eq_u32(all.len_min, 7u, "shortest name 7");
	ct_check_eq_u32(all.len_max, 30u, "longest name 30");
	ct_check_eq_u32(all.mode_mask, 0x0bu, "access modes 0, 1 and 3 all occur");
	ct_check(all.group_nonzero > 0u, "a non-zero UIC group occurs");
}

static void test_refusals(void)
{
	uint8_t b[BODY_LEN];
	uint8_t fr[FRAME_LEN];
	struct vms_dlm_dir_reg r;
	struct vms_dlm_res_ident id;
	uint32_t wr;

	printf("-- refusals and the RULE C gate\n");
	memset(&id, 0, sizeof(id));
	memcpy(id.name, "SYS$SYS_ID\x01\x04\0\0\0\0", 16u);
	id.name_len = 16u;
	id.mode = 1u;
	id.hash = 0x324324dbu;
	ct_check(vms_dlm_dir_reg_build(&id, fr, sizeof(fr), &wr) == VMS_CODEC_OK,
		 "builds SYS$SYS_ID (rejoin frame 665's identity)");
	memcpy(b, fr + BODY_OFF, BODY_LEN);
	b[12] = 0x01;                 /* what the connection manager stamps */
	b[14] = 0x04;
	ct_check(vms_dlm_dir_reg_parse_body(b, BODY_LEN, &r) == VMS_CODEC_OK &&
		 r.id.name_len == 16u && r.id.mode == 1u && r.dlm_seq == 4u &&
		 r.id.hash == 0x324324dbu, "and parses back to the same identity");
	ct_check(!vms_dlm_shape_fit_for_any_member(b, BODY_LEN),
		 "RULE C still refuses to address an op-0x0d at a real VAX");

	ct_check(vms_dlm_dir_reg_parse_body(b, 47u, &r) != VMS_CODEC_OK,
		 "a body cut before the name length is refused");
	ct_check(vms_dlm_dir_reg_parse_body(b, 60u, &r) != VMS_CODEC_OK,
		 "a body cut inside the name is refused");
	b[8] = 0x82;
	ct_check(vms_dlm_dir_reg_parse_body(b, BODY_LEN, &r) == VMS_CODEC_E_CLASS,
		 "the 82/0d ANSWER is not a record");
	b[8] = 0x02;
	b[9] = 0x01;
	ct_check(vms_dlm_dir_reg_parse_body(b, BODY_LEN, &r) == VMS_CODEC_E_CLASS,
		 "an op-0x01 is not a record");
	b[9] = 0x0d;
	b[12] = 0x02;
	ct_check(vms_dlm_dir_reg_parse_body(b, BODY_LEN, &r) == VMS_CODEC_E_CLASS,
		 "body[12:14] != 0x0001 is refused");
	b[12] = 0x01;
	b[47] = 0u;
	ct_check(vms_dlm_dir_reg_parse_body(b, BODY_LEN, &r) != VMS_CODEC_OK,
		 "a zero name length is refused");
	b[47] = 32u;
	ct_check(vms_dlm_dir_reg_parse_body(b, BODY_LEN, &r) == VMS_CODEC_E_RANGE,
		 "a name length of 32 is refused");

	id.name_len = 0u;
	ct_check(vms_dlm_dir_reg_build(&id, fr, sizeof(fr), &wr) == VMS_CODEC_E_INVAL,
		 "no builder output for an identity with no name");
	id.name_len = 32u;
	ct_check(vms_dlm_dir_reg_build(&id, fr, sizeof(fr), &wr) == VMS_CODEC_E_INVAL,
		 "nor for a 32-byte name");
	id.name_len = 16u;
	ct_check(vms_dlm_dir_reg_build(&id, fr, 150u, &wr) != VMS_CODEC_OK,
		 "nor into a buffer too short for the body");
	ct_check(vms_dlm_dir_reg_build((const struct vms_dlm_res_ident *)0, fr,
				       sizeof(fr), &wr) == VMS_CODEC_E_INVAL,
		 "nor from no identity");
}

int main(void)
{
	printf("test_codec_dirreg: the op-0x0d record against every real one "
	       "(rd vms-20c)\n");
	test_every_real_record();
	test_refusals();
	return ct_summary("test_codec_dirreg");
}
