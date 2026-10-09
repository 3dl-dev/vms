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

		if (line[0] == '\n' || line[0] == '#')
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

int main(void)
{
	test_refusals();
	test_named_specimen();
	/* The row counts are the ones frozen with the corpus (rd vms-9c95);
	 * a shrinking fixture must not quietly weaken this proof. */
	report("derivation split", OVMX_DLM_HASH_DIR "/dlm_hash_derivation.tsv",
	       963);
	report("HELD-OUT split", OVMX_DLM_HASH_DIR "/dlm_hash_heldout.tsv",
	       253);
	report("FORWARD PREDICTION (m3-soledir, captured after the function was frozen)",
	       OVMX_DLM_HASH_DIR "/dlm_hash_predicted_m3soledir.tsv", 533);
	return ct_summary("test_dlm_hash");
}
