/*
 * test_eihd_parse.c -- host unit test for the OpenVMS Alpha image reader
 * (src/imgact/imgact_eihd.h, rd vms-3b3f).
 *
 * Runs the reader over the REAL images we LINKed on the lab OpenVMS Alpha V8.4
 * node from our own MACRO-32 sources (tests/native-images/alpha/) and checks
 * every field against what that node's own ANALYZE/IMAGE reported for the same
 * file (the .ANL.txt files in tests/lab/captures/native-image-alpha-20261008/). A reader
 * that mis-places a field fails here before any boot.
 *
 * usage: test_eihd_parse <dir-with-the-images>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgact_eihd.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

struct img {
	unsigned char *b;
	long len;
	struct eihd_info h;
	const unsigned char *iaf;
	unsigned long iaflen;
	struct eiaf_info a;
};

static unsigned char *slurp(const char *dir, const char *name, long *len)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("FAIL cannot open %s\n", path);
		exit(2);
	}
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *b = malloc((size_t)*len);
	if (fread(b, 1, (size_t)*len, f) != (size_t)*len)
		exit(2);
	fclose(f);
	return b;
}

/* Load an image and locate its fixup section through the ISD that maps it. */
static void load(const char *dir, const char *name, struct img *m)
{
	memset(m, 0, sizeof *m);
	m->b = slurp(dir, name, &m->len);
	int rc = eihd_parse_header(m->b, (unsigned long)m->len, &m->h);
	CHECK(rc == 0, "%s: header rc=%d", name, rc);
	if (rc)
		return;
	unsigned long off = m->h.isdoff;
	struct eihd_isd d;
	while (eihd_next_isd(m->b, &m->h, &off, &d) == 1) {
		if ((d.flags & EISD_M_GBL) || !d.vbn)
			continue;
		if (m->h.iafva >= d.va && m->h.iafva < d.va + d.secsize) {
			unsigned long fo = (d.vbn - 1) * EIHD_BLOCK + (m->h.iafva - d.va);
			m->iaf = m->b + fo;
			m->iaflen = d.secsize - (m->h.iafva - d.va);
		}
	}
	CHECK(m->iaf != 0, "%s: no ISD maps the fixup section", name);
	if (m->iaf)
		CHECK(eiaf_parse(m->iaf, m->iaflen, &m->a) == 0, "%s: EIAF parse", name);
}

static int collect_n;
static uint32_t collected[64];
static void rel_cb(void *arg, uint32_t off)
{
	(void)arg;
	if (collect_n < 64)
		collected[collect_n++] = off;
}

static int lp_n;
static uint32_t lp_shl[64], lp_off[64];
static int lp_cb(void *arg, uint32_t shl, uint32_t off)
{
	(void)arg;
	if (lp_n < 64) {
		lp_shl[lp_n] = shl;
		lp_off[lp_n] = off;
		lp_n++;
	}
	return 0;
}

static int cp_n;
static uint64_t cp_off[16];
static uint32_t cp_len[16], cp_prt[16];
static void cp_cb(void *arg, uint64_t off, uint32_t len, uint32_t prt)
{
	(void)arg;
	if (cp_n < 16) {
		cp_off[cp_n] = off;
		cp_len[cp_n] = len;
		cp_prt[cp_n] = prt;
		cp_n++;
	}
}

static void check_shl(struct img *m, const char *name, unsigned i, const char *want)
{
	char nm[48];
	int rc = eiaf_shl_name(m->iaf, m->iaflen, &m->a, i, nm, sizeof nm);
	CHECK(rc == 0 && strcmp(nm, want) == 0, "%s: shl[%u] = '%s' want '%s'", name, i,
	      rc ? "?" : nm, want);
}

/* Value of the quadword at virtual address `va` in the file image. */
static uint64_t q_at(struct img *m, uint64_t va)
{
	unsigned long off = m->h.isdoff;
	struct eihd_isd d;
	while (eihd_next_isd(m->b, &m->h, &off, &d) == 1)
		if (!(d.flags & EISD_M_GBL) && d.vbn && va >= d.va && va + 8 <= d.va + d.secsize)
			return eihd_q(m->b + (d.vbn - 1) * EIHD_BLOCK + (va - d.va));
	return ~0ull;
}

static void test_hello(const char *dir)
{
	struct img m;
	load(dir, "HELLO.EXE", &m);
	CHECK(m.h.imgtype == EIHD_K_EXE, "HELLO type %u", m.h.imgtype);
	CHECK(m.h.hdrblkcnt == 2, "HELLO hdrblkcnt %u", m.h.hdrblkcnt);
	CHECK(m.h.iafva == 0x40000, "HELLO iafva %llx", (unsigned long long)m.h.iafva);
	CHECK(m.h.symvva == 0 && m.h.symvec_size == 0, "HELLO symvec");
	CHECK(m.h.lnkflags == 0x28, "HELLO lnkflags %x (PICIMG|DBGDMT)", m.h.lnkflags);
	CHECK(m.h.tfr[0] == 0xFFFFFFFF00000340ull, "HELLO tfr1 %llx", (unsigned long long)m.h.tfr[0]);
	CHECK(m.h.tfr[1] == 0x10000, "HELLO tfr2 %llx", (unsigned long long)m.h.tfr[1]);
	CHECK(m.h.tfr[2] == 0, "HELLO tfr3");
	CHECK(m.h.inishr == 0, "HELLO inishr");
	CHECK(strcmp(m.h.imgnam, "HELLO") == 0, "HELLO imgnam '%s'", m.h.imgnam);

	/* The seven ISDs ANALYZE/IMAGE listed. */
	static const struct { uint64_t va; uint32_t sz, flags, vbn; uint8_t type; } want[] = {
		{ 0x10000,    512,   0x140A, 3, EISD_K_NORMAL },
		{ 0x20000,    512,   0x040A, 4, EISD_K_NORMAL },
		{ 0x30000,    512,   0x0C00, 5, EISD_K_NORMAL },
		{ 0x40000,    512,   0x004A, 6, EISD_K_NORMAL },
		{ 0x7FFF0000, 10240, 0x040C, 0, EISD_K_USRSTACK },
		{ 0,          914944, 0x0001, 0, EISD_K_SHRPIC },
		{ 0,          10240, 0x0001, 0, EISD_K_PRVFXD },
	};
	unsigned long off = m.h.isdoff;
	struct eihd_isd d;
	unsigned n = 0;
	int rc;
	while ((rc = eihd_next_isd(m.b, &m.h, &off, &d)) == 1) {
		if (n < 7) {
			CHECK(d.va == want[n].va, "HELLO isd%u va %llx", n + 1, (unsigned long long)d.va);
			CHECK(d.secsize == want[n].sz, "HELLO isd%u size %u", n + 1, d.secsize);
			CHECK(d.flags == want[n].flags, "HELLO isd%u flags %x", n + 1, d.flags);
			CHECK(d.vbn == want[n].vbn, "HELLO isd%u vbn %u", n + 1, d.vbn);
			CHECK(d.type == want[n].type, "HELLO isd%u type %u", n + 1, d.type);
		}
		if (n == 5) {
			CHECK(strcmp(d.gblnam, "LIBRTL_001") == 0, "isd6 gblnam %s", d.gblnam);
			CHECK(d.ident == 0x01000001 && d.matchctl == EIHD_MATCH_LEQUAL, "isd6 match");
			CHECK(eihd_gblnam_is(d.gblnam, "LIBRTL"), "isd6 gblnam_is");
		}
		if (n == 6) {
			CHECK(strcmp(d.gblnam, "SYS$PUBLIC_VECTORS_001") == 0, "isd7 gblnam %s", d.gblnam);
			CHECK(d.ident == 0x33FECE84 && d.matchctl == EIHD_MATCH_EQUAL, "isd7 match");
			CHECK(!eihd_gblnam_is(d.gblnam, "SYS$PUBLIC"), "isd7 prefix must be whole name");
		}
		n++;
	}
	CHECK(rc == 0 && n == 7, "HELLO isd count %u rc %d", n, rc);

	/* Fixup section. */
	CHECK(!(m.a.flags & 1), "HELLO EIAF$V_SHR");
	CHECK(m.a.shrimgcnt == 2, "HELLO shrimgcnt %u", m.a.shrimgcnt);
	CHECK(m.a.lpfixoff == 0x58 && m.a.chgprtoff == 0x80 && m.a.shlstoff == 0xA8,
	      "HELLO EIAF offsets");
	CHECK(!eiaf_has_unsupported(&m.a), "HELLO uses only observed fixup kinds");
	check_shl(&m, "HELLO", 0, "LIBRTL");
	check_shl(&m, "HELLO", 1, "SYS$PUBLIC_VECTORS");
	lp_n = 0;
	CHECK(eiaf_walk_lp(m.iaf, m.iaflen, &m.a, lp_cb, 0) == 3, "HELLO lp count");
	CHECK(lp_shl[0] == 0 && lp_off[0] == 0x40, "HELLO lp0");
	CHECK(lp_shl[1] == 1 && lp_off[1] == 0x30, "HELLO lp1");
	CHECK(lp_shl[2] == 1 && lp_off[2] == 0x50, "HELLO lp2");
	/* Before activation a linkage pair holds the symbol-vector byte offset of
	 * its target (the values the link map shows) and a zero. */
	CHECK(q_at(&m, 0x10040) == 0x320 && q_at(&m, 0x10048) == 0, "LIB$PUT_OUTPUT lp");
	CHECK(q_at(&m, 0x10030) == 0x100, "SYS$ASSIGN lp");
	CHECK(q_at(&m, 0x10050) == 0x80, "SYS$QIOW lp");
	/* The main transfer address is a procedure descriptor whose entry is the
	 * code section. */
	CHECK(q_at(&m, 0x10008) == 0x30000, "HELLO PDSC entry");
	cp_n = 0;
	CHECK(eiaf_walk_chgprt(m.iaf, m.iaflen, &m.a, cp_cb, 0) == 2, "HELLO chgprt count");
	CHECK(cp_off[0] == 0x30000 && cp_len[0] == 512 && cp_prt[0] == EIHD_PRT_UREW, "chgprt0");
	CHECK(cp_off[1] == 0 && cp_len[1] == 512 && cp_prt[1] == EIHD_PRT_UR, "chgprt1");
	collect_n = 0;
	CHECK(eiaf_walk_rel(m.iaf, m.iaflen, m.a.qrelfixoff, 8, rel_cb, 0) == 0, "HELLO no qrel");
}

static void test_myshr(const char *dir)
{
	struct img m;
	load(dir, "MYSHR.EXE", &m);
	CHECK(m.h.imgtype == EIHD_K_LIM, "MYSHR type %u", m.h.imgtype);
	CHECK(m.h.ident == 0x01000000 && m.h.matchctl == EIHD_MATCH_LEQUAL, "MYSHR gsmatch");
	CHECK(m.h.symvva == 0x60 && m.h.symvec_size == 32, "MYSHR symvec");
	CHECK(m.h.iafva == 0x30000, "MYSHR iafva");
	CHECK(m.a.flags & 1, "MYSHR EIAF$V_SHR");
	collect_n = 0;
	static const uint32_t q[] = { 0x8, 0x28, 0x50, 0x60, 0x68, 0x70, 0x78 };
	CHECK(eiaf_walk_rel(m.iaf, m.iaflen, m.a.qrelfixoff, 8, rel_cb, 0) == 7, "MYSHR qrel n");
	for (int i = 0; i < 7; i++)
		CHECK(collected[i] == q[i], "MYSHR qrel[%d] %x", i, collected[i]);
	collect_n = 0;
	CHECK(eiaf_walk_rel(m.iaf, m.iaflen, m.a.lrelfixoff, 4, rel_cb, 0) == 1, "MYSHR lrel n");
	CHECK(collected[0] == 0x10004, "MYSHR lrel %x", collected[0]);
	/* A procedure's symbol vector entry is {code address, procedure value},
	 * the same layout as a linkage pair. */
	CHECK(q_at(&m, 0x60) == 0x20000 && q_at(&m, 0x68) == 0x0, "MYSHR_ADD entry");
	CHECK(q_at(&m, 0x70) == 0x20030 && q_at(&m, 0x78) == 0x20, "MYSHR_GREET entry");
	lp_n = 0;
	CHECK(eiaf_walk_lp(m.iaf, m.iaflen, &m.a, lp_cb, 0) == 1 && lp_off[0] == 0x40, "MYSHR lp");
	check_shl(&m, "MYSHR", 0, "LIBRTL");
}

static void test_main3(const char *dir)
{
	struct img m;
	load(dir, "MAIN3.EXE", &m);
	CHECK(m.a.shrimgcnt == 3, "MAIN3 shrimgcnt");
	check_shl(&m, "MAIN3", 0, "MYSHR");
	check_shl(&m, "MAIN3", 1, "LIBRTL");
	check_shl(&m, "MAIN3", 2, "SYS$PUBLIC_VECTORS");
	lp_n = 0;
	CHECK(eiaf_walk_lp(m.iaf, m.iaflen, &m.a, lp_cb, 0) == 4, "MAIN3 lp n");
	CHECK(lp_shl[0] == 0 && lp_off[0] == 0x50 && lp_shl[1] == 0 && lp_off[1] == 0x60, "MAIN3 lp MYSHR");
	CHECK(lp_shl[2] == 1 && lp_off[2] == 0x30 && lp_shl[3] == 1 && lp_off[3] == 0x90, "MAIN3 lp LIBRTL");
	CHECK(q_at(&m, 0x10050) == 0x0 || q_at(&m, 0x10050) == 0x10, "MAIN3 MYSHR vector offsets");
}

static void test_cstdio(const char *dir)
{
	struct img m;
	load(dir, "CSTDIO.EXE", &m);
	check_shl(&m, "CSTDIO", 0, "DECC$SHR");
	check_shl(&m, "CSTDIO", 1, "SYS$PUBLIC_VECTORS");
}

static void test_not_an_image(const char *dir)
{
	long len;
	unsigned char *b = slurp(dir, "HELLO.MAR", &len);
	struct eihd_info h;
	unsigned char blk[EIHD_BLOCK] = { 0 };
	memcpy(blk, b, len < (long)sizeof blk ? (size_t)len : sizeof blk);
	CHECK(eihd_parse_header(blk, sizeof blk, &h) == -1, "text file must not parse");
	/* An image header truncated to its first block (hdrblkcnt 2) is malformed. */
	long hl;
	unsigned char *hb = slurp(dir, "HELLO.EXE", &hl);
	CHECK(eihd_parse_header(hb, EIHD_BLOCK, &h) == -2, "truncated header must be refused");
	free(b);
	free(hb);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/native-images/alpha";
	test_hello(dir);
	test_myshr(dir);
	test_main3(dir);
	test_cstdio(dir);
	test_not_an_image(dir);
	if (failures) {
		printf("test_eihd_parse: %d FAILURE(S)\n", failures);
		return 1;
	}
	printf("test_eihd_parse: PASS\n");
	return 0;
}
