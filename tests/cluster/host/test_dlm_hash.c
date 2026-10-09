// SPDX-License-Identifier: GPL-2.0
/*
 * test_dlm_hash.c - the derived DLM resource-name hash reproduces real VMS
 * wire values, rung R1 (rd vms-66fe, held-out leg of rd vms-c6e).
 *
 * This is the whole proof of the function, and it is deliberately not a table
 * of hand-written expectations: every row it checks is a value a real OpenVMS
 * VAX put on a cluster wire, extracted by tools/cluster/dlm_hash/extract.py
 * and committed with the pcap + frame it came from
 * (tests/cluster/fixtures/README-dlm-hash-corpus.md).
 *
 * THE TWO SPLITS ARE CHECKED SEPARATELY AND THAT MATTERS. The derivation split
 * is the evidence the function was found FROM, so reproducing it proves only
 * that the fit is exact. The held-out split was chosen by a seeded rule and
 * frozen in the corpus commit BEFORE any derivation work began, and was not
 * looked at until the function was final -- so reproducing it is the only
 * result here that distinguishes "determined the function" from "memorised a
 * table". If a future change makes the held-out split red, the function is
 * wrong; do not re-split the corpus (check_corpus.py gates that too).
 *
 * The THIRD set is a FORWARD PREDICTION. dlm_hash_predicted_m3soledir.tsv was
 * extracted from a lab capture that did not exist when the function was frozen
 * -- the function was committed at 2026-10-08T20:41:38Z and that pcap was
 * written at 20:57:15Z -- from a different rig, a different cluster group, and
 * two frame shapes rather than one (op-0x01 lookups AND op-0x0d directory
 * registrations). 84 of its 533 keys appear in no other fixture here.
 */
#include "cluster_test.h"
#include "vms_dlm_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DLM_HASH_TSV_LINE_MAX 512

struct hash_row {
	uint8_t  name[VMS_DLM_HASH_NAME_MAX];
	uint32_t name_len;
	uint8_t  mode;
	uint16_t group;
	uint32_t value;
};

/* ---------------------------------------------------------------- *
 * The fixture is a TSV, so the parsing is three small readers.
 * ---------------------------------------------------------------- */

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Returns the decoded byte count, or -1. */
static int decode_hex(const char *s, uint8_t *out, size_t cap)
{
	size_t n = 0;

	while (s[0] != '\0' && s[0] != '\t') {
		int hi, lo;

		if (s[1] == '\0' || n == cap)
			return -1;
		hi = hex_nibble(s[0]);
		lo = hex_nibble(s[1]);
		if (hi < 0 || lo < 0)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
		s += 2;
	}
	return (int)n;
}

/* The `n`-th tab-separated field of `line`, or NULL. */
static const char *field(const char *line, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		line = strchr(line, '\t');
		if (line == NULL)
			return NULL;
		line++;
	}
	return line;
}

static int parse_row(const char *line, struct hash_row *row)
{
	const char *f_len = field(line, 1);
	const char *f_mode = field(line, 2);
	const char *f_group = field(line, 3);
	const char *f_value = field(line, 4);
	int n;

	if (f_len == NULL || f_mode == NULL || f_group == NULL || f_value == NULL)
		return -1;
	n = decode_hex(line, row->name, sizeof(row->name));
	if (n <= 0 || (unsigned long)n != strtoul(f_len, NULL, 10))
		return -1;
	row->name_len = (uint32_t)n;
	row->mode = (uint8_t)strtoul(f_mode, NULL, 10);
	row->group = (uint16_t)strtoul(f_group, NULL, 10);
	row->value = (uint32_t)strtoul(f_value, NULL, 16);
	return 0;
}

/* ---------------------------------------------------------------- *
 * The check itself
 * ---------------------------------------------------------------- */

struct split_result {
	unsigned long rows;
	unsigned long matched;
	unsigned long unparsed;
};

static void check_row(const struct hash_row *row, struct split_result *r)
{
	uint32_t got = 0;
	enum vms_dlm_hash_status st;

	st = vms_dlm_name_hash(row->group, row->mode, row->name,
			       row->name_len, &got);
	r->rows++;
	if (st == VMS_DLM_HASH_OK && got == row->value) {
		r->matched++;
		return;
	}
	printf("  MISMATCH len=%u mode=%u group=%u wire=%08x got=%08x st=%d\n",
	       row->name_len, row->mode, row->group, row->value, got, (int)st);
}

static struct split_result check_split(const char *path)
{
	struct split_result r = { 0, 0, 0 };
	char line[DLM_HASH_TSV_LINE_MAX];
	FILE *fh = fopen(path, "r");

	ct_check(fh != NULL, "fixture opens");
	/* header */
	ct_check(fgets(line, sizeof(line), fh) != NULL, "fixture has a header");
	while (fgets(line, sizeof(line), fh) != NULL) {
		struct hash_row row;

		/* Blank, a comment, or a repeated header: the driven-run
		 * prediction file leads with a four-line provenance comment, so
		 * the first line is not necessarily the column header. */
		if (line[0] == '\n' || line[0] == '#' ||
		    strncmp(line, "name_hex", 8) == 0)
			continue;
		if (parse_row(line, &row) != 0) {
			r.unparsed++;
			continue;
		}
		check_row(&row, &r);
	}
	fclose(fh);
	return r;
}

static void report(const char *what, const char *path, unsigned long least)
{
	struct split_result r = check_split(path);

	printf("%s: %lu rows, %lu reproduced, %lu unparsed\n",
	       what, r.rows, r.matched, r.unparsed);
	ct_check(r.unparsed == 0, "every fixture row parses");
	ct_check(r.rows >= least, "the split still carries its rows");
	ct_check(r.matched == r.rows, "every real-VAX wire value is reproduced");
}

/*
 * The DRIVEN run's prediction file (rd vms-c6e leg 2). This file holds no
 * capture: it is what THIS tree says a real VAX will put on the wire for 81
 * (name, mode, group) triples a lab run is about to drive, committed before
 * the VAX is asked to lock anything. It is generated on the lab side by
 * tools/cluster/dlm_hash/gen_heldout_run.py (Python, because the generator
 * also emits the MACRO-32 driver), so THIS is the check that the number the
 * lab will be scored against is the PRODUCT's own function and not a Python
 * lookalike that drifted from it. Same parser, deliberately different
 * wording: nothing here is evidence about VMS yet.
 */
static void report_predictions(const char *what, const char *path,
			       unsigned long least)
{
	struct split_result r = check_split(path);

	printf("%s: %lu rows, %lu agree with vms_dlm_name_hash(), %lu unparsed\n",
	       what, r.rows, r.matched, r.unparsed);
	ct_check(r.unparsed == 0, "every prediction row parses");
	ct_check(r.rows >= least, "the prediction set still carries its rows");
	ct_check(r.matched == r.rows,
		  "every committed prediction is what the C function computes");
}

/* ---------------------------------------------------------------- *
 * Refusals: a value nobody can compute is never invented (INV-6).
 * ---------------------------------------------------------------- */

static void test_refusals(void)
{
	static const uint8_t name[4] = { 'D', 'L', 'M', 'T' };
	uint32_t out = 0xdeadbeefu;

	ct_check(vms_dlm_name_hash(0, 0, NULL, 4, &out) == VMS_DLM_HASH_E_INVAL,
		  "a null name is refused");
	ct_check(vms_dlm_name_hash(0, 0, name, 4, NULL) == VMS_DLM_HASH_E_INVAL,
		  "a null out is refused");
	ct_check(vms_dlm_name_hash(0, 0, name, 0, &out) == VMS_DLM_HASH_E_RANGE,
		  "a zero-length name is refused");
	ct_check(vms_dlm_name_hash(0, 0, name, VMS_DLM_HASH_NAME_MAX + 1u,
				    &out) == VMS_DLM_HASH_E_RANGE,
		  "a name longer than the wire field is refused");
	ct_check(out == 0xdeadbeefu, "a refusal writes nothing (INV-6)");
}

/*
 * One GROUNDED specimen spelled out, so a reader can see a real pair without
 * opening the TSV: `DLMTA`, user mode, group 0, from VAX2/VAX3/VAX1 alike in
 * dlmlab L1 (rd vms-4fb finding 1 quotes the same bytes, e3 6f 33 00 LE).
 */
static void test_named_specimen(void)
{
	static const uint8_t dlmta[5] = { 'D', 'L', 'M', 'T', 'A' };
	uint32_t out = 0;

	ct_check(vms_dlm_name_hash(0, 3, dlmta, 5, &out) == VMS_DLM_HASH_OK,
		  "DLMTA hashes");
	ct_check(out == 0x00336fe3u, "DLMTA reproduces the captured value");
}

/* ================================================================ *
 * §coverage -- the proven envelope is DERIVED FROM THE EVIDENCE, not asserted
 * (rd vms-b5b0).
 *
 * vms_dlm_hash.h carries three constants that decide which identities
 * $ENQ may route on and which it refuses (SS$_UNSUPPORTED). They are claims
 * about what a real VAX has been WATCHED hashing, so they are computed here
 * from the two proof artifacts and compared, rather than read and trusted:
 *
 *   - the corpus TSVs (every row is a captured wire value), and
 *   - the DRIVEN run, counting ONLY the pre-registered triples the VAX
 *     actually put on the wire: predicted.tsv MINUS the ABSENT lines of
 *     run-20261009-score.txt. A prediction the run never exercised proves
 *     nothing and must not widen the envelope.
 *
 * Under the two reading rules the header states: EXACT-VALUE membership for
 * the two enumerations (access mode, name length) and the BIT SPAN for the
 * magnitude (UIC group).
 * ================================================================ */

struct cover {
	uint32_t len_mask;     /* bit n = a name of length n was observed */
	uint32_t mode_mask;    /* bit m = access mode m was observed      */
	uint16_t group_or;     /* OR of every observed group word         */
	unsigned long rows;
};

static void cover_add(struct cover *c, const struct hash_row *row)
{
	c->len_mask |= (uint32_t)1u << row->name_len;
	c->mode_mask |= (uint32_t)1u << (row->mode & 31u);
	c->group_or |= row->group;
	c->rows++;
}

/* Accumulate every row of a captured-value TSV. */
static void cover_add_tsv(struct cover *c, const char *path)
{
	char line[DLM_HASH_TSV_LINE_MAX];
	FILE *fh = fopen(path, "r");

	ct_check(fh != NULL, "coverage fixture opens");
	if (fh == NULL)
		return;
	while (fgets(line, sizeof(line), fh) != NULL) {
		struct hash_row row;

		if (line[0] == '\n' || line[0] == '#' ||
		    strncmp(line, "name_hex", 8) == 0)
			continue;
		if (parse_row(line, &row) == 0)
			cover_add(c, &row);
	}
	fclose(fh);
}

#define C6E_ABSENT_MAX 128

struct absent_set {
	char   key[C6E_ABSENT_MAX][DLM_HASH_TSV_LINE_MAX];
	size_t n;
};

/* "<name_hex> <mode> <group>" -- the identity of one scored triple. */
static void triple_key(char *out, size_t cap, const char *name_hex,
		       unsigned mode, unsigned group)
{
	snprintf(out, cap, "%s %u %u", name_hex, mode, group);
}

static void absent_load(struct absent_set *a, const char *path)
{
	char line[DLM_HASH_TSV_LINE_MAX];
	FILE *fh = fopen(path, "r");

	a->n = 0;
	ct_check(fh != NULL, "the driven run's score file opens");
	if (fh == NULL)
		return;
	while (fgets(line, sizeof(line), fh) != NULL) {
		char name_hex[DLM_HASH_TSV_LINE_MAX];
		unsigned mode = 0, group = 0;

		if (sscanf(line, " ABSENT %s mode=%u group=%u", name_hex,
			   &mode, &group) != 3)
			continue;
		ct_check(a->n < C6E_ABSENT_MAX, "the ABSENT list fits");
		if (a->n >= C6E_ABSENT_MAX)
			break;
		triple_key(a->key[a->n], sizeof(a->key[0]), name_hex, mode,
			   group);
		a->n++;
	}
	fclose(fh);
	/*
	 * The score file must also say, in as many words, that nothing
	 * MISMATCHED -- an envelope derived from a run with a mismatch in it
	 * would be derived from a falsified function.
	 */
	fh = fopen(path, "r");
	if (fh != NULL) {
		int saw_zero_mismatch = 0;

		while (fgets(line, sizeof(line), fh) != NULL) {
			unsigned n = 1;

			if (sscanf(line, " MISMATCH : %u", &n) == 1 && n == 0u)
				saw_zero_mismatch = 1;
		}
		fclose(fh);
		ct_check(saw_zero_mismatch,
			  "the driven run scored 0 MISMATCH");
	}
}

static int absent_holds(const struct absent_set *a, const char *key)
{
	size_t i;

	for (i = 0; i < a->n; i++) {
		if (strcmp(a->key[i], key) == 0)
			return 1;
	}
	return 0;
}

/* Accumulate the driven run's OBSERVED triples only. */
static void cover_add_driven(struct cover *c, const char *pred_path,
			     const struct absent_set *absent)
{
	char line[DLM_HASH_TSV_LINE_MAX];
	FILE *fh = fopen(pred_path, "r");
	unsigned long skipped = 0;
	unsigned long before = c->rows;

	ct_check(fh != NULL, "the driven run's prediction file opens");
	if (fh == NULL)
		return;
	while (fgets(line, sizeof(line), fh) != NULL) {
		struct hash_row row;
		char name_hex[DLM_HASH_TSV_LINE_MAX];
		char key[DLM_HASH_TSV_LINE_MAX];
		size_t n;

		if (line[0] == '\n' || line[0] == '#' ||
		    strncmp(line, "name_hex", 8) == 0)
			continue;
		if (parse_row(line, &row) != 0)
			continue;
		for (n = 0; n < sizeof(name_hex) - 1u && line[n] != '\t' &&
			    line[n] != '\n' && line[n] != '\0'; n++)
			name_hex[n] = line[n];
		name_hex[n] = '\0';
		triple_key(key, sizeof(key), name_hex, row.mode, row.group);
		if (absent_holds(absent, key)) {
			skipped++;
			continue;
		}
		cover_add(c, &row);
	}
	fclose(fh);
	printf("driven run: %lu triples observed, %lu ABSENT and not counted\n",
	       c->rows - before, skipped);
	ct_check(skipped == absent->n,
		  "every ABSENT line matched a pre-registered triple");
}

static void test_coverage(void)
{
	struct absent_set absent;
	struct cover c;

	memset(&c, 0, sizeof(c));
	cover_add_tsv(&c, OVMX_DLM_HASH_DIR "/dlm_hash_derivation.tsv");
	cover_add_tsv(&c, OVMX_DLM_HASH_DIR "/dlm_hash_heldout.tsv");
	cover_add_tsv(&c, OVMX_DLM_HASH_DIR "/dlm_hash_predicted_m3soledir.tsv");
	absent_load(&absent, OVMX_C6E_RUN_DIR "/run-20261009-score.txt");
	cover_add_driven(&c, OVMX_C6E_RUN_DIR "/predicted.tsv", &absent);

	printf("observed: len_mask=%08x mode_mask=%08x group_or=%u\n",
	       c.len_mask, c.mode_mask, (unsigned)c.group_or);

	ct_check(c.len_mask == VMS_DLM_HASH_LEN_PROVEN,
		  "VMS_DLM_HASH_LEN_PROVEN is exactly the observed lengths");
	ct_check(c.mode_mask == VMS_DLM_HASH_MODE_PROVEN,
		  "VMS_DLM_HASH_MODE_PROVEN is exactly the observed modes");
	ct_check((uint32_t)c.group_or == VMS_DLM_HASH_GROUP_PROVEN_MAX,
		  "VMS_DLM_HASH_GROUP_PROVEN_MAX is the observed group bit span");

	/* And the gate behaves as those constants say, at each boundary. */
	{
		static const uint8_t nm[31] = {
			'A','B','C','D','E','F','G','H','I','J','K','L','M','N',
			'O','P','Q','R','S','T','U','V','W','X','Y','Z','0','1',
			'2','3','4'
		};
		uint32_t out = 0xdeadbeefu;

		ct_check(vms_dlm_name_hash_coverage(0, 3, 5) == VMS_DLM_HASH_OK,
			  "user mode, group 0, length 5 is covered");
		ct_check(vms_dlm_name_hash_coverage(16383, 1, 31) ==
			  VMS_DLM_HASH_OK,
			  "exec mode, group 16383, length 31 is covered");
		ct_check(vms_dlm_name_hash_coverage(0, 2, 5) ==
			  VMS_DLM_HASH_E_COVER,
			  "SUPERVISOR mode is refused, never extrapolated");
		ct_check(vms_dlm_name_hash_coverage(16384, 3, 5) ==
			  VMS_DLM_HASH_E_COVER,
			  "a UIC group with bit 14 set is refused");
		ct_check(vms_dlm_name_hash_coverage(0, 3, 23) ==
			  VMS_DLM_HASH_E_COVER,
			  "a 23-byte name is refused (never seen on the wire)");
		ct_check(vms_dlm_name_hash_coverage(0, 3, 29) ==
			  VMS_DLM_HASH_E_COVER,
			  "a 29-byte name is refused (never seen on the wire)");
		ct_check(vms_dlm_name_hash_proven(0, 2, nm, 5, &out) ==
			  VMS_DLM_HASH_E_COVER,
			  "the routing entry point refuses an uncovered identity");
		ct_check(out == 0xdeadbeefu,
			  "a coverage refusal writes no value (INV-6)");
		ct_check(vms_dlm_name_hash_proven(0, 3, nm, 5, &out) ==
			  VMS_DLM_HASH_OK && out != 0xdeadbeefu,
			  "the routing entry point serves a covered identity");
	}
}

int main(void)
{
	test_refusals();
	test_named_specimen();
	test_coverage();
	/* The row counts are the ones frozen with the corpus (rd vms-9c95);
	 * a shrinking fixture must not quietly weaken this proof. */
	report("derivation split", OVMX_DLM_HASH_DIR "/dlm_hash_derivation.tsv",
	       963);
	report("HELD-OUT split", OVMX_DLM_HASH_DIR "/dlm_hash_heldout.tsv",
	       253);
	report("FORWARD PREDICTION (m3-soledir, captured after the function was frozen)",
	       OVMX_DLM_HASH_DIR "/dlm_hash_predicted_m3soledir.tsv", 533);
	report_predictions("DRIVEN-RUN predictions (rd vms-c6e leg 2, no capture yet)",
			   OVMX_C6E_RUN_DIR "/predicted.tsv", 81);
	return ct_summary("test_dlm_hash");
}
