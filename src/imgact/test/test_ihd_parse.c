/*
 * test_ihd_parse.c -- host unit test for the OpenVMS VAX image reader
 * (src/imgact/imgact_ihd.h, rd vms-b869), run over the real images LINKed on
 * the lab VAX V7.3 node (tests/native-images/vax/) and checked against that
 * node's ANALYZE/IMAGE reports (tests/lab/captures/native-image-vax-20261009/).
 * usage: test_ihd_parse <dir-with-the-images>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgact_ihd.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

struct img { unsigned char *b; long len; struct ihd_info h; const unsigned char *iaf;
	     unsigned long iaflen; struct iaf_info a; };

static void load(const char *dir, const char *name, struct img *m)
{
	char path[512];
	memset(m, 0, sizeof *m);
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("FAIL cannot open %s\n", path); exit(2); }
	fseek(f, 0, SEEK_END); m->len = ftell(f); fseek(f, 0, SEEK_SET);
	m->b = malloc((size_t)m->len);
	if (fread(m->b, 1, (size_t)m->len, f) != (size_t)m->len) exit(2);
	fclose(f);
	CHECK(ihd_parse_header(m->b, (unsigned long)m->len, &m->h) == 0, "%s header", name);
	unsigned long off = m->h.size;
	struct ihd_isd d;
	while (ihd_next_isd(m->b, &m->h, &off, &d) == 1) {
		if ((d.flags & ISD_M_GBL) || !d.vbn) continue;
		if (m->h.iafva >= d.va && m->h.iafva < d.va + d.pagcnt * IHD_PAGE) {
			m->iaf = m->b + (d.vbn - 1) * IHD_BLOCK + (m->h.iafva - d.va);
			m->iaflen = d.va + d.pagcnt * IHD_PAGE - m->h.iafva;
		}
	}
	CHECK(m->iaf && iaf_parse(m->iaf, m->iaflen, &m->a) == 0, "%s IAF", name);
}

static int n_g, n_dot; static uint32_t g_shl[200], g_cell[200], g_val[200];
static int ref_cb(void *arg, uint32_t shl, uint32_t co, uint32_t v)
{ (void)arg; if (n_g < 200) { g_shl[n_g] = shl; g_cell[n_g] = co; g_val[n_g] = v; } n_g++; return 0; }
static int cnt_cb(void *arg, uint32_t shl, uint32_t co, uint32_t v)
{ (void)arg; (void)shl; (void)co; (void)v; n_dot++; return 0; }
static uint32_t cp_off, cp_pages, cp_prt; static int cp_n;
static void cp_cb(void *arg, uint32_t o, uint32_t p, uint32_t prt)
{ (void)arg; cp_off = o; cp_pages = p; cp_prt = prt; cp_n++; }

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/native-images/vax";
	struct img m;
	char nm[48];

	load(dir, "HELLO.EXE", &m);
	CHECK(m.h.imgtype == IHD_K_EXE && m.h.hdrblkcnt == 1, "HELLO type/blocks");
	CHECK(m.h.lnkflags == 0xA8, "HELLO lnkflags %x", m.h.lnkflags);
	CHECK(m.h.tfr[0] == 0x7FFEDF68 && m.h.tfr[1] == 0x400 && m.h.tfr[2] == 0, "HELLO transfer");
	CHECK(m.h.iafva == 0x600, "HELLO iafva");
	CHECK(strcmp(m.h.imgnam, "HELLO") == 0, "HELLO name '%s'", m.h.imgnam);
	static const struct { uint32_t va; uint16_t pg; uint32_t fl; uint8_t ty; uint32_t vbn; } w[] = {
		{ 0x200, 1, 0x8A, 0, 2 }, { 0x400, 1, 0x80, 0, 3 }, { 0x600, 1, 0x48A, 0, 4 },
		{ 0x7FFFD800, 20, 0x8C, ISD_K_USRSTACK, 0 }, { 0, 265, 0x21, ISD_K_SHRPIC, 0 } };
	unsigned long off = m.h.size; struct ihd_isd d; int n = 0, rc;
	while ((rc = ihd_next_isd(m.b, &m.h, &off, &d)) == 1) {
		if (n < 5) {
			CHECK(d.va == w[n].va && d.pagcnt == w[n].pg && d.type == w[n].ty &&
			      d.vbn == w[n].vbn, "HELLO isd%d va=%x pg=%u ty=%u vbn=%u", n + 1,
			      d.va, d.pagcnt, d.type, d.vbn);
		}
		if (n == 4)
			CHECK(!strcmp(d.gblnam, "LIBRTL_001") && d.ident == 0x0100000E && d.matchctl == 2,
			      "HELLO isd5 global section %s %x %u", d.gblnam, d.ident, d.matchctl);
		n++;
	}
	CHECK(rc == 0 && n == 5, "HELLO isd count %d", n);
	CHECK(m.a.shrimgcnt == 2 && iaf_shl_name(m.iaf, &m.a, 1, nm, sizeof nm) == 0 &&
	      !strcmp(nm, "LIBRTL"), "HELLO shareable list");
	n_g = 0;
	CHECK(iaf_walk_refs(m.iaf, m.iaflen, m.a.gfixoff, m.a.shrimgcnt, ref_cb, 0) == 1 &&
	      g_shl[0] == 1 && g_val[0] == 0x478 && m.h.iafva + g_cell[0] == 0x648,
	      "HELLO G^ cell at 0x648 holds LIB$PUT_OUTPUT's transfer-vector offset 0x478");
	cp_n = 0;
	CHECK(iaf_walk_chgprt(m.iaf, m.iaflen, &m.a, cp_cb, 0) == 1 && cp_off == 0x400 &&
	      cp_pages == 1 && cp_prt == 13, "HELLO protection change");

	load(dir, "MYSHR.EXE", &m);
	CHECK(m.h.imgtype == IHD_K_LIM && m.h.ident == 0x01000000 && m.h.matchctl == 2 &&
	      m.h.iafva == 0x400, "MYSHR shareable ident/match/iafva");
	n_g = 0;
	CHECK(iaf_walk_refs(m.iaf, m.iaflen, m.a.dotadroff, m.a.shrimgcnt, ref_cb, 0) == 1 &&
	      g_shl[0] == 0 && g_val[0] == 0x204, "MYSHR .ADDRESS self-relocation at 0x204");

	load(dir, "MYSHRV2.EXE", &m);
	CHECK(m.h.ident == 0x02000000, "MYSHRV2 ident 2.0");

	load(dir, "MAIN3.EXE", &m);
	CHECK(m.a.shrimgcnt == 3 && iaf_shl_name(m.iaf, &m.a, 1, nm, sizeof nm) == 0 &&
	      !strcmp(nm, "MYSHR"), "MAIN3 shareable 1 is MYSHR (%s)", nm);

	load(dir, "CSTDIO.EXE", &m);
	int found = 0;
	for (unsigned i = 1; i < m.a.shrimgcnt; i++)
		if (iaf_shl_name(m.iaf, &m.a, i, nm, sizeof nm) == 0 && !strcmp(nm, "DECC$SHR"))
			found = 1;
	CHECK(found, "CSTDIO references DECC$SHR");

	load(dir, "ORDSVX.EXE", &m);
	n_g = 0; n_dot = 0;
	CHECK(iaf_walk_refs(m.iaf, m.iaflen, m.a.gfixoff, m.a.shrimgcnt, ref_cb, 0) == 192 &&
	      iaf_walk_refs(m.iaf, m.iaflen, m.a.dotadroff, m.a.shrimgcnt, cnt_cb, 0) == n_dot,
	      "ORDSVX G^ list (%d)", n_g);

	{
		unsigned char blk[IHD_BLOCK]; struct ihd_info h;
		memset(blk, ' ', sizeof blk);
		CHECK(ihd_parse_header(blk, sizeof blk, &h) == -1, "a text block is not a VAX image");
	}
	if (failures) { printf("test_ihd_parse: %d FAILURE(S)\n", failures); return 1; }
	printf("test_ihd_parse: PASS\n");
	return 0;
}
