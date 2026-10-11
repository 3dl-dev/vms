/*
 * imgact_ihd.h -- reader for the OpenVMS VAX native image format (IHD/ISD/IAF),
 * rd vms-b869 (the VAX rung of vms-3b3f).
 *
 * Header-only and freestanding, like imgact_eihd.h (the Alpha reader).
 * Clean-room: every offset below was derived from OBSERVED behavior only --
 * images we assembled and linked ourselves from our own MACRO-32 sources on
 * the lab OpenVMS VAX V7.3 node, their LINK/MAP/FULL maps, ANALYZE/IMAGE
 * reports and DUMP output, correlated byte for byte
 * (tests/lab/captures/native-image-vax-20261009/). Field names are the ones
 * ANALYZE/IMAGE prints. No VSI/HPE source, header or binary was read. Anything
 * a captured image did not exercise is refused, never guessed.
 *
 * Observed differences from the Alpha EIHD format: 16-bit header offsets,
 * an ASCII "02"/"05" format id, byte-wide header block count and image type,
 * 512-byte pages addressed by virtual page number, and two kinds of
 * shareable reference fixup: G^ cells and .ADDRESS cells, both holding a
 * value to which the target image's base is added. System services are not
 * a shareable: an image calls them at fixed P1 addresses (the P1 system
 * service vector, docs/oracle/vax73-symvec-offsets.txt).
 */
#ifndef OVMX_IMGACT_IHD_H
#define OVMX_IMGACT_IHD_H

#include <stdint.h>

#define IHD_BLOCK        512u
#define IHD_PAGE         512u
#define IHD_K_EXE        1u
#define IHD_K_LIM        2u
#define IHD_MAX_HDRBLKS  16u

#define ISD_M_GBL        (1u << 0)
#define ISD_M_CRF        (1u << 1)
#define ISD_M_DZRO       (1u << 2)
#define ISD_M_WRT        (1u << 3)
#define ISD_M_FIXUPVEC   (1u << 10)
#define ISD_K_USRSTACK   253u
#define ISD_K_SHRPIC     3u

struct ihd_info {
	uint16_t size, activoff, symdbgoff, imgidoff;
	uint8_t  hdrblkcnt, imgtype;
	uint32_t lnkflags;
	uint8_t  matchctl;
	uint32_t ident;      /* GSMATCH ident (shareable): major<<24 | minor */
	uint32_t iafva;      /* fixup section virtual address */
	uint32_t tfr[3];
	char     imgnam[40];
};

struct ihd_isd {
	uint16_t size, pagcnt;
	uint32_t va;         /* vpn * 512 */
	uint32_t flags;      /* low 24 bits */
	uint8_t  type;       /* high byte */
	uint8_t  matchctl;   /* GBL: match control (flags bits 4-6) */
	uint32_t vbn;        /* 0 when absent (12-byte form) */
	uint32_t ident;      /* GBL */
	char     gblnam[44]; /* GBL */
};

struct iaf_info {
	uint32_t gfixoff;    /* G^ reference fixups */
	uint32_t dotadroff;  /* .ADDRESS reference fixups (0: none) */
	uint32_t chgprtoff;  /* protection change fixups */
	uint32_t shlstoff;   /* shareable image list */
	uint32_t shrimgcnt;  /* including entry 0, "this image" */
};

#define IAF_FIXED_SIZE  0x40u
#define IAF_SHL_SIZE    0x40u
#define IAF_SHL_NAME    0x18u

static inline uint16_t ihd_w(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t ihd_l(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static inline int ihd_ascic(const uint8_t *p, unsigned long avail, char *out,
			    unsigned long outsz)
{
	if (avail < 1)
		return -1;
	unsigned long n = p[0];
	if (n + 1 > avail || n + 1 > outsz)
		return -1;
	for (unsigned long i = 0; i < n; i++)
		out[i] = (char)p[1 + i];
	out[n] = '\0';
	return 0;
}

/* 0 ok; -1 not an OpenVMS VAX image; -2 malformed or unobserved. */
static inline int ihd_parse_header(const uint8_t *h, unsigned long len, struct ihd_info *o)
{
	if (len < IHD_BLOCK)
		return -1;
	if (h[0x0C] != '0' || h[0x0D] != '2' || h[0x0E] != '0' || h[0x0F] != '5')
		return -1;
	o->size      = ihd_w(h + 0x00);
	o->activoff  = ihd_w(h + 0x02);
	o->symdbgoff = ihd_w(h + 0x04);
	o->imgidoff  = ihd_w(h + 0x06);
	o->hdrblkcnt = h[0x10];
	o->imgtype   = h[0x11];
	o->lnkflags  = ihd_w(h + 0x20);
	o->matchctl  = h[0x23];
	o->ident     = ihd_l(h + 0x24);
	o->iafva     = ihd_l(h + 0x2C);
	if (!o->hdrblkcnt || o->hdrblkcnt > IHD_MAX_HDRBLKS ||
	    (unsigned long)o->hdrblkcnt * IHD_BLOCK > len)
		return -2;
	if (o->imgtype != IHD_K_EXE && o->imgtype != IHD_K_LIM)
		return -2;
	unsigned long hl = (unsigned long)o->hdrblkcnt * IHD_BLOCK;
	if (o->size < 0x30 || o->size >= hl || o->activoff < 0x30 ||
	    (unsigned long)o->activoff + 12 > hl)
		return -2;
	for (int k = 0; k < 3; k++)
		o->tfr[k] = ihd_l(h + o->activoff + 4 * k);
	o->imgnam[0] = '\0';
	if (o->imgidoff && (unsigned long)o->imgidoff + 40 <= hl)
		(void)ihd_ascic(h + o->imgidoff, 40, o->imgnam, sizeof o->imgnam);
	return 0;
}

/* ISDs follow the fixed header (at `size`); a zero size word ends the list,
 * 0xFFFF continues in the next header block. 1 = got one, 0 = end, -1 bad. */
static inline int ihd_next_isd(const uint8_t *h, const struct ihd_info *info,
			       unsigned long *off, struct ihd_isd *d)
{
	unsigned long hl = (unsigned long)info->hdrblkcnt * IHD_BLOCK;
	for (;;) {
		unsigned long o = *off;
		if (o + 2 > hl)
			return 0;
		uint16_t sz = ihd_w(h + o);
		if (sz == 0xFFFF) {
			*off = (o / IHD_BLOCK + 1) * IHD_BLOCK;
			continue;
		}
		if (sz == 0)
			return 0;
		if (sz < 12 || o + sz > hl)
			return -1;
		const uint8_t *p = h + o;
		uint32_t vpnpfc = ihd_l(p + 4), fl = ihd_l(p + 8);
		d->size = sz;
		d->pagcnt = ihd_w(p + 2);
		d->va = (vpnpfc & 0x3FFFFFu) * IHD_PAGE;   /* region bits included (P1: 0x3FFFEC -> 0x7FFFD800) */
		d->flags = fl & 0xFFFFFFu;
		d->type = (uint8_t)(fl >> 24);
		d->matchctl = (uint8_t)((d->flags >> 4) & 7u);
		d->vbn = sz >= 16 ? ihd_l(p + 12) : 0;
		d->ident = 0;
		d->gblnam[0] = '\0';
		if (d->flags & ISD_M_GBL) {
			if (sz < 21)
				return -1;
			d->ident = ihd_l(p + 16);
			if (ihd_ascic(p + 20, sz - 20, d->gblnam, sizeof d->gblnam) < 0)
				return -1;
		}
		*off = o + sz;
		return 1;
	}
}

static inline int iaf_parse(const uint8_t *iaf, unsigned long len, struct iaf_info *o)
{
	if (len < IAF_FIXED_SIZE)
		return -1;
	o->gfixoff   = ihd_l(iaf + 0x0C);
	o->dotadroff = ihd_l(iaf + 0x10);
	o->chgprtoff = ihd_l(iaf + 0x14);
	o->shlstoff  = ihd_l(iaf + 0x18);
	o->shrimgcnt = ihd_l(iaf + 0x1C);
	uint32_t offs[] = { o->gfixoff, o->dotadroff, o->chgprtoff, o->shlstoff };
	for (unsigned i = 0; i < 4; i++)
		if (offs[i] && (offs[i] < IAF_FIXED_SIZE || offs[i] >= len))
			return -1;
	if (!o->shrimgcnt || o->shrimgcnt > 64 ||
	    (unsigned long)o->shlstoff + (unsigned long)o->shrimgcnt * IAF_SHL_SIZE > len)
		return -1;
	return 0;
}

/* Name of shareable list entry i (entry 0 is the image itself: ""). */
static inline int iaf_shl_name(const uint8_t *iaf, const struct iaf_info *o, unsigned i,
			       char *out, unsigned long outsz)
{
	if (i >= o->shrimgcnt)
		return -1;
	return ihd_ascic(iaf + o->shlstoff + i * IAF_SHL_SIZE + IAF_SHL_NAME,
			 IAF_SHL_SIZE - IAF_SHL_NAME, out, outsz);
}

/* A reference fixup list: groups { count, shareable index, cell[count] }
 * ended by a zero count. For G^ fixups the cells themselves are in the list
 * (cb gets the cell's offset within the fixup section and its value, the
 * routine's transfer-vector offset); for .ADDRESS fixups each value is the
 * offset, from the image base, of a longword to which the target's base is
 * added. Returns the number of cells, or -1 when malformed. */
static inline long iaf_walk_refs(const uint8_t *iaf, unsigned long len, uint32_t off,
				 uint32_t shrimgcnt,
				 int (*cb)(void *, uint32_t shl, uint32_t celloff, uint32_t value),
				 void *arg)
{
	long n = 0;
	if (!off)
		return 0;
	unsigned long p = off;
	for (;;) {
		if (p + 4 > len)
			return -1;
		uint32_t cnt = ihd_l(iaf + p);
		if (cnt == 0)
			return n;
		if (p + 8 + (unsigned long)cnt * 4 > len)
			return -1;
		uint32_t shl = ihd_l(iaf + p + 4);
		if (shl >= shrimgcnt)
			return -1;
		for (uint32_t k = 0; k < cnt; k++) {
			uint32_t co = (uint32_t)(p + 8 + k * 4);
			if (cb(arg, shl, co, ihd_l(iaf + co)) < 0)
				return -1;
			n++;
		}
		p += 8 + (unsigned long)cnt * 4;
	}
}

/* Protection changes: { count, count * { offset from image base, page count
 * (word), PRT$C code (word) } }. */
static inline long iaf_walk_chgprt(const uint8_t *iaf, unsigned long len,
				   const struct iaf_info *o,
				   void (*cb)(void *, uint32_t off, uint32_t pages, uint32_t prt),
				   void *arg)
{
	if (!o->chgprtoff)
		return 0;
	unsigned long p = o->chgprtoff;
	if (p + 4 > len)
		return -1;
	uint32_t cnt = ihd_l(iaf + p);
	if (cnt > 256 || p + 4 + (unsigned long)cnt * 8 > len)
		return -1;
	for (uint32_t k = 0; k < cnt; k++) {
		const uint8_t *e = iaf + p + 4 + k * 8;
		cb(arg, ihd_l(e), ihd_w(e + 4), ihd_w(e + 6));
	}
	return (long)cnt;
}

#endif /* OVMX_IMGACT_IHD_H */
