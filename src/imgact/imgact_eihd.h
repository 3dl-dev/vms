/*
 * imgact_eihd.h -- reader for the OpenVMS Alpha native image format (EIHD),
 * rd vms-3b3f: an .EXE LINKed on real OpenVMS Alpha activates unchanged on
 * OVMX/Alpha.
 *
 * Header-only and freestanding (no libc): pure computation over byte buffers,
 * shared by IMGACT.EXE (arch/alpha) and the host unit test
 * (test/test_eihd_parse.c), which runs it over the real lab-built images in
 * tests/native-images/alpha/.
 *
 * Clean-room (AGENTS.md): every offset below was derived from OBSERVED
 * behavior only -- images we assembled and linked ourselves from our own
 * MACRO-32 sources on the lab OpenVMS Alpha V8.4 node, their LINK/MAP/FULL
 * maps, ANALYZE/IMAGE reports and DUMP output, correlated byte for byte
 * (tests/lab/captures/native-image-alpha-20261008/README.md). The field NAMES
 * are the ones ANALYZE/IMAGE prints (EIHD$..., EISD$..., EIAF$...). No VSI/HPE
 * source, header file or binary was read. Anything a captured image did not
 * exercise is refused (the caller fails the activation honestly), never
 * guessed.
 */
#ifndef OVMX_IMGACT_EIHD_H
#define OVMX_IMGACT_EIHD_H

#include <stdint.h>

#define EIHD_BLOCK          512u
#define EIHD_MAJORID        3u      /* "image format major id: 3, minor id: 0" */
#define EIHD_MINORID        0u
#define EIHD_K_EXE          1u      /* image type: executable (EIHD$K_EXE)     */
#define EIHD_K_LIM          2u      /* image type: shareable  (EIHD$K_LIM)     */
#define EIHD_MAX_HDRBLKS    16u

/* EISD flags (ANALYZE/IMAGE bit numbers). */
#define EISD_M_GBL          (1u << 0)
#define EISD_M_CRF          (1u << 1)
#define EISD_M_DZRO         (1u << 2)
#define EISD_M_WRT          (1u << 3)
#define EISD_M_FIXUPVEC     (1u << 6)
#define EISD_M_VECTOR       (1u << 8)
#define EISD_M_EXE          (1u << 11)

/* EISD section types seen in the captured images. */
#define EISD_K_NORMAL       0u
#define EISD_K_PRVFXD       2u
#define EISD_K_SHRPIC       3u
#define EISD_K_USRSTACK     253u

/* Match control (GSMATCH) as an ISD and the shareable header carry it. */
#define EIHD_MATCH_ALL      0u
#define EIHD_MATCH_EQUAL    1u      /* ISD$K_MATEQU */
#define EIHD_MATCH_LEQUAL   2u      /* ISD$K_MATLEQ */

/* PRT$C_ protection codes seen in protection-change fixups. */
#define EIHD_PRT_UREW       13u
#define EIHD_PRT_UR         15u

/* A transfer address whose high longword is all ones names an entry of the
 * SYS$PUBLIC_VECTORS symbol vector by its byte offset (SYS$IMGSTA, the first
 * transfer address of every image LINKed with traceback). */
#define EIHD_TFR_SYSVEC_HI  0xFFFFFFFFu

struct eihd_info {
	uint32_t size;           /* EIHD$L_SIZE                               */
	uint32_t isdoff;         /* offset of the first image section descr.  */
	uint32_t activoff;       /* offset of the activation (transfer) data  */
	uint32_t imgidoff;       /* offset of the image identification        */
	uint32_t imgtype;        /* EIHD_K_EXE / EIHD_K_LIM                   */
	uint32_t hdrblkcnt;      /* header block count                        */
	uint32_t lnkflags;       /* linker flags                              */
	uint32_t ident;          /* GSMATCH ident: major<<24 | minor          */
	uint32_t matchctl;       /* GSMATCH match control                     */
	uint32_t symvec_size;    /* symbol vector size in bytes               */
	uint64_t iafva;          /* fixup section virtual address             */
	uint64_t symvva;         /* symbol vector virtual address             */
	uint64_t tfr[3];         /* first..third transfer address             */
	uint64_t inishr;         /* shareable initialization (refused if set) */
	char     imgnam[40];     /* image name, NUL-terminated                */
};

struct eihd_isd {
	uint32_t size;           /* descriptor length                          */
	uint32_t secsize;        /* section byte count                         */
	uint64_t va;             /* base virtual address                       */
	uint32_t flags;          /* EISD_M_*                                   */
	uint32_t vbn;            /* base VBN (0: not file-backed)              */
	uint8_t  matchctl;
	uint8_t  type;           /* EISD_K_*                                   */
	uint32_t ident;          /* global section ident (GBL only)            */
	char     gblnam[44];     /* global section name (GBL only)             */
};

/* Image activator fixup section (EIAF) table offsets, relative to the EIAF. */
struct eiaf_info {
	uint32_t flags;          /* bit 0: EIAF$V_SHR                          */
	uint32_t qrelfixoff;     /* quadword relocations                       */
	uint32_t lrelfixoff;     /* longword relocations                       */
	uint32_t qdotadroff;     /* not seen in any captured image: refused    */
	uint32_t ldotadroff;     /* not seen: refused                          */
	uint32_t codeadroff;     /* not seen: refused                          */
	uint32_t lpfixoff;       /* linkage pair reference fixups              */
	uint32_t chgprtoff;      /* protection change fixups                   */
	uint32_t shlstoff;       /* shareable image list                       */
	uint32_t shrimgcnt;      /* number of shareable images                 */
	uint32_t shlextra;
	uint32_t lppsbfixoff;    /* not seen: refused                          */
};

#define EIAF_FIXED_SIZE     0x58u
#define EIAF_SHL_SIZE       0x40u   /* one shareable list entry            */
#define EIAF_SHL_NAME       0x18u   /* counted name within an entry        */

static inline uint32_t eihd_l(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t eihd_q(const uint8_t *p)
{
	return (uint64_t)eihd_l(p) | ((uint64_t)eihd_l(p + 4) << 32);
}

/* Copy a counted (ASCIC) string of at most `max` characters. */
static inline int eihd_ascic(const uint8_t *p, unsigned long avail,
			     char *out, unsigned long outsz)
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

/*
 * Parse the fixed image header. `hdr` holds the first `len` bytes of the file
 * (at least one block). Returns 0 on success, -1 when the file is not an
 * OpenVMS Alpha image (major/minor id), -2 when it claims to be one but is
 * malformed or uses a feature no captured image exercised.
 */
static inline int eihd_parse_header(const uint8_t *hdr, unsigned long len,
				    struct eihd_info *o)
{
	if (len < EIHD_BLOCK)
		return -1;
	if (eihd_l(hdr + 0x00) != EIHD_MAJORID || eihd_l(hdr + 0x04) != EIHD_MINORID)
		return -1;
	o->size        = eihd_l(hdr + 0x08);
	o->isdoff      = eihd_l(hdr + 0x0C);
	o->activoff    = eihd_l(hdr + 0x10);
	o->imgidoff    = eihd_l(hdr + 0x18);
	o->iafva       = eihd_q(hdr + 0x20);
	o->symvva      = eihd_q(hdr + 0x28);
	o->imgtype     = eihd_l(hdr + 0x34);
	o->hdrblkcnt   = eihd_l(hdr + 0x4C);
	o->lnkflags    = eihd_l(hdr + 0x50);
	o->ident       = eihd_l(hdr + 0x54);
	o->matchctl    = eihd_l(hdr + 0x5C);
	o->symvec_size = eihd_l(hdr + 0x60);
	if (o->hdrblkcnt < 1 || o->hdrblkcnt > EIHD_MAX_HDRBLKS ||
	    (unsigned long)o->hdrblkcnt * EIHD_BLOCK > len)
		return -2;
	if (o->imgtype != EIHD_K_EXE && o->imgtype != EIHD_K_LIM)
		return -2;
	unsigned long hlen = (unsigned long)o->hdrblkcnt * EIHD_BLOCK;
	if (o->isdoff < 0x68 || o->isdoff >= hlen ||
	    o->activoff < 0x68 || o->activoff + 0x30 > hlen)
		return -2;
	/* Activation data: size (0x30 observed), spare, three transfer
	 * addresses, a fourth slot, then the shareable-initialization address. */
	const uint8_t *a = hdr + o->activoff;
	if (eihd_l(a) < 0x30)
		return -2;
	o->tfr[0] = eihd_q(a + 0x08);
	o->tfr[1] = eihd_q(a + 0x10);
	o->tfr[2] = eihd_q(a + 0x18);
	o->inishr = eihd_q(a + 0x28);
	o->imgnam[0] = '\0';
	/* Image identification: two ids, the link time, then the counted name. */
	if (o->imgidoff >= 0x68 && o->imgidoff + 0x10 + 40 <= hlen)
		(void)eihd_ascic(hdr + o->imgidoff + 0x10, 40, o->imgnam,
				 sizeof o->imgnam);
	return 0;
}

/*
 * Walk the image section descriptors. `*off` starts at info->isdoff and is
 * advanced past each descriptor. A descriptor whose first longword is all
 * ones ends the current header block (the list continues in the next one); a
 * zero size ends the list. Returns 1 with *d filled, 0 at the end, -1 if the
 * list is malformed.
 */
static inline int eihd_next_isd(const uint8_t *hdr, const struct eihd_info *info,
				unsigned long *off, struct eihd_isd *d)
{
	unsigned long hlen = (unsigned long)info->hdrblkcnt * EIHD_BLOCK;
	for (;;) {
		unsigned long o = *off;
		if (o + 4 > hlen)
			return 0;
		if (eihd_l(hdr + o) == 0xFFFFFFFFu) {
			o = (o / EIHD_BLOCK + 1) * EIHD_BLOCK;
			*off = o;
			continue;
		}
		if (o + 12 > hlen)
			return -1;
		uint32_t sz = eihd_l(hdr + o + 0x08);
		if (sz == 0)
			return 0;
		if (sz < 0x24 || o + sz > hlen)
			return -1;
		const uint8_t *p = hdr + o;
		d->size     = sz;
		d->secsize  = eihd_l(p + 0x0C);
		d->va       = eihd_q(p + 0x10);
		d->flags    = eihd_l(p + 0x18);
		d->vbn      = eihd_l(p + 0x1C);
		d->matchctl = p[0x21];
		d->type     = p[0x22];
		d->ident    = 0;
		d->gblnam[0] = '\0';
		if (d->flags & EISD_M_GBL) {
			if (sz < 0x29)
				return -1;
			d->ident = eihd_l(p + 0x24);
			if (eihd_ascic(p + 0x28, sz - 0x28, d->gblnam,
				       sizeof d->gblnam) < 0)
				return -1;
		}
		*off = o + sz;
		return 1;
	}
}

/* Parse the fixed part of the image activator fixup section. */
static inline int eiaf_parse(const uint8_t *iaf, unsigned long len,
			     struct eiaf_info *o)
{
	if (len < EIAF_FIXED_SIZE)
		return -1;
	o->flags       = eihd_l(iaf + 0x1C);
	o->qrelfixoff  = eihd_l(iaf + 0x20);
	o->lrelfixoff  = eihd_l(iaf + 0x24);
	o->qdotadroff  = eihd_l(iaf + 0x28);
	o->ldotadroff  = eihd_l(iaf + 0x2C);
	o->codeadroff  = eihd_l(iaf + 0x30);
	o->lpfixoff    = eihd_l(iaf + 0x34);
	o->chgprtoff   = eihd_l(iaf + 0x38);
	o->shlstoff    = eihd_l(iaf + 0x3C);
	o->shrimgcnt   = eihd_l(iaf + 0x40);
	o->shlextra    = eihd_l(iaf + 0x44);
	o->lppsbfixoff = eihd_l(iaf + 0x50);
	uint32_t offs[] = { o->qrelfixoff, o->lrelfixoff, o->lpfixoff,
			    o->chgprtoff, o->shlstoff };
	for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++)
		if (offs[i] && (offs[i] < EIAF_FIXED_SIZE || offs[i] >= len))
			return -1;
	if (o->shrimgcnt > 64 ||
	    (o->shrimgcnt &&
	     (unsigned long)o->shlstoff + (unsigned long)o->shrimgcnt * EIAF_SHL_SIZE > len))
		return -1;
	return 0;
}

/* Fixup kinds no captured image exercised: refuse rather than guess. */
static inline int eiaf_has_unsupported(const struct eiaf_info *o)
{
	return o->qdotadroff || o->ldotadroff || o->codeadroff ||
	       o->lppsbfixoff || o->shlextra;
}

/* The counted name of shareable list entry `i`. */
static inline int eiaf_shl_name(const uint8_t *iaf, unsigned long len,
				const struct eiaf_info *o, unsigned i,
				char *out, unsigned long outsz)
{
	if (i >= o->shrimgcnt)
		return -1;
	(void)len;   /* eiaf_parse bounded the whole list against len */
	unsigned long e = (unsigned long)o->shlstoff + (unsigned long)i * EIAF_SHL_SIZE;
	if (iaf[e + 0x10] != EIAF_SHL_SIZE)     /* observed entry-size byte */
		return -1;
	return eihd_ascic(iaf + e + EIAF_SHL_NAME, EIAF_SHL_SIZE - EIAF_SHL_NAME,
			  out, outsz);
}

/*
 * Relocation tables (quadword or longword): a list of records
 *   { bit count, base offset, bitmap[bit count / 8] }
 * ended by a zero bit count. Bit i set means the cell at base + i * width is
 * relocated. Calls cb(arg, offset) for every set bit. The bit count was 64 in
 * every captured record; any count not a multiple of 64 is refused. Returns
 * the number of cells, or -1 when the table is malformed.
 */
static inline long eiaf_walk_rel(const uint8_t *iaf, unsigned long len,
				 uint32_t tableoff, unsigned width,
				 void (*cb)(void *, uint32_t), void *arg)
{
	long n = 0;
	if (!tableoff)
		return 0;
	unsigned long o = tableoff;
	for (;;) {
		if (o + 8 > len)
			return -1;
		uint32_t bits = eihd_l(iaf + o);
		uint32_t base = eihd_l(iaf + o + 4);
		if (bits == 0)
			return n;
		if (bits % 64)
			return -1;
		unsigned long nbytes = bits / 8;
		if (o + 8 + nbytes > len)
			return -1;
		for (uint32_t b = 0; b < bits; b++)
			if (iaf[o + 8 + b / 8] & (1u << (b % 8))) {
				cb(arg, base + b * width);
				n++;
			}
		o += 8 + nbytes;
	}
}

/*
 * Linkage pair reference fixups: a list of groups
 *   { count, shareable list index, offset[count] }
 * ended by a zero count. Calls cb(arg, shl_index, offset) per linkage pair.
 * Returns the number of linkage pairs, or -1 when malformed.
 */
static inline long eiaf_walk_lp(const uint8_t *iaf, unsigned long len,
				const struct eiaf_info *o,
				int (*cb)(void *, uint32_t, uint32_t), void *arg)
{
	long n = 0;
	if (!o->lpfixoff)
		return 0;
	unsigned long p = o->lpfixoff;
	for (;;) {
		if (p + 4 > len)
			return -1;
		uint32_t cnt = eihd_l(iaf + p);
		if (cnt == 0)
			return n;
		if (p + 8 + (unsigned long)cnt * 4 > len)
			return -1;
		uint32_t shl = eihd_l(iaf + p + 4);
		if (shl >= o->shrimgcnt)
			return -1;
		for (uint32_t k = 0; k < cnt; k++) {
			if (cb(arg, shl, eihd_l(iaf + p + 8 + k * 4)) < 0)
				return -1;
			n++;
		}
		p += 8 + (unsigned long)cnt * 4;
	}
}

/*
 * Protection change fixups: { count, count * { quad offset, byte count,
 * PRT$C code } }. Calls cb(arg, offset, bytes, prt) per entry. Returns the
 * number of entries, or -1 when malformed.
 */
static inline long eiaf_walk_chgprt(const uint8_t *iaf, unsigned long len,
				    const struct eiaf_info *o,
				    void (*cb)(void *, uint64_t, uint32_t, uint32_t),
				    void *arg)
{
	if (!o->chgprtoff)
		return 0;
	unsigned long p = o->chgprtoff;
	if (p + 4 > len)
		return -1;
	uint32_t cnt = eihd_l(iaf + p);
	if (cnt > 256 || p + 4 + (unsigned long)cnt * 16 > len)
		return -1;
	for (uint32_t k = 0; k < cnt; k++) {
		const uint8_t *e = iaf + p + 4 + k * 16;
		cb(arg, eihd_q(e), eihd_l(e + 8), eihd_l(e + 12));
	}
	return (long)cnt;
}

/* GSMATCH: does a shareable whose header ident is `have` satisfy a consumer
 * linked against `want` under match control `ctl`? */
static inline int eihd_gsmatch_ok(uint32_t ctl, uint32_t want, uint32_t have)
{
	uint32_t wmaj = want >> 24, wmin = want & 0xFFFFFFu;
	uint32_t hmaj = have >> 24, hmin = have & 0xFFFFFFu;
	switch (ctl) {
	case EIHD_MATCH_ALL:    return 1;
	case EIHD_MATCH_EQUAL:  return hmaj == wmaj && hmin == wmin;
	case EIHD_MATCH_LEQUAL: return hmaj == wmaj && hmin >= wmin;
	default:                return 0;
	}
}

/* The global section name the linker records for a shareable is its name
 * followed by "_" and a three-digit sequence number (LIBRTL -> "LIBRTL_001"). */
static inline int eihd_gblnam_is(const char *gblnam, const char *shl)
{
	unsigned long i = 0;
	while (shl[i] && gblnam[i] == shl[i])
		i++;
	if (shl[i] != '\0' || gblnam[i] != '_')
		return 0;
	for (unsigned k = 1; k <= 3; k++)
		if (gblnam[i + k] < '0' || gblnam[i + k] > '9')
			return 0;
	return gblnam[i + 4] == '\0';
}

#endif /* OVMX_IMGACT_EIHD_H */
