// SPDX-License-Identifier: GPL-2.0
/*
 * score_remaster.c - score a FRESH remaster capture with the COMMITTED codec
 * (rd vms-380; the arm this exists for is rd vms-380 RA8 and rd vms-6ad1 RA7).
 *
 * usage: tools/cluster/remaster_census.py --pairs <pcap> | score_remaster
 *
 * WHY A PROGRAM AND NOT A SCRIPT. A held-out proof is only worth the thing
 * doing the predicting. If a Python re-implementation of the rules scores the
 * capture, what passed is the script. This reads the request/answer pairs the
 * census tool extracts and, for each one, calls the SHIPPING builder --
 * vms_dlm_rm_ack_build or vms_dlm_rm_lock_answer_build, the same code the
 * executive will send with -- then diffs its output against what the real VAX
 * really answered, byte by byte over body[4:132]. body[0:4] is the connection
 * manager's send/ack pair and no codec owns it.
 *
 * THE TWO CELLS THE CODEC DOES NOT NAME (body[30:32] and body[32:34] of an
 * op-0x12 answer, rd vms-380 rule 2) are reported SEPARATELY rather than
 * excused silently: the summary prints how many answers agreed with the real
 * VAX there and how many did not, so the number is in front of whoever reads
 * the result.
 *
 * Exit 0 only when every pair outside those four bytes is byte-exact.
 */
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN */
#include "vms_cluster_codec_dlm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 1024

struct tally {
	unsigned pairs;
	unsigned exact;
	unsigned mismatched;
	unsigned unnamed_agreed;
	unsigned unnamed_differed;
	unsigned subblk_zero;      /* real sub answers whose body[36:56] is 0 */
	unsigned subblk_residue;   /* ... and those carrying a busy master's */
	unsigned refused;
};

static int hex_nib(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Decode exactly `want` bytes of hex; return 0 on success. */
static int unhex(const char *s, uint8_t *out, uint32_t want)
{
	uint32_t i;

	for (i = 0; i < want; i++) {
		int hi = hex_nib(s[2 * i]), lo = hex_nib(s[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

/* Is this offset one of the two cells rd vms-380 leaves unnamed? */
static int is_unnamed_0x12_cell(uint32_t off)
{
	return off >= 30u && off < 34u;
}

/* An op-0x12 ROOT record carries no parent (body[36:44] all zero). */
static int rec_is_root(const uint8_t *req)
{
	int i;

	for (i = 36; i < 44; i++) {
		if (req[i] != 0u)
			return 0;
	}
	return 1;
}

static uint32_t le32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/*
 * Compare one built body against the real answer and report. `allow_unnamed`
 * excuses -- and counts -- the two cells the codec does not name.
 */
static void judge(const char *what, const uint8_t *built, const uint8_t *real,
		  int allow_unnamed, struct tally *t)
{
	unsigned bad = 0, i;

	for (i = 4u; i < VMS_CM_BODY_LEN; i++) {
		if (built[i] == real[i])
			continue;
		if (allow_unnamed && is_unnamed_0x12_cell(i)) {
			t->unnamed_differed++;
			continue;
		}
		printf("    MISMATCH %s body[%u]: built %02x, real VAX %02x\n",
		       what, i, built[i], real[i]);
		bad++;
	}
	if (allow_unnamed) {
		for (i = 30u; i < 34u; i++) {
			if (built[i] == real[i])
				t->unnamed_agreed++;
		}
	}
	if (bad == 0u) {
		t->exact++;
		printf("    ok %s: byte-exact over body[4:132]%s\n", what,
		       allow_unnamed ? " outside the two unnamed cells" : "");
	} else {
		t->mismatched++;
	}
}

/* RULE 1: the 82/15 answer to an op-0x0e / 0x0f / 0x10 / 0x14. */
static void score_ack(const uint8_t *req, const uint8_t *ans, struct tally *t)
{
	uint8_t frame[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	int new_master;
	char what[64];

	/*
	 * `new_master` is a ROLE, and the only honest source for it here is the
	 * real answer itself: an op-0x0f answered with body[28:32] cleared came
	 * from the member that held the open remaster. Stating it any other way
	 * would be scoring the builder against a guess about the lab.
	 */
	new_master = (req[9] == VMS_DLM_WIREOP_RM_0F &&
		      ans[28] == 0u && ans[29] == 0u &&
		      ans[30] == 0u && ans[31] == 0u) ? 1 : 0;
	sprintf(what, "02/%02x -> 82/15 (new_master=%d)", req[9], new_master);

	memset(frame, 0xa5, sizeof(frame));
	if (vms_dlm_rm_ack_build(req, VMS_CM_BODY_LEN, new_master, frame,
				 (uint32_t)sizeof(frame), NULL) != VMS_CODEC_OK) {
		printf("    REFUSED %s: the builder would not build it\n", what);
		t->refused++;
		return;
	}
	judge(what, frame + VMS_OFF_SYSAP_BODY, ans, 0, t);
}

/* RULE 2: the 82/01 answer to an op-0x12 lock record. */
static void score_lock_answer(const uint8_t *req, const uint8_t *ans,
			      struct tally *t)
{
	struct vms_dlm_rm_lock_answer a;
	uint8_t frame[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	char what[64];
	int i, root = rec_is_root(req);

	/*
	 * THE HANDLE is read out of the real answer, because this program has
	 * no lock database -- which is exactly why the handle is NOT what this
	 * run proves.
	 *
	 * THE VALUE BLOCK IS NOT. It is read out of the RECORD, at body[88:108]
	 * -- the one place the real path gets it too: the record feeds
	 * vms_lock_dlm_adopt_lock, the engine writes the RSB, and the answer is
	 * the read-back (rd vms-ed5). So this run DOES prove the block: if the
	 * 420/420 rule is wrong anywhere, the diff below says so.
	 */
	memset(&a, 0, sizeof(a));
	a.master_lkid = le32(ans + 20);
	for (i = 0; i < (int)VMS_DLM_VALBLK_WIRE_LEN; i++) {
		a.valblk[i] = req[88 + i];
		if (a.valblk[i] != 0u)
			a.valblk_present = 1u;
	}
	a.valblk_seq = le32(req + 104);
	if (a.valblk_seq != 0u)
		a.valblk_present = 1u;
	if (!a.valblk_present)
		a.valblk_seq = 0u;
	sprintf(what, "02/12 -> 82/01 (%s)", root ? "ROOT" : "sub");

	memset(frame, 0xa5, sizeof(frame));
	if (vms_dlm_rm_lock_answer_build(req, VMS_CM_BODY_LEN, &a, frame,
					 (uint32_t)sizeof(frame),
					 NULL) != VMS_CODEC_OK) {
		printf("    REFUSED %s: the builder would not build it\n", what);
		t->refused++;
		return;
	}
	if (!root) {
		if (a.valblk_present)
			t->subblk_residue++;
		else
			t->subblk_zero++;
	}
	judge(what, frame + VMS_OFF_SYSAP_BODY, ans, 1, t);
}

/* P5: the root/sub split of a real 82/01 answer, independent of our builder. */
static void note_root_sub_split(const uint8_t *req, const uint8_t *ans)
{
	int i, blk = 0;

	if (rec_is_root(req)) {
		for (i = 36; i < 52; i++) {
			if (ans[i] != 0u)
				blk++;
		}
		printf("    ROOT answer: body[30:32] = %02x %02x, value block "
		       "%s (%d non-zero bytes), sequence %u\n",
		       ans[30], ans[31], blk ? "PRESENT" : "ABSENT", blk,
		       le32(ans + 52));
	} else {
		for (i = 36; i < 56; i++) {
			if (ans[i] != 0u)
				blk++;
		}
		printf("    sub  answer: body[30:32] = %02x %02x, "
		       "body[36:56] %s (%d non-zero bytes)\n",
		       ans[30], ans[31], blk ? "NOT ZERO" : "all zero", blk);
	}
}

static void score_line(const char *line, struct tally *t)
{
	uint8_t req[VMS_CM_BODY_LEN], ans[VMS_CM_BODY_LEN];
	unsigned step = 0, aop = 0;
	const char *p;

	if (sscanf(line, "%x %x", &step, &aop) != 2)
		return;
	p = strchr(line, ' ');
	p = p ? strchr(p + 1, ' ') : NULL;
	if (p == NULL)
		return;
	p++;
	if (unhex(p, req, VMS_CM_BODY_LEN) != 0)
		return;
	p = strchr(p, ' ');
	if (p == NULL)
		return;
	if (unhex(p + 1, ans, VMS_CM_BODY_LEN) != 0)
		return;

	t->pairs++;
	if (aop == 0x15u) {
		score_ack(req, ans, t);
	} else if (aop == 0x01u) {
		note_root_sub_split(req, ans);
		score_lock_answer(req, ans, t);
	}
}

int main(void)
{
	char line[LINE_MAX_LEN];
	struct tally t;

	memset(&t, 0, sizeof(t));
	printf("score_remaster: the COMMITTED codec against a real VAX's own "
	       "answers\n");
	while (fgets(line, sizeof(line), stdin) != NULL)
		score_line(line, &t);

	printf("\nscore_remaster: %u pair(s): %u byte-exact, %u mismatched, "
	       "%u refused\n", t.pairs, t.exact, t.mismatched, t.refused);
	printf("  the two cells rd vms-380 does not name (op-0x12 answer "
	       "body[30:34]): %u byte(s) agreed, %u differed\n",
	       t.unnamed_agreed, t.unnamed_differed);
	printf("  SUB records: %u whose body[88:108] is twenty zeros (so their "
	       "answer carries none), %u carrying a real value block\n",
	       t.subblk_zero, t.subblk_residue);
	if (t.pairs == 0u) {
		printf("  FAIL: no pairs on stdin -- nothing was scored\n");
		return 1;
	}
	if (t.mismatched != 0u || t.refused != 0u) {
		printf("  FAIL: %u mismatched, %u refused\n", t.mismatched,
		       t.refused);
		return 1;
	}
	printf("  PASS: every pair is byte-exact outside the unnamed cells\n");
	return 0;
}
