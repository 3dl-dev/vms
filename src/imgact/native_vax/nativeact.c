/*
 * nativeact.c -- NATIVEACT.EXE, the OpenVMS VAX native image activator on
 * OVMX/VAX (rd vms-b869, the VAX rung of vms-3b3f).
 *
 * An .EXE LINKed on real OpenVMS VAX (IHD format, src/imgact/imgact_ihd.h) is
 * not something the NetBSD kernel can load (ENOEXEC), so DCL runs this
 * program with the image spec as its first argument, in the same process, as
 * VMS hands every image to its activator. It
 *   - reads the image off the ODS-2 volume through the executive ACP (no
 *     POSIX read of the image, INV-6), as IMGACT does;
 *   - maps the image's sections at their P0 addresses. A VAX image starts at
 *     0x200, inside virtual page 0, which NetBSD refuses to map. Baron's ruling
 *     (2026-10-09, rd vms-b869): page 0 is allowed per process, only through
 *     this activation path, so the executive grants it to this process only
 *     (VMS_IOCTL_NATIVE_PAGE0) and NetBSD's null-page defence stays on for
 *     every other process;
 *   - builds the P1 system service vector (0x7FFEDE00...: an image calls
 *     SYS$QIOW at 7FFEDE00) and the transfer vectors of the run-time library
 *     images it calls (LIBRTL, DECC$SHR), each entry the target routine's own
 *     entry mask and a JMP past it -- the form of a VAX transfer vector entry,
 *     so the routine runs as if CALLed directly. The targets are OVMX's own
 *     routines, linked into this program; on VAX they take their arguments in
 *     the VMS forms already (32-bit addresses, 8-byte descriptors, CALLS/CALLG
 *     argument lists);
 *   - activates the image's own native shareables (relocating them by their
 *     .ADDRESS fixups), checks every GSMATCH against the ident the image was
 *     linked with, fills its G^ reference cells, applies its protection
 *     changes, and calls its transfer addresses; the returned condition value
 *     goes to SYS$EXIT, which records it in the executive.
 * Every address and offset comes from images LINKed on the lab VAX V7.3 node
 * and that node's own ANALYZE/IMAGE and link maps
 * (docs/oracle/vax73-symvec-offsets.txt, tests/lab/captures/
 * native-image-vax-20261009/). An entry OVMX does not provide, a fixup kind no
 * captured image uses, or an unreadable header fails the activation with the
 * real activator's status, never a pretend success.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "descrip.h"
#include "lib$routines.h"
#include "starlet.h"
#include "vms_kif.h"
#include "lnmdef.h"
#include "imgact_acp.h"
#include "imgact_eihd.h"   /* shared: GSMATCH, shareable file resolution */
#include "imgact_ihd.h"
#include "vms_ioctl.h"

/* Statuses the real activator returns (observed on the lab VAX V7.3 node). */
#define STS_INHIB_MSG      0x10000000u
#define CLI_IMAGEFNF       0x000388B2u   /* -CLI-E-IMAGEFNF       */
#define SS_SHRIDMISMAT     0x000020BCu   /* -SYSTEM-F-SHRIDMISMAT */
#define IMGACT_BADHDR      0x004D8C84u   /* -IMGACT-F-BADHDR      */
#define SS_UNSUPPORTED     0x00000E4Cu   /* -SYSTEM-F-UNSUPPORTED */

#define NBPG          4096u              /* NetBSD/vax page */
#define PG_DOWN(x)    ((x) & ~(uintptr_t)(NBPG - 1))
#define PG_UP(x)      PG_DOWN((x) + NBPG - 1)
#define P1VEC_BASE    0x7FFEDE00u
#define P1VEC_PAGE    0x7FFED000u

/* ---- the activator's host primitives for the ACP reader (imgact_acp.c) ---- */
int imgact_acp_dev_open(void) { return open("/dev/vms", O_RDWR | O_CLOEXEC); }
void imgact_acp_dev_close(int fd) { close(fd); }
long imgact_acp_dev_ioctl(int fd, unsigned long req, void *arg)
{
	return ioctl(fd, req, arg) < 0 ? -errno : 0;
}

/* ---- failures, reported as VMS reports them under DCL ---- */
static void fail(const char *image, const char *l2, const char *d2,
		 const char *l3, uint32_t cond)
{
	fflush(stdout);
	fprintf(stderr, "%%DCL-W-ACTIMAGE, error activating image %s\n", image);
	if (l2)
		fprintf(stderr, "%s%s\n", l2, d2 ? d2 : "");
	if (l3)
		fprintf(stderr, "%s\n", l3);
	fflush(stderr);
	sys$exit(STS_INHIB_MSG | cond);
	_exit(44);
}

static void fail_hdr(const char *image, const char *spec)
{
	fail(image, "-CLI-E-IMGNAME, image file ", spec,
	     "-IMGACT-F-BADHDR, an error was discovered in the image header",
	     IMGACT_BADHDR);
}

static void fail_notimpl(const char *image, const char *spec, const char *what)
{
	char l3[300];
	snprintf(l3, sizeof l3, "-SYSTEM-F-UNSUPPORTED, unsupported operation or function\n"
		 "-IMGACT-I-NOTIMPL, not implemented by OVMX: %s", what);
	fail(image, "-CLI-E-IMGNAME, image file ", spec, l3, SS_UNSUPPORTED);
}

/* ---- tracing: OVMX$NATIVEACT_TRACE defined (any table in LNM$FILE_DEV)
 * prints each activation step and each vector call to SYS$ERROR, so a
 * native image that stops can be followed from the console. ---- */
static int g_trace = -1;
static int tracing(void)
{
	if (g_trace < 0) {
		char tr[64];
		uint16_t rl = 0;
		$DESCRIPTOR(tab, "LNM$FILE_DEV");
		$DESCRIPTOR(ln, "OVMX$NATIVEACT_TRACE");
		struct item_list_3 il[2];
		memset(il, 0, sizeof il);
		il[0].buflen = sizeof tr;
		il[0].item_code = LNM$_STRING;
		il[0].bufaddr = tr;
		il[0].retlen = &rl;
		g_trace = (sys$trnlnm(0, &tab, &ln, 0, il) & 1) ? 1 : 0;
	}
	return g_trace;
}
#define TRACE(...) do { if (tracing()) { fprintf(stderr, "%%NATIVEACT-I-TRACE, " __VA_ARGS__); \
		fputc('\n', stderr); fflush(stderr); } } while (0)

/* ---- the routines an image reaches through the P1 vector and the RTL
 * transfer vectors: address/offset -> OVMX routine ---- */
static int imgsta(void *xfervec, void *cli, void *hdr, void *imgfile,
		  unsigned linkflag, unsigned cliflag);

typedef void (*rtn_t)(void);
struct vent { uint32_t at; rtn_t fn; const char *name; };

/* The vector targets: OVMX's own services; the image's VAX argument forms
 * (32-bit descriptors and addresses) are the native forms here. */
static int v_qiow(uint32_t efn, uint16_t chan, uint32_t func, void *iosb, void *astadr,
		  uint32_t astprm, void *p1, uint32_t p2, uint32_t p3, uint32_t p4,
		  uint32_t p5, uint32_t p6)
{
	TRACE("SYS$QIOW efn=%u chan=%u func=%#x p1=%p p2=%u", efn, chan, func, p1, p2);
	int st = (int)sys$qiow(efn, chan, func, iosb, (void (*)(uint32_t))astadr, astprm, p1,
			       p2, p3, p4, p5, p6);
	TRACE("SYS$QIOW -> %#x", st);
	return st;
}
static int v_assign(void *devnam, uint16_t *chan, uint32_t acmode, void *mbxnam,
		    uint32_t flags)
{
	TRACE("SYS$ASSIGN");
	int st = (int)sys$assign(devnam, chan, acmode, mbxnam, flags);
	TRACE("SYS$ASSIGN -> %#x chan=%u", st, chan ? *chan : 0);
	return st;
}
static void v_exit(uint32_t code)
{
	TRACE("SYS$EXIT %#x", code);
	fflush(stdout);
	sys$exit(code);
}

/* P1 system service vector (absolute addresses). */
static const struct vent p1vec[] = {
	{ 0x7FFEDE00, (rtn_t)v_qiow,     "SYS$QIOW"   },
	{ 0x7FFEDE50, (rtn_t)v_assign,   "SYS$ASSIGN" },
	{ 0x7FFEDF40, (rtn_t)v_exit,     "SYS$EXIT"   },
	{ 0x7FFEDF68, (rtn_t)imgsta,     "SYS$IMGSTA" },
};

/* Run-time library transfer vectors (offset from the image base). */
struct rtlimg { const char *name; const struct vent *e; unsigned n; };
static int v_put_output(void *msg)
{
	TRACE("LIB$PUT_OUTPUT");
	int st = (int)lib$put_output(msg);
	TRACE("LIB$PUT_OUTPUT -> %#x", st);
	return st;
}
static const struct vent librtl_tv[] = {
	{ 0x478, (rtn_t)v_put_output, "LIB$PUT_OUTPUT" },
};
static const struct vent decc_tv[] = {
	{ 0x380, (rtn_t)printf, "DECC$DPRINTF" },   /* D_float printf: NetBSD/vax's own */
	{ 0x3A0, (rtn_t)puts,   "DECC$PUTS"    },
};
static const struct rtlimg rtlimgs[] = {
	{ "LIBRTL",   librtl_tv, sizeof librtl_tv / sizeof librtl_tv[0] },
	{ "DECC$SHR", decc_tv,   sizeof decc_tv / sizeof decc_tv[0] },
};

/* One transfer vector entry: the routine's entry mask, then JMP @#rtn+2. */
static void put_entry(uint8_t *p, rtn_t fn)
{
	uint32_t tgt = (uint32_t)(uintptr_t)fn + 2;
	memcpy(p, (const void *)fn, 2);       /* entry mask */
	p[2] = 0x17;                          /* JMP */
	p[3] = 0x9F;                          /* @#absolute */
	memcpy(p + 4, &tgt, 4);
}

/* SYS$IMGSTA: the first transfer address of an image LINKed with traceback.
 * It calls the next transfer address with the same activation list (OVMX
 * establishes no traceback handler, as on Alpha). */
typedef int (*xfer_t)(void *, void *, void *, void *, unsigned, unsigned);
static int imgsta(void *xfervec, void *cli, void *hdr, void *imgfile,
		  unsigned linkflag, unsigned cliflag)
{
	uint32_t *v = xfervec;
	if (!v || !v[1])
		return SS$_BADPARAM;
	xfer_t next = (xfer_t)(uintptr_t)v[1];
	void *a0 = v[2] ? (void *)&v[1] : (void *)(uintptr_t)v[1];
	TRACE("SYS$IMGSTA -> transfer to %#x", v[1]);
	int st = next(a0, cli, hdr, imgfile, linkflag, cliflag);
	TRACE("the image returned %#x", st);
	return st;
}

/* ---- the images of this activation ---- */
struct img {
	char      name[40];
	char      spec[256];
	uint8_t   hdr[IHD_MAX_HDRBLKS * IHD_BLOCK];
	struct ihd_info h;
	uintptr_t base;          /* run-time address of linked address 0 */
	uint32_t  lo, hi;
	uint8_t  *iaf;
	unsigned long iaflen;
	struct iaf_info a;
	int       shl[64];       /* shareable list index -> activation table */
};

struct shr {
	char      name[40];
	int       img;           /* native shareable: index in imgs[], else -1 */
	const struct rtlimg *rtl;
	uintptr_t base;
	uint32_t  ident;
};
static struct img imgs[8];
static int nimgs;
static struct shr shrs[16];
static int nshrs;
static const char *g_sysdev;

static int acp_open_spec(struct imgact_acp_file *f, const char *volpath)
{
	return (imgact_acp_open(f, g_sysdev, volpath) & 1) ? 0 : -1;
}

/* The VMS spec a shareable name designates: its logical-name translation (a
 * bare name is retried while it translates), with SYS$SHARE: and .EXE as
 * defaults. Mapped to the volume for SYS$SHARE/SYS$LIBRARY/SYS$SYSTEM only. */
static int shl_file(const char *name, char *vol, size_t vsz, char *spec, size_t ssz)
{
	char cur[256];
	snprintf(cur, sizeof cur, "%s", name);
	for (int d = 0; d < 10 && !strpbrk(cur, ":[<.;"); d++) {
		char tr[256];
		$DESCRIPTOR(tab, "LNM$FILE_DEV");
		struct dsc$descriptor_s ln = { (uint16_t)strlen(cur), DSC$K_DTYPE_T,
					       DSC$K_CLASS_S, cur };
		uint16_t rl = 0;
		struct item_list_3 il[2];
		memset(il, 0, sizeof il);
		il[0].buflen = sizeof tr - 1;
		il[0].item_code = LNM$_STRING;
		il[0].bufaddr = tr;
		il[0].retlen = &rl;
		if (!(sys$trnlnm(0, &tab, &ln, 0, il) & 1))
			break;
		tr[rl] = '\0';
		snprintf(cur, sizeof cur, "%s", tr);
	}
	char path[256], full[256];
	int r = eihd_spec_to_file(cur, "/vms/SYS0/SYSCOMMON/SYSLIB/",
				      "/vms/SYS0/SYSCOMMON/SYSEXE/", path, sizeof path,
				      full, sizeof full);
	snprintf(spec, ssz, "%s", r < 0 ? name : full);
	snprintf(vol, vsz, "%s", r < 0 ? "" : path);
	return r;
}

static int activate_shr(const char *name, const char *by);

/* Fill the G^ reference cells (in the fixup section) and the .ADDRESS
 * longwords of image m with the bases of the shareables they name. */
struct fixctx { struct img *m; int bad; const char *why; };

static uintptr_t shr_value(struct img *m, uint32_t shl, uint32_t value, int *ok)
{
	*ok = 1;
	if (shl == 0)
		return m->base + value;
	struct shr *s = &shrs[m->shl[shl]];
	if (s->rtl) {
		for (unsigned k = 0; k < s->rtl->n; k++)
			if (s->rtl->e[k].at == value)
				return s->base + value;
		*ok = 0;
		return 0;
	}
	return s->base + value;
}

static int gfix_cb(void *arg, uint32_t shl, uint32_t co, uint32_t value)
{
	struct fixctx *c = arg;
	int ok;
	uintptr_t v = shr_value(c->m, shl, value, &ok);
	if (!ok) {
		static char why[96];
		snprintf(why, sizeof why, "transfer vector entry %%X'%08X' of %s", value,
			 shrs[c->m->shl[shl]].name);
		c->why = why;
		return -1;
	}
	uint32_t v32 = (uint32_t)v;
	memcpy(c->m->iaf + co, &v32, 4);
	return 0;
}

static int dotadr_cb(void *arg, uint32_t shl, uint32_t co, uint32_t value)
{
	struct fixctx *c = arg;
	(void)co;
	struct img *m = c->m;
	if (value < m->lo || value + 4 > m->hi)
		return -1;
	uint32_t cell;
	memcpy(&cell, (void *)(m->base + value), 4);
	uintptr_t add = shl == 0 ? m->base : shrs[m->shl[shl]].base;
	cell += (uint32_t)add;
	memcpy((void *)(m->base + value), &cell, 4);
	return 0;
}

static struct img *g_cp;
static void chgprt_cb(void *arg, uint32_t off, uint32_t pages, uint32_t prt)
{
	(void)arg;
	/* User-mode access: UW (4) leaves it writable, every read-only-to-user
	 * code makes it read-only. A NetBSD page holds eight VAX pages, so a
	 * page is made read-only only when the whole range covers it. */
	uintptr_t a = g_cp->base + off, z = a + (uintptr_t)pages * IHD_PAGE;
	if (prt == 4)
		return;
	uintptr_t pa = PG_UP(a), pz = PG_DOWN(z);
	if (pz > pa)
		mprotect((void *)pa, pz - pa, PROT_READ | PROT_EXEC);
}

/* Read, map and fix up image m (from `src`, already accessed). */
static void load(struct img *m, struct imgact_acp_file *src, int is_main)
{
	if (imgact_acp_pread(src, m->hdr, IHD_BLOCK, 0) != IHD_BLOCK)
		fail_hdr(m->name, m->spec);
	int rc = ihd_parse_header(m->hdr, IHD_BLOCK, &m->h);
	if (rc == 0 && m->h.hdrblkcnt > 1) {
		unsigned long hl = (unsigned long)m->h.hdrblkcnt * IHD_BLOCK;
		if (imgact_acp_pread(src, m->hdr, hl, 0) != (long)hl)
			fail_hdr(m->name, m->spec);
		rc = ihd_parse_header(m->hdr, hl, &m->h);
	}
	if (rc < 0 || m->h.imgtype != (is_main ? IHD_K_EXE : IHD_K_LIM))
		fail_hdr(m->name, m->spec);

	unsigned long off = m->h.size;
	struct ihd_isd d;
	m->lo = 0xFFFFFFFFu;
	m->hi = 0;
	while ((rc = ihd_next_isd(m->hdr, &m->h, &off, &d)) == 1) {
		if ((d.flags & ISD_M_GBL) || d.type == ISD_K_USRSTACK)
			continue;
		if (d.va >= 0x40000000u)
			fail_notimpl(m->name, m->spec, "a P1 image section");
		if (d.va < m->lo) m->lo = d.va;
		if (d.va + d.pagcnt * IHD_PAGE > m->hi) m->hi = d.va + d.pagcnt * IHD_PAGE;
	}
	if (rc < 0 || m->lo == 0xFFFFFFFFu)
		fail_hdr(m->name, m->spec);

	uintptr_t span = PG_UP(m->hi) - PG_DOWN(m->lo);
	void *map;
	if (is_main) {
		/* Page 0 for this process only, granted by the executive. */
		if (PG_DOWN(m->lo) == 0) {
			if (!(vms_kif_native_page0() & 1))
				fail_notimpl(m->name, m->spec,
					     "virtual page 0 (the executive refused it)");
		}
		map = mmap((void *)PG_DOWN(m->lo), span, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		if (map != (void *)PG_DOWN(m->lo))
			fail_notimpl(m->name, m->spec, "mapping the image at its link address");
		TRACE("%s mapped at %p (%lu bytes)", m->name, map, (unsigned long)span);
		m->base = 0;
	} else {
		map = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (map == MAP_FAILED || (uintptr_t)map + span > 0x40000000u)
			fail_notimpl(m->name, m->spec, "mapping the shareable in P0");
		m->base = (uintptr_t)map - PG_DOWN(m->lo);
	}

	off = m->h.size;
	m->iaf = 0;
	while (ihd_next_isd(m->hdr, &m->h, &off, &d) == 1) {
		if ((d.flags & ISD_M_GBL) || d.type == ISD_K_USRSTACK)
			continue;
		unsigned long n = (unsigned long)d.pagcnt * IHD_PAGE;
		if (!(d.flags & ISD_M_DZRO)) {
			if (d.vbn <= m->h.hdrblkcnt ||
			    imgact_acp_pread(src, (void *)(m->base + d.va), n,
					     (long)(d.vbn - 1) * IHD_BLOCK) < 0)
				fail_hdr(m->name, m->spec);
		}
		if (m->h.iafva >= d.va && m->h.iafva < d.va + n) {
			m->iaf = (uint8_t *)(m->base + m->h.iafva);
			m->iaflen = d.va + n - m->h.iafva;
		}
	}
	if (!m->iaf || iaf_parse(m->iaf, m->iaflen, &m->a) < 0)
		fail_hdr(m->name, m->spec);
	TRACE("%s: sections read, %u shareable(s)", m->name, m->a.shrimgcnt ? m->a.shrimgcnt - 1 : 0);

	/* Shareables (entry 0 is this image), GSMATCH-checked. */
	for (unsigned i = 1; i < m->a.shrimgcnt; i++) {
		char nm[40];
		if (iaf_shl_name(m->iaf, &m->a, i, nm, sizeof nm) < 0)
			fail_hdr(m->name, m->spec);
		int si = activate_shr(nm, m->name);
		m->shl[i] = si;
		off = m->h.size;
		while (ihd_next_isd(m->hdr, &m->h, &off, &d) == 1) {
			if (!(d.flags & ISD_M_GBL) || !eihd_gblnam_is(d.gblnam, nm))
				continue;
			if (!shrs[si].rtl &&
			    !eihd_gsmatch_ok(d.matchctl, d.ident, shrs[si].ident))
				fail(nm, "-CLI-E-IMGNAME, image file ", imgs[shrs[si].img].spec,
				     "-SYSTEM-F-SHRIDMISMAT, ident mismatch with shareable image",
				     SS_SHRIDMISMAT);
		}
	}

	struct fixctx c = { m, 0, 0 };
	if (iaf_walk_refs(m->iaf, m->iaflen, m->a.gfixoff, m->a.shrimgcnt, gfix_cb, &c) < 0) {
		if (c.why)
			fail_notimpl(m->name, m->spec, c.why);
		fail_hdr(m->name, m->spec);
	}
	if (iaf_walk_refs(m->iaf, m->iaflen, m->a.dotadroff, m->a.shrimgcnt, dotadr_cb, &c) < 0)
		fail_hdr(m->name, m->spec);
	g_cp = m;
	if (iaf_walk_chgprt(m->iaf, m->iaflen, &m->a, chgprt_cb, 0) < 0)
		fail_hdr(m->name, m->spec);
	TRACE("%s: fixups applied", m->name);
}

static int activate_shr(const char *name, const char *by)
{
	for (int i = 0; i < nshrs; i++)
		if (!strcmp(shrs[i].name, name))
			return i;
	if (nshrs >= (int)(sizeof shrs / sizeof shrs[0]))
		fail_notimpl(by, name, "this many shareable images");
	struct shr *s = &shrs[nshrs];
	snprintf(s->name, sizeof s->name, "%s", name);
	s->img = -1;
	s->rtl = 0;
	for (unsigned k = 0; k < sizeof rtlimgs / sizeof rtlimgs[0]; k++)
		if (!strcmp(rtlimgs[k].name, name)) {
			/* An OVMX run-time library: its transfer vector, built here. */
			const struct rtlimg *r = &rtlimgs[k];
			uint32_t top = 0;
			for (unsigned j = 0; j < r->n; j++)
				if (r->e[j].at + 8 > top)
					top = r->e[j].at + 8;
			uint8_t *tv = mmap(NULL, PG_UP(top), PROT_READ | PROT_WRITE,
					   MAP_PRIVATE | MAP_ANON, -1, 0);
			if (tv == MAP_FAILED)
				fail_notimpl(by, name, "mapping a transfer vector");
			for (unsigned j = 0; j < r->n; j++)
				put_entry(tv + r->e[j].at, r->e[j].fn);
			mprotect(tv, PG_UP(top), PROT_READ | PROT_EXEC);
			s->rtl = r;
			s->base = (uintptr_t)tv;
			return nshrs++;
		}
	/* A native shareable image. */
	char vol[256], spec[256];
	int r = shl_file(name, vol, sizeof vol, spec, sizeof spec);
	if (r > 0)
		fail_notimpl(by, spec, "activating a shareable image outside SYS$SHARE or SYS$SYSTEM");
	struct imgact_acp_file f;
	if (!vol[0] || acp_open_spec(&f, vol) < 0)
		fail(name, "-CLI-E-IMAGEFNF, image file not found ", spec, 0, CLI_IMAGEFNF);
	if (nimgs >= (int)(sizeof imgs / sizeof imgs[0]))
		fail_notimpl(by, name, "this many native shareable images");
	int si = nshrs++;
	struct img *m = &imgs[nimgs];
	s->img = nimgs++;
	snprintf(m->name, sizeof m->name, "%s", name);
	snprintf(m->spec, sizeof m->spec, "%s", spec);
	load(m, &f, 0);
	imgact_acp_close(&f);
	shrs[si].base = m->base;
	shrs[si].ident = m->h.ident;
	return si;
}

/* The staged copy DCL ran names the image on the volume: a SYS$SYSTEM image
 * by its file name, any other by its FID (ovmx_layout.h's staging shapes). */
static int open_main(struct imgact_acp_file *f, const char *staged, char *spec, size_t ssz,
		     char *name, size_t nsz)
{
	const char *b = strrchr(staged, '/');
	b = b ? b + 1 : staged;
	snprintf(spec, ssz, "SYS$SYSTEM:%s", b);
	size_t k = 0;
	for (; b[k] && b[k] != '.' && k + 1 < nsz; k++)
		name[k] = b[k];
	name[k] = '\0';
	const char *fid = strstr(staged, "/FID/");
	if (fid) {
		unsigned n, s, r, x;
		if (sscanf(fid + 5, "%u.%u.%u.%u/", &n, &s, &r, &x) == 4) {
			snprintf(spec, ssz, "%s", b);
			return (imgact_acp_open_fid(f, g_sysdev, (uint16_t)n, (uint16_t)s,
						    (uint8_t)r, (uint8_t)x) & 1) ? 0 : -1;
		}
	}
	char vol[512];
	snprintf(vol, sizeof vol, "/vms/SYS0/SYSCOMMON/SYSEXE/%s", b);
	return acp_open_spec(f, vol);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "%%NATIVEACT-F-NOIMAGE, no image to activate\n");
		return 44;
	}
	TRACE("activating %s", argv[1]);
	g_sysdev = getenv("OVMX_SYSDEVICE");
	if (!g_sysdev || !*g_sysdev)
		g_sysdev = "DUA0:";

	struct img *m = &imgs[nimgs++];
	struct imgact_acp_file f;
	if (open_main(&f, argv[1], m->spec, sizeof m->spec, m->name, sizeof m->name) < 0)
		fail(m->name, "-CLI-E-IMAGEFNF, image file not found ", m->spec, 0, CLI_IMAGEFNF);
	TRACE("opened %s", m->spec);
	load(m, &f, 1);
	imgact_acp_close(&f);
	TRACE("loaded %#x..%#x base %#lx", m->lo, m->hi, (unsigned long)m->base);

	/* The P1 system service vector, below the NetBSD user stack top. */
	uint8_t *pv = mmap((void *)P1VEC_PAGE, NBPG, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (pv != (uint8_t *)P1VEC_PAGE)
		fail_notimpl(m->name, m->spec, "the P1 system service vector page");
	memset(pv, 0, NBPG);
	for (unsigned k = 0; k < sizeof p1vec / sizeof p1vec[0]; k++)
		put_entry(pv + (p1vec[k].at - P1VEC_PAGE), p1vec[k].fn);
	mprotect(pv, NBPG, PROT_READ | PROT_EXEC);
	TRACE("P1 vector page mapped at %p", (void *)pv);

	/* The transfer vector: SYS$IMGSTA, then the image's own. */
	static uint32_t xvec[4];
	unsigned n = 0;
	for (unsigned k = 0; k < 3 && m->h.tfr[k]; k++) {
		uint32_t t = m->h.tfr[k];
		if (t >= P1VEC_BASE && t < P1VEC_PAGE + NBPG) {
			int known = 0;
			for (unsigned j = 0; j < sizeof p1vec / sizeof p1vec[0]; j++)
				known |= p1vec[j].at == t;
			if (!known)
				fail_notimpl(m->name, m->spec, "a transfer address in the P1 vector");
		}
		xvec[n++] = t;
	}
	xvec[n] = 0;
	if (!n)
		fail_hdr(m->name, m->spec);

	struct dsc$descriptor_s img = { (uint16_t)strlen(m->spec), DSC$K_DTYPE_T,
					DSC$K_CLASS_S, m->spec };
	xfer_t first = (xfer_t)(uintptr_t)xvec[0];
	TRACE("transfer vector %#x %#x %#x", xvec[0], xvec[1], xvec[2]);
	int cond = first(n > 1 ? (void *)xvec : (void *)(uintptr_t)xvec[0], 0, m->hdr, &img,
			 m->h.lnkflags, 1);
	fflush(stdout);
	sys$exit((uint32_t)cond);
	return 0;
}
