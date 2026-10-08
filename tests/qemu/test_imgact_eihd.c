/*
 * test_imgact_eihd.c -- the activation steps IMGACT.EXE performs on an OpenVMS
 * Alpha native image (vms-3b3f), run on the real images.
 *
 * MAIN3.EXE and MYSHR.EXE were LINKed on the lab OpenVMS Alpha V8.4 node from
 * our own MACRO-32 sources (tests/native-images/alpha/); on that node MAIN3
 * calls MYSHR_GREET and MYSHR_ADD in MYSHR and LIB$PUT_OUTPUT in LIBRTL
 * (tests/lab/captures/native-image-alpha-20261008/). This suite does with
 * those bytes what IMGACT does when it activates them -- through the SAME
 * functions (src/imgact/imgact_eihd.h): place MYSHR away from its link
 * address and apply its relocations, check MAIN3's GSMATCH against MYSHR's
 * ident, and fill MAIN3's linkage pairs from MYSHR's symbol vector and from a
 * vector table standing in for OVMX's LIBRTL. It executes no Alpha code, so it
 * runs on every host; the booted Alpha proof is CI alpha-native-image.
 *
 * USERSPACE (test_imgact_*): needs no /dev/vms.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgact_eihd.h"

extern const uint8_t eihd_img_main3[], eihd_img_main3_end[];
extern const uint8_t eihd_img_myshr[], eihd_img_myshr_end[];
extern const uint8_t eihd_img_hello[], eihd_img_hello_end[];
extern const uint8_t eihd_img_text[], eihd_img_text_end[];

static int pass = 0, fail = 0;
static void check(int cond, const char *name)
{
	if (cond) { printf("  PASS: %s\n", name); pass++; }
	else      { printf("  FAIL: %s\n", name); fail++; }
}

/* One image placed in memory the way IMGACT places it. */
struct placed {
	const uint8_t   *file;
	unsigned long    flen;
	struct eihd_info h;
	unsigned long    lo, span;
	uint8_t         *rt;          /* run-time address of `lo`             */
	uint64_t         delta;
	const uint8_t   *iaf;
	unsigned long    iaflen;
	struct eiaf_info a;
};

static int place(struct placed *p, const uint8_t *b, const uint8_t *e)
{
	memset(p, 0, sizeof *p);
	p->file = b;
	p->flen = (unsigned long)(e - b);
	if (eihd_parse_header(b, p->flen, &p->h) != 0)
		return -1;
	unsigned long off = p->h.isdoff, hi = 0;
	struct eihd_isd d;
	p->lo = ~0UL;
	while (eihd_next_isd(b, &p->h, &off, &d) == 1) {
		if ((d.flags & EISD_M_GBL) || d.type == EISD_K_USRSTACK)
			continue;
		if (d.va < p->lo) p->lo = d.va;
		if (d.va + d.secsize > hi) hi = d.va + d.secsize;
	}
	if (p->lo == ~0UL)
		return -1;
	p->span = hi - p->lo;
	p->rt = calloc(1, p->span);
	if (!p->rt)
		return -1;
	p->delta = (uint64_t)(uintptr_t)p->rt - p->lo;
	off = p->h.isdoff;
	while (eihd_next_isd(b, &p->h, &off, &d) == 1) {
		if ((d.flags & EISD_M_GBL) || d.type == EISD_K_USRSTACK ||
		    (d.flags & EISD_M_DZRO))
			continue;
		unsigned long fo = (unsigned long)(d.vbn - 1) * EIHD_BLOCK;
		if (!d.vbn || fo + d.secsize > p->flen)
			return -1;
		memcpy(p->rt + (d.va - p->lo), b + fo, d.secsize);
		if (p->h.iafva >= d.va && p->h.iafva < d.va + d.secsize) {
			p->iaf = p->rt + (p->h.iafva - p->lo);
			p->iaflen = d.va + d.secsize - p->h.iafva;
		}
	}
	if (!p->iaf || eiaf_parse(p->iaf, p->iaflen, &p->a) != 0)
		return -1;
	return 0;
}

static uint64_t q_at(const struct placed *p, unsigned long va)
{
	return eihd_q(p->rt + (va - p->lo));
}

/* GSMATCH of the shareable named `shl` as the consumer recorded it. */
static int consumer_match(const struct placed *c, const char *shl,
			  uint32_t *ident, uint32_t *ctl)
{
	unsigned long off = c->h.isdoff;
	struct eihd_isd d;
	while (eihd_next_isd(c->file, &c->h, &off, &d) == 1)
		if ((d.flags & EISD_M_GBL) && eihd_gblnam_is(d.gblnam, shl)) {
			*ident = d.ident;
			*ctl = d.matchctl;
			return 0;
		}
	return -1;
}

/* MAIN3's shareable list: 0 MYSHR (the native image placed here), 1 LIBRTL and
 * 2 SYS$PUBLIC_VECTORS (a stand-in vector: code and procedure value derived
 * from the offset, so a pair filled from the wrong entry or with its halves
 * swapped is visible). */
static struct placed g_myshr;
static int g_refuse;
#define STANDIN_CODE(v) (0x7C0DE0000ull + (v))
#define STANDIN_PV(v)   (0x7DE5C0000ull + (v))
static int resolve(void *arg, uint32_t shl, uint64_t voff, uint64_t *code, uint64_t *pv)
{
	(void)arg;
	if (g_refuse)
		return -1;
	if (shl == 0)
		return eihd_symvec_entry(g_myshr.rt, g_myshr.lo, g_myshr.span,
					 &g_myshr.h, voff, code, pv);
	if (voff % 16)
		return -1;
	*code = STANDIN_CODE(voff);
	*pv   = STANDIN_PV(voff);
	return 0;
}

int main(void)
{
	printf("=== test_imgact_eihd: OpenVMS Alpha native image activation steps (vms-3b3f) ===\n");

	struct placed main3, hello;
	check(place(&g_myshr, eihd_img_myshr, eihd_img_myshr_end) == 0 &&
	      g_myshr.h.imgtype == EIHD_K_LIM,
	      "MYSHR.EXE reads as an OpenVMS Alpha shareable image");
	check(place(&main3, eihd_img_main3, eihd_img_main3_end) == 0 &&
	      main3.h.imgtype == EIHD_K_EXE,
	      "MAIN3.EXE reads as an OpenVMS Alpha executable image");
	check(place(&hello, eihd_img_hello, eihd_img_hello_end) == 0 &&
	      hello.h.tfr[0] == 0xFFFFFFFF00000340ull && hello.h.tfr[1] == 0x10000,
	      "HELLO.EXE's transfer vector is SYS$IMGSTA (SYS$PUBLIC_VECTORS +%X340), then its main");
	{
		struct eihd_info h;
		uint8_t blk[EIHD_BLOCK] = { 0 };
		unsigned long n = (unsigned long)(eihd_img_text_end - eihd_img_text);
		memcpy(blk, eihd_img_text, n < sizeof blk ? n : sizeof blk);
		check(eihd_parse_header(blk, sizeof blk, &h) == -1,
		      "a text file is not an OpenVMS Alpha image");
	}

	/* MYSHR placed away from its link address: its relocations must move
	 * every address cell -- the symbol vector's {code, procedure value}
	 * pairs and its data's descriptor pointer -- by exactly that distance. */
	uint32_t gmsg_ptr = eihd_l(g_myshr.rt + 0x10004);
	long nrel = eihd_relocate(g_myshr.iaf, g_myshr.iaflen, &g_myshr.a, g_myshr.rt,
				  g_myshr.span, g_myshr.delta);
	check(nrel == 8, "MYSHR's 7 quadword + 1 longword relocations all apply");
	check(q_at(&g_myshr, 0x60) == 0x20000 + g_myshr.delta &&
	      q_at(&g_myshr, 0x68) == 0x00000 + g_myshr.delta &&
	      q_at(&g_myshr, 0x70) == 0x20030 + g_myshr.delta &&
	      q_at(&g_myshr, 0x78) == 0x00020 + g_myshr.delta,
	      "MYSHR's symbol vector entries are relocated to where it was placed");
	check(eihd_l(g_myshr.rt + 0x10004) == (uint32_t)(gmsg_ptr + g_myshr.delta),
	      "MYSHR's longword descriptor pointer is relocated");

	/* GSMATCH, as MAIN3 recorded it: LEQUAL 1.0 against MYSHR. */
	uint32_t want = 0, ctl = 99;
	check(consumer_match(&main3, "MYSHR", &want, &ctl) == 0 &&
	      ctl == EIHD_MATCH_LEQUAL && want == 0x01000000 &&
	      eihd_gsmatch_ok(ctl, want, g_myshr.h.ident),
	      "MAIN3 accepts the MYSHR it was linked against (LEQUAL 1.0)");
	check(!eihd_gsmatch_ok(ctl, want, 0x02000000u),
	      "MAIN3 refuses MYSHR relinked GSMATCH=LEQUAL,2,0 (SHRIDMISMAT on VMS)");
	check(consumer_match(&main3, "SYS$PUBLIC_VECTORS", &want, &ctl) == 0 &&
	      ctl == EIHD_MATCH_EQUAL && eihd_gsmatch_ok(ctl, want, 0x33FECE84u) &&
	      !eihd_gsmatch_ok(ctl, want, 0x33FECE85u),
	      "SYS$PUBLIC_VECTORS must match MAIN3's recorded ident exactly (EQUAL 51.16699012)");

	/* Linkage pairs. Before the fixup each holds its target's vector offset. */
	uint64_t pre50 = q_at(&main3, 0x10050), pre60 = q_at(&main3, 0x10060);
	uint64_t pre30 = q_at(&main3, 0x10030), pre90 = q_at(&main3, 0x10090);
	check((pre50 == 0x0 || pre50 == 0x10) && (pre60 == 0x0 || pre60 == 0x10) &&
	      pre50 != pre60 && pre30 == 0x320 && pre90 == 0x320,
	      "MAIN3's linkage pairs name MYSHR_ADD/MYSHR_GREET and LIB$PUT_OUTPUT by vector offset");
	g_refuse = 1;
	check(eihd_fixup_lp(main3.iaf, main3.iaflen, &main3.a, main3.rt, main3.span,
			    resolve, 0) < 0,
	      "a linkage pair whose target entry does not resolve fails the fixup");
	g_refuse = 0;
	/* the refused pass stopped at the first pair; re-place for the real one */
	free(main3.rt);
	place(&main3, eihd_img_main3, eihd_img_main3_end);
	long nlp = eihd_fixup_lp(main3.iaf, main3.iaflen, &main3.a, main3.rt, main3.span,
				 resolve, 0);
	uint64_t e50c, e50p, e60c, e60p;
	eihd_symvec_entry(g_myshr.rt, g_myshr.lo, g_myshr.span, &g_myshr.h, pre50, &e50c, &e50p);
	eihd_symvec_entry(g_myshr.rt, g_myshr.lo, g_myshr.span, &g_myshr.h, pre60, &e60c, &e60p);
	/* negctl: eihd-lp-pair-swapped */
	check(nlp == 4 &&
	      q_at(&main3, 0x10050) == e50c && q_at(&main3, 0x10058) == e50p &&
	      q_at(&main3, 0x10060) == e60c && q_at(&main3, 0x10068) == e60p,
	      "MAIN3's linkage pairs to MYSHR hold MYSHR's relocated {code address, procedure value}");
	check(q_at(&main3, 0x10030) == STANDIN_CODE(0x320) &&
	      q_at(&main3, 0x10038) == STANDIN_PV(0x320) &&
	      q_at(&main3, 0x10090) == STANDIN_CODE(0x320) &&
	      q_at(&main3, 0x10098) == STANDIN_PV(0x320),
	      "MAIN3's linkage pairs to LIBRTL hold the {code address, procedure value} of entry %X320");

	printf("=== test_imgact_eihd: %d passed, %d failed ===\n", pass, fail);
	return fail == 0 ? 0 : 1;
}
