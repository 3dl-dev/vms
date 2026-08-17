/*
 * IMGACT.EXE — OVMX image activator (aarch64 + x86_64).
 *
 * Beads vms-913.2 (aarch64 core) and vms-913.11 (x86_64). Design contract:
 * docs/design-image-activation.md.
 *
 * IMGACT.EXE is a static-PIE, freestanding binary registered as the ELF
 * PT_INTERP for all OVMX executables. On exec(), the kernel loads IMGACT.EXE,
 * which then performs the OpenVMS image-activation role in userspace: it maps
 * the target's shareable images (DT_NEEDED), processes relocations, resolves
 * inter-image symbols, sets up TLS, runs constructors, and transfers control
 * to the executable's entry point. On any hard failure it emits an OpenVMS
 * message (%IMGACT-F-...) to SYS$ERROR (fd 2) and exits with a nonzero code.
 *
 * ---------------------------------------------------------------------------
 * Attribution: the ELF loading/relocation structure (segment mapping,
 * DT_HASH symbol lookup, RELATIVE/GLOB_DAT/JUMP_SLOT/TLSDESC handling, TLS
 * block layout) is adapted in spirit from musl libc's ldso/dynlink.c, which
 * is MIT-licensed and is named by the design spec as the reference
 * implementation. No VSI/HPE OpenVMS source or binary was referenced; the
 * VMS-facing behavior (message formats, search path, GSMATCH scaffolding)
 * comes from the OVMX design spec and public VMS documentation only.
 * ---------------------------------------------------------------------------
 *
 * Scope: aarch64 + x86_64 core activator + minimal proof harness. All
 * architecture-specific detail (syscall ABI, relocation numbers, TLS variant,
 * entry/TP/TLSDESC assembly) lives under arch/<arch>/; the loader body is
 * architecture-independent. Out of scope and deferred to sibling beads:
 * GSMATCH enforcement (913.4), the INSTALL known-image DB (913.5), CMake
 * OVMX_IMGACT mode (913.3). GNU_HASH is not yet supported; OVMX images are
 * linked --hash-style=sysv (DT_HASH) for now.
 */

#include <elf.h>

#if defined(__aarch64__)
#  include "arch/aarch64/imgact_arch.h"
#elif defined(__x86_64__)
#  include "arch/x86_64/imgact_arch.h"
#elif defined(__alpha__)
#  include "arch/alpha/imgact_arch.h"
#else
#  error "IMGACT.EXE: unsupported architecture (aarch64, x86_64, alpha only)"
#endif
#include "ovmx_image.h"   /* OVMX symbol-vector image format (LINK.EXE) */
#include "ovmx_symvec.h"  /* shared resolver + GSMATCH (bead vms-8d5)  */
#include "known_images.h" /* Known Image DB lookup (bead vms-913.5; wired vms-30d) */
#include "imgact_prodreg.h" /* publish resident producers into LIBVMS$SHR (vms-db2) */
#include "imgact_acp.h"   /* image reads over the executive Files-11 ACP (vms-3e8e) */

#ifndef AT_EXECFN
#define AT_EXECFN 31
#endif

#ifndef O_RDONLY
#define O_RDONLY 0
#endif
#ifndef O_RDWR
#define O_RDWR 2
#endif

/* The system disk the activator reads images from. On real OpenVMS this is the
 * discovered SYS$SYSDEVICE; IMGACT hardcodes the boot unit exactly as it used
 * to hardcode the "/vms" mount point (IMGACT_FALLBACK_SYSLIB). Resolving
 * SYS$SYSDEVICE dynamically is a follow-up (it is a discovered logical, epic
 * vms-47d). The QEMU harness maps the boot volume to DKA0:. */
#ifndef IMGACT_ACP_SYSDEVICE
#define IMGACT_ACP_SYSDEVICE "DKA0:"
#endif

/* --------------------------------------------------------------------------
 * Freestanding syscall layer (no libc; IMGACT.EXE is -nostdlib).
 *
 * The raw syscall primitive (syscall6) and the per-arch syscall numbers are
 * provided by arch/<arch>/imgact_arch.h; the typed wrappers below are shared.
 * -------------------------------------------------------------------------- */

static long sys_openat(const char *path, int flags)
{
	/* AT_FDCWD = -100 */
	return syscall6(SYS_openat, -100, (long)path, flags, 0, 0, 0);
}
static long sys_close(int fd) { return syscall6(SYS_close, fd, 0, 0, 0, 0, 0); }
/* NOTE (vms-3e8e): image-file reads no longer use pread(2) on a /vms POSIX
 * path -- they ride the executive Files-11 ACP (IO$_READVBLK, imgact_acp.c).
 * SYS_pread64 stays defined for the arch headers' completeness; there is no
 * sys_pread() wrapper because nothing in the activator reads a file that way. */
static long sys_write(int fd, const void *buf, unsigned long n)
{
	return syscall6(SYS_write, fd, (long)buf, n, 0, 0, 0);
}
static void *sys_mmap(void *addr, unsigned long len, int prot, int flags,
		      int fd, long off)
{
	return (void *)syscall6(SYS_mmap, (long)addr, len, prot, flags, fd, off);
}
static long sys_mprotect(void *addr, unsigned long len, int prot)
{
	return syscall6(SYS_mprotect, (long)addr, len, prot, 0, 0, 0);
}
static void sys_exit(int code)
{
	syscall6(SYS_exit_group, code, 0, 0, 0, 0, 0);
	for (;;) { }
}
static long sys_munmap(void *addr, unsigned long len)
{
	return syscall6(SYS_munmap, (long)addr, len, 0, 0, 0, 0);
}
static long sys_ioctl(int fd, unsigned long req, void *arg)
{
	return syscall6(SYS_ioctl, fd, (long)req, (long)arg, 0, 0, 0);
}

/* --------------------------------------------------------------------------
 * Files-11 ACP host primitives (vms-3e8e). imgact_acp.c reaches /dev/vms
 * through these three functions; here they are the freestanding syscall
 * backings (the QEMU test provides libc-backed versions of the same symbols).
 * -------------------------------------------------------------------------- */
int imgact_acp_dev_open(void)
{
	long fd = sys_openat("/dev/vms", O_RDWR);
	return fd < 0 ? -1 : (int)fd;
}
void imgact_acp_dev_close(int fd)
{
	sys_close(fd);
}
long imgact_acp_dev_ioctl(int fd, unsigned long req, void *arg)
{
	return sys_ioctl(fd, req, arg);
}

#ifndef PROT_READ
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
/* MAP_ANONYMOUS is NOT the same bit across Linux ports: x86_64/aarch64 (and
 * most others) use 0x20, but Alpha (like mips/sparc/parisc) uses 0x10 --
 * confirmed against alpha-linux-gnu's own <asm/mman.h> and empirically (a
 * plain 0x20 anonymous mmap() on Alpha returned -EBADF: the kernel read it
 * as a file-backed mapping request against the fd=-1 sentinel). vms-e11. */
#if defined(__alpha__)
#define MAP_ANONYMOUS 0x10
#else
#define MAP_ANONYMOUS 0x20
#endif
#endif
#define MAP_FAILED ((void *)-1)

/* --------------------------------------------------------------------------
 * Freestanding string / memory helpers.
 * -------------------------------------------------------------------------- */

void *memcpy(void *d, const void *s, unsigned long n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;
	while (n--) *dp++ = *sp++;
	return d;
}
void *memset(void *d, int c, unsigned long n)
{
	unsigned char *dp = d;
	while (n--) *dp++ = (unsigned char)c;
	return d;
}
static unsigned long xstrlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (unsigned long)(p - s);
}
static int xstrcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static char *xstrcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++)) { }
	return r;
}
static char *xstrcat(char *d, const char *s)
{
	char *p = d;
	while (*p) p++;
	xstrcpy(p, s);
	return d;
}

/* --------------------------------------------------------------------------
 * Known Image Database libc shim (bead vms-30d).
 *
 * known_images.c (bead vms-913.5) is a small, self-contained POSIX C module
 * -- open()/fstat()/mmap()/munmap()/close()/string.h -- built normally
 * everywhere else in the tree (see the hosted `known_images` CMake target
 * and its unit/integration tests). IMGACT.EXE is -nostdlib/-ffreestanding
 * and links against nothing, so this file's Makefile/CMakeLists.txt compile
 * known_images.c as an extra translation unit of the IMGACT.EXE binary
 * itself; its libc calls come out as undefined external symbols that must
 * be satisfied within this link. These are the freestanding definitions
 * that satisfy them, built on the same raw syscall6() primitive as the rest
 * of this file (memcpy/memset above already follow this pattern). Only the
 * subset known_images.c actually calls is implemented; signatures use
 * plain integer/pointer types (no dependency on host <fcntl.h>/<sys/stat.h>/
 * <sys/mman.h> struct layouts -- fstat()'s statbuf is passed straight
 * through to the kernel, untouched by this file).
 * -------------------------------------------------------------------------- */

#if defined(__aarch64__)
#  define IMGACT_SYS_FSTAT 80
#elif defined(__x86_64__)
#  define IMGACT_SYS_FSTAT 5
#elif defined(__alpha__)
#  define IMGACT_SYS_FSTAT 91
#endif

int open(const char *path, int flags, ...)
{
	return (int)sys_openat(path, flags);
}
int close(int fd)
{
	return (int)sys_close(fd);
}
int fstat(int fd, void *statbuf)
{
	return syscall6(IMGACT_SYS_FSTAT, fd, (long)statbuf, 0, 0, 0, 0) < 0 ? -1 : 0;
}
void *mmap(void *addr, unsigned long len, int prot, int flags, int fd, long off)
{
	return sys_mmap(addr, len, prot, flags, fd, off);
}
int munmap(void *addr, unsigned long len)
{
	return sys_munmap(addr, len) < 0 ? -1 : 0;
}
char *strncpy(char *dst, const char *src, unsigned long n)
{
	unsigned long i = 0;
	for (; i < n && src[i]; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = 0;
	return dst;
}
int strncmp(const char *a, const char *b, unsigned long n)
{
	for (unsigned long i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if (ca != cb)
			return (int)ca - (int)cb;
		if (ca == 0)
			return 0;
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * OpenVMS diagnostic output (SYS$ERROR).
 *
 * Messages follow the VMS %FACILITY-severity-IDENT, text format documented in
 * docs/design-image-activation.md §11. Fatal messages are followed by a
 * nonzero process exit (the spec calls for SIGABRT; a nonzero exit satisfies
 * the same "activation failed" contract and keeps IMGACT dependency-free).
 * -------------------------------------------------------------------------- */

static void eputs(const char *s) { sys_write(2, s, xstrlen(s)); }

static void vms_fatal(const char *ident, const char *text, const char *detail)
{
	char line[512];
	line[0] = 0;
	xstrcat(line, "%IMGACT-F-");
	xstrcat(line, ident);
	xstrcat(line, ", ");
	xstrcat(line, text);
	xstrcat(line, "\n");
	eputs(line);
	if (detail) {
		line[0] = 0;
		xstrcat(line, "-IMGACT-I-FILENAME, file: ");
		xstrcat(line, detail);
		xstrcat(line, "\n");
		eputs(line);
	}
}

/* IMGACT condition-value severities: fatal exits use a nonzero status. */
#define IMGACT_EXIT_FAIL 44

static void die_imgnotfnd(const char *soname)
{
	vms_fatal("IMGNOTFND", "image file not found", soname);
	sys_exit(IMGACT_EXIT_FAIL);
}
static void die_imgfmterr(const char *name)
{
	vms_fatal("IMGFMTERR", "image format error", name);
	sys_exit(IMGACT_EXIT_FAIL);
}
static void die_undsym(const char *name)
{
	vms_fatal("UNDSYM", "undefined symbol in image", name);
	sys_exit(IMGACT_EXIT_FAIL);
}
static void die_mapfail(const char *name)
{
	vms_fatal("MAPFAIL", "failed to map image into memory", name);
	sys_exit(IMGACT_EXIT_FAIL);
}
static void die_reloc(const char *name)
{
	vms_fatal("RELOCERR", "unsupported relocation in image", name);
	sys_exit(IMGACT_EXIT_FAIL);
}
static void die_tlserr(const char *name)
{
	vms_fatal("TLSERR", "TLS initialization failed", name);
	sys_exit(IMGACT_EXIT_FAIL);
}

/* --------------------------------------------------------------------------
 * Loaded-object model.
 * -------------------------------------------------------------------------- */

#define MAX_OBJS   32
#define PAGE_SIZE  4096UL
#define PAGE_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_UP(x)   PAGE_DOWN((x) + PAGE_SIZE - 1)
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((unsigned long)(a) - 1))

/* SysV .hash section entry width: 4 bytes (Elf32_Word) everywhere except
 * Alpha, which uses 8 bytes (Elf64_Xword) -- see imgact_arch.h. */
#ifdef IMGACT_HASH_XWORD
typedef Elf64_Xword imgact_hashword_t;
#else
typedef Elf64_Word imgact_hashword_t;
#endif

struct obj {
	char           name[64];
	unsigned long  base;          /* load bias */
	Elf64_Dyn     *dyn;
	Elf64_Sym     *symtab;
	const char    *strtab;
	imgact_hashword_t *hash;      /* DT_HASH */
	Elf64_Rela    *rela;
	unsigned long  relasz;
	Elf64_Rela    *jmprel;
	unsigned long  pltrelsz;
	void         (*init)(void);
	Elf64_Addr    *init_array;
	unsigned long  init_arraysz;
	/* TLS (PT_TLS) */
	int            has_tls;
	unsigned long  tls_image;     /* runtime addr of .tdata init image */
	unsigned long  tls_filesz;
	unsigned long  tls_memsz;
	unsigned long  tls_align;
	unsigned long  tls_offset;    /* offset from TP */
	int            relocated;
};

static struct obj g_objs[MAX_OBJS];
static int        g_nobjs;
static Elf64_auxv_t *g_auxv;      /* saved for __getauxval builtin */
static char        **g_envp;      /* process envp — the C-RTL __init_libc arg */
static char         *g_argv0;     /* process argv[0] (program name for musl)  */

/* --------------------------------------------------------------------------
 * Interpreter-exported ("builtin") symbols.
 *
 * IMGACT.EXE is the root of the global symbol namespace: symbols that a
 * shareable image imports but that no loaded image defines resolve here. This
 * is the same role ld-musl plays as libc provider (design spec §1). For
 * vms-913.2 the builtin set is intentionally small — it proves the
 * fall-through-to-interpreter resolution path. Wiring the full musl libc
 * symbol set is part of the OVMX_IMGACT build integration (bead 913.3).
 * -------------------------------------------------------------------------- */

static unsigned long imgact_getauxval(unsigned long type)
{
	Elf64_auxv_t *a = g_auxv;
	if (a)
		for (; a->a_type != AT_NULL; a++)
			if (a->a_type == type)
				return a->a_un.a_val;
	return 0;
}
/* GCC's runtime helpers call __getauxval (double underscore); musl exports
 * getauxval. IMGACT bridges the gap (design spec §8). */
unsigned long __getauxval(unsigned long type) { return imgact_getauxval(type); }

/* Probe symbols exercised by the vms-913.2 proof harness. */
static int imgact_probe(void) { return 0; }
int imgact_counter = 0;

#ifdef IMGACT_R_TLS_DTPMOD
/*
 * __tls_get_addr — General Dynamic TLS runtime resolver (bead vms-e11,
 * Alpha[A2]). Architectures with no TLSDESC (Alpha: GCC rejects
 * -mtls-dialect=desc/gnu2 outright) fall back to the classic model: the
 * compiler emits a JMP_SLOT-resolved call to __tls_get_addr(&tls_index),
 * where the two-word tls_index is filled by DTPMOD64 (runtime module id)
 * + DTPREL64 (module-relative offset) relocations -- both handled in
 * apply_rela() below, guarded on this same macro.
 *
 * IMGACT owns the runtime module numbering (it assigns each g_objs[]
 * module id = its 1-based array index at DTPMOD64-relocation time, below),
 * so no real DTV is needed: every module's TP-relative offset is already
 * fixed by assign_tls_offsets()/setup_tls() before any target/lib code that
 * could call __tls_get_addr() runs.
 */
struct tls_index { unsigned long ti_module; unsigned long ti_offset; };

static void *imgact_tls_get_addr(struct tls_index *ti)
{
	if (ti->ti_module < 1 || ti->ti_module > (unsigned long)g_nobjs)
		die_tlserr("__tls_get_addr");
	struct obj *o = &g_objs[ti->ti_module - 1];
	char *tp = (char *)imgact_get_tp();
	return tp + o->tls_offset + ti->ti_offset;
}
void *__tls_get_addr(struct tls_index *ti) { return imgact_tls_get_addr(ti); }
#endif

struct builtin_sym { const char *name; unsigned long addr; };
static const struct builtin_sym g_builtins[] = {
	{ "imgact_probe",   (unsigned long)imgact_probe   },
	{ "imgact_counter", (unsigned long)&imgact_counter },
	{ "__getauxval",    (unsigned long)__getauxval    },
#ifdef IMGACT_R_TLS_DTPMOD
	{ "__tls_get_addr", (unsigned long)__tls_get_addr },
#endif
};
#define N_BUILTINS (sizeof(g_builtins) / sizeof(g_builtins[0]))

/* --------------------------------------------------------------------------
 * Symbol resolution.
 * -------------------------------------------------------------------------- */

#if defined(__alpha__)
/* Alpha has no integer-divide instruction (see the "division-free decimal"
 * note in tools/cross-alpha/ovmx-alpha-selftest.c); GCC normally lowers `%`
 * to a call to libgcc's __remqu, which this -nostdlib build doesn't link and
 * which uses a non-standard, undocumented-here register convention anyway
 * (confirmed by inspecting IMGACT.EXE's own disassembly: the plain `%`
 * operator left a live GLOB_DAT relocation against __remqu that IMGACT's
 * self_relocate() -- RELATIVE-only, by design -- never fills, so any call
 * would jump through a null GOT slot). Software binary long division sidesteps
 * both problems: it is normal, standard-calling-convention C, so it needs no
 * ABI assumption at all. */
static unsigned long umod64(unsigned long a, unsigned long b)
{
	unsigned long q = 0, r = 0;
	for (int i = 63; i >= 0; i--) {
		r = (r << 1) | ((a >> i) & 1UL);
		if (r >= b) {
			r -= b;
			q |= (1UL << i);
		}
	}
	(void)q;
	return r;
}
#else
static inline unsigned long umod64(unsigned long a, unsigned long b)
{
	return a % b;
}
#endif

static unsigned long elf_sysv_hash(const char *name)
{
	unsigned long h = 0, g;
	while (*name) {
		h = (h << 4) + (unsigned char)*name++;
		g = h & 0xf0000000UL;
		if (g)
			h ^= g >> 24;
		h &= ~g;
	}
	return h;
}

/* Find a *defined* symbol by name within one object via DT_HASH. */
static Elf64_Sym *obj_find(struct obj *o, const char *name, unsigned long hash)
{
	if (!o->hash || !o->symtab || !o->strtab)
		return 0;
	imgact_hashword_t  nbucket = o->hash[0];
	imgact_hashword_t *bucket  = o->hash + 2;
	imgact_hashword_t *chain   = bucket + nbucket;
	if (!nbucket)
		return 0;
	for (imgact_hashword_t i = bucket[umod64(hash, nbucket)]; i; i = chain[i]) {
		Elf64_Sym *s = &o->symtab[i];
		if (s->st_shndx == SHN_UNDEF)
			continue;
		if (xstrcmp(o->strtab + s->st_name, name) == 0)
			return s;
	}
	return 0;
}

struct symres {
	int            found;
	unsigned long  value;   /* resolved runtime address (non-TLS) */
	struct obj    *obj;     /* defining object (0 for builtins)   */
	Elf64_Sym     *sym;
};

/* Global-scope lookup: executable -> loaded images -> interpreter builtins. */
static struct symres resolve_sym(const char *name)
{
	struct symres r = { 0, 0, 0, 0 };
	unsigned long hash = elf_sysv_hash(name);
	for (int i = 0; i < g_nobjs; i++) {
		Elf64_Sym *s = obj_find(&g_objs[i], name, hash);
		if (s) {
			r.found = 1;
			r.obj   = &g_objs[i];
			r.sym   = s;
			r.value = g_objs[i].base + s->st_value;
			return r;
		}
	}
	for (unsigned long i = 0; i < N_BUILTINS; i++) {
		if (xstrcmp(g_builtins[i].name, name) == 0) {
			r.found = 1;
			r.value = g_builtins[i].addr;
			return r;
		}
	}
	return r;
}

/* --------------------------------------------------------------------------
 * ELF object mapping and dynamic-section parsing.
 * -------------------------------------------------------------------------- */

static void parse_dynamic(struct obj *o)
{
	unsigned long strtab = 0, symtab = 0, hash = 0;
	unsigned long rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
	unsigned long init = 0, init_array = 0, init_arraysz = 0;
	for (Elf64_Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
		switch (d->d_tag) {
		case DT_STRTAB:      strtab = d->d_un.d_ptr; break;
		case DT_SYMTAB:      symtab = d->d_un.d_ptr; break;
		case DT_HASH:        hash = d->d_un.d_ptr; break;
		case DT_RELA:        rela = d->d_un.d_ptr; break;
		case DT_RELASZ:      relasz = d->d_un.d_val; break;
		case DT_JMPREL:      jmprel = d->d_un.d_ptr; break;
		case DT_PLTRELSZ:    pltrelsz = d->d_un.d_val; break;
		case DT_INIT:        init = d->d_un.d_ptr; break;
		case DT_INIT_ARRAY:  init_array = d->d_un.d_ptr; break;
		case DT_INIT_ARRAYSZ:init_arraysz = d->d_un.d_val; break;
		default: break;
		}
	}
	/* Dynamic-section pointers are link-time vaddrs: apply the load bias. */
	o->strtab       = strtab ? (const char *)(o->base + strtab) : 0;
	o->symtab       = symtab ? (Elf64_Sym *)(o->base + symtab) : 0;
	o->hash         = hash ? (imgact_hashword_t *)(o->base + hash) : 0;
	o->rela         = rela ? (Elf64_Rela *)(o->base + rela) : 0;
	o->relasz       = relasz;
	o->jmprel       = jmprel ? (Elf64_Rela *)(o->base + jmprel) : 0;
	o->pltrelsz     = pltrelsz;
	o->init         = init ? (void (*)(void))(o->base + init) : 0;
	o->init_array   = init_array ? (Elf64_Addr *)(o->base + init_array) : 0;
	o->init_arraysz = init_arraysz;
}

/* Record PT_TLS geometry for an object from its program headers. */
static void scan_tls(struct obj *o, Elf64_Phdr *phdr, int phnum)
{
	for (int i = 0; i < phnum; i++) {
		if (phdr[i].p_type == PT_TLS) {
			o->has_tls    = 1;
			o->tls_image  = o->base + phdr[i].p_vaddr;
			o->tls_filesz = phdr[i].p_filesz;
			o->tls_memsz  = phdr[i].p_memsz;
			o->tls_align  = phdr[i].p_align ? phdr[i].p_align : 1;
			return;
		}
	}
}

/* --------------------------------------------------------------------------
 * Image source (vms-3e8e): every image-file read the activator does now rides
 * the executive Files-11 (ODS-2) ACP -- IO$_ACCESS + IO$_READVBLK over
 * /dev/vms (imgact_acp.c) -- NOT open()/pread() on a /vms POSIX path, the
 * passthrough the ACP pivot retires (docs/design-files11-acp-executive.md
 * §4.6). Fail-honest: if the boot volume is not ACP-mounted (SS$_NOSUCHDEV) or
 * the image is not on it (SS$_NOSUCHFILE) the open fails and the activator dies
 * with an honest %IMGACT message; there is NEVER a silent POSIX fallback
 * (CLAUDE.md Rule 9 / INV-6). imgsrc_pread() keeps pread(2) semantics so the
 * existing exact-count read checks below are unchanged.
 * -------------------------------------------------------------------------- */
struct imgsrc { struct imgact_acp_file f; };

static int imgsrc_open(struct imgsrc *s, const char *path)
{
	uint32_t st = imgact_acp_open(&s->f, IMGACT_ACP_SYSDEVICE, path);
	return (st & 1u) ? 0 : -1;
}
static long imgsrc_pread(struct imgsrc *s, void *buf, unsigned long n, long off)
{
	return imgact_acp_pread(&s->f, buf, n, off);
}
static void imgsrc_close(struct imgsrc *s)
{
	imgact_acp_close(&s->f);
}

/* Map a shareable image file into memory; fill base/dyn. Returns obj index. */
static struct obj *load_object(const char *soname, const char *path)
{
	if (g_nobjs >= MAX_OBJS)
		die_mapfail(soname);

	struct imgsrc src;
	if (imgsrc_open(&src, path) < 0)
		return 0;

	Elf64_Ehdr eh;
	if (imgsrc_pread(&src, &eh, sizeof(eh), 0) != (long)sizeof(eh) ||
	    eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
	    eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F') {
		imgsrc_close(&src);
		die_imgfmterr(soname);
	}

	Elf64_Phdr ph[32];
	if (eh.e_phnum > 32) {
		imgsrc_close(&src);
		die_imgfmterr(soname);
	}
	if (imgsrc_pread(&src, ph, (unsigned long)eh.e_phnum * eh.e_phentsize,
		      (long)eh.e_phoff) < 0) {
		imgsrc_close(&src);
		die_imgfmterr(soname);
	}

	/* Compute the contiguous virtual span across PT_LOAD segments. */
	unsigned long lo = ~0UL, hi = 0;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_vaddr < lo)
			lo = PAGE_DOWN(ph[i].p_vaddr);
		if (ph[i].p_vaddr + ph[i].p_memsz > hi)
			hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (lo == ~0UL) {
		imgsrc_close(&src);
		die_imgfmterr(soname);
	}
	unsigned long span = PAGE_UP(hi) - lo;

	/* Reserve the whole span R/W anonymous, then read segments into place
	 * via the ACP window (IO$_READVBLK). Anonymous memory is zero-filled,
	 * so .bss needs no explicit clear. (Read-then-place first cut, vms-3e8e;
	 * demand-page-through-the-window is the end state.) */
	void *map = sys_mmap(0, span, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) {
		imgsrc_close(&src);
		die_mapfail(soname);
	}
	unsigned long base = (unsigned long)map - lo;

	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
			continue;
		if (imgsrc_pread(&src, (void *)(base + ph[i].p_vaddr),
			      ph[i].p_filesz, (long)ph[i].p_offset) < 0) {
			imgsrc_close(&src);
			die_mapfail(soname);
		}
	}
	imgsrc_close(&src);

	/* Apply final segment protections. */
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		int prot = 0;
		if (ph[i].p_flags & PF_R) prot |= PROT_READ;
		if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
		if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
		unsigned long a = PAGE_DOWN(base + ph[i].p_vaddr);
		unsigned long z = PAGE_UP(base + ph[i].p_vaddr + ph[i].p_memsz);
		sys_mprotect((void *)a, z - a, prot);
	}

	struct obj *o = &g_objs[g_nobjs++];
	memset(o, 0, sizeof(*o));
	xstrcpy(o->name, soname);
	o->base = base;

	/* Locate PT_DYNAMIC and PT_TLS. */
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type == PT_DYNAMIC)
			o->dyn = (Elf64_Dyn *)(base + ph[i].p_vaddr);
	}
	if (!o->dyn)
		die_imgfmterr(soname);
	parse_dynamic(o);
	scan_tls(o, ph, eh.e_phnum);
	return o;
}

/* --------------------------------------------------------------------------
 * DT_NEEDED resolution and recursive load.
 * -------------------------------------------------------------------------- */

#define IMGACT_FALLBACK_SYSLIB   "/vms/SYS0/SYSCOMMON/SYSLIB"
#define IMGACT_KNOWN_IMAGES_DB   "/vms/SYS0/SYSCOMMON/SYSEXE/VMS$KNOWN_IMAGES.DAT"

static struct obj *find_loaded(const char *soname)
{
	for (int i = 0; i < g_nobjs; i++)
		if (xstrcmp(g_objs[i].name, soname) == 0)
			return &g_objs[i];
	return 0;
}

/* --------------------------------------------------------------------------
 * Known Image Database (Priority 1 of docs/design-image-activation.md §4).
 *
 * Lazily mmap(MAP_SHARED)'d on first DT_NEEDED lookup (bead vms-30d wiring
 * of the vms-913.5 module) so activations with no DT_NEEDED entries never
 * touch it. A missing/corrupt/absent database (-1) is cached, not retried
 * per soname -- every subsequent lookup falls straight through to the
 * Priority 2 SYS$SHARE fallback below.
 * -------------------------------------------------------------------------- */

static struct known_images_db g_known_db;
static int                    g_known_db_state; /* 0=untried 1=open -1=unavailable */

static const struct known_images_db *known_db(void)
{
	if (g_known_db_state == 0) {
		g_known_db_state =
			known_images_open(&g_known_db, IMGACT_KNOWN_IMAGES_DB) == 0 ? 1 : -1;
	}
	return g_known_db_state == 1 ? &g_known_db : 0;
}

/* Release the KFE mmap/fd once DT_NEEDED resolution is done; nothing past
 * this point in imgact_bootstrap() needs it, and the fd should not leak
 * into the activated program's descriptor table. */
static void known_db_shutdown(void)
{
	if (g_known_db_state == 1)
		known_images_close(&g_known_db);
	g_known_db_state = -1;
}

/* Resolve a SONAME to a path and map it. Search order per design spec §4:
 *   Priority 1: Known Image Database -- O(1) mmap'd hash lookup, no
 *               filesystem search at all on a hit (bead vms-30d).
 *   Priority 2: hardcoded SYS$SHARE fallback (vms-913.2; always available,
 *               even before the Known Image DB exists).
 * Priorities 3 (SYS$SHARE logical name, once the logical name tables are
 * executive-resident -- vms-a4b) and 4 (ELF RPATH) are not yet implemented. */
static struct obj *load_needed(const char *soname)
{
	struct obj *existing = find_loaded(soname);
	if (existing)
		return existing;

	char path[512];

	const struct known_images_db *kdb = known_db();
	const struct kfe_entry *kfe = kdb ? known_images_lookup(kdb, soname) : 0;
	if (kfe) {
		/* kfe->path is a fixed-size field that may not be NUL-terminated
		 * if a writer filled all 256 bytes; bound and terminate defensively
		 * (same idiom known_images.c itself uses for soname). */
		unsigned long n = sizeof(kfe->path);
		if (n >= sizeof(path))
			n = sizeof(path) - 1;
		memcpy(path, kfe->path, n);
		path[n] = 0;
	} else {
		path[0] = 0;
		xstrcat(path, IMGACT_FALLBACK_SYSLIB);
		xstrcat(path, "/");
		xstrcat(path, soname);
	}

	struct obj *o = load_object(soname, path);
	if (!o)
		die_imgnotfnd(soname);
	return o;
}

/* Walk an object's DT_NEEDED list, loading each dependency (breadth first). */
static void load_deps(struct obj *o)
{
	/* Snapshot the count: load_needed may append new objects. */
	for (Elf64_Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
		if (d->d_tag != DT_NEEDED)
			continue;
		const char *soname = o->strtab + d->d_un.d_val;
		struct obj *dep = load_needed(soname);
		(void)dep;
	}
}

/* --------------------------------------------------------------------------
 * TLS layout.
 *
 * Each TLS module is assigned a SIGNED TP-relative base offset in o->tls_offset
 * (stored in an unsigned long; two's-complement wraparound makes TP + offset
 * work for both signs). A variable at st_value within the module lives at
 * TP + tls_offset + st_value — the value a static TLSDESC descriptor returns.
 *
 *   Variant I  (aarch64, TLS_ABOVE_TP): TP is the block base; the TCB is
 *     reserved first (offsets positive), blocks grow upward.
 *   Variant II (x86_64): the TCB sits at/above TP (with a self-pointer at TP);
 *     blocks live below TP (offsets negative). g_tls_tp_off is the distance
 *     from the mmap base up to TP.
 * -------------------------------------------------------------------------- */

static unsigned long g_tls_total;    /* bytes to mmap for the whole TLS area */
static unsigned long g_tls_tp_off;   /* offset of TP within the mmap'd area  */

static void assign_tls_offsets(void)
{
#if IMGACT_TLS_VARIANT == 1
	unsigned long cursor = TLS_TCB_SIZE;   /* reserve TCB below the blocks */
	for (int i = 0; i < g_nobjs; i++) {
		struct obj *o = &g_objs[i];
		if (!o->has_tls)
			continue;
		unsigned long off = ALIGN_UP(cursor, o->tls_align);
		o->tls_offset = off;                       /* positive */
		cursor = off + o->tls_memsz;
	}
	g_tls_tp_off = 0;                              /* TP == mmap base */
	g_tls_total  = cursor;
#else
	/* Variant II: cumulative distance below TP (Drepper, variant 2). */
	unsigned long run = 0, maxalign = 16;
	for (int i = 0; i < g_nobjs; i++) {
		struct obj *o = &g_objs[i];
		if (!o->has_tls)
			continue;
		run = ALIGN_UP(run + o->tls_memsz, o->tls_align);
		o->tls_offset = (unsigned long)(-(long)run);   /* negative */
		if (o->tls_align > maxalign)
			maxalign = o->tls_align;
	}
	g_tls_tp_off = ALIGN_UP(run, maxalign);        /* TP above the blocks */
	g_tls_total  = g_tls_tp_off + TLS_TCB_SIZE;    /* + TCB above TP       */
#endif
}

/* Allocate the TLS block, copy per-module init images, program the TP. */
static void setup_tls(void)
{
	unsigned long len = PAGE_UP(g_tls_total);
	if (len == 0)
		return;
	void *area = sys_mmap(0, len, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		die_tlserr("TLS block");
	char *tp = (char *)area + g_tls_tp_off;

	for (int i = 0; i < g_nobjs; i++) {
		struct obj *o = &g_objs[i];
		if (!o->has_tls)
			continue;
		memcpy(tp + o->tls_offset, (void *)o->tls_image, o->tls_filesz);
		/* .tbss already zero (anonymous mapping). */
	}
#if IMGACT_TLS_VARIANT == 2
	*(void **)tp = tp;   /* self-pointer at %fs:0 (Variant II TCB) */
#endif
	imgact_set_tp(tp);
}

/* --------------------------------------------------------------------------
 * Relocation processing.
 * -------------------------------------------------------------------------- */

static void apply_rela(struct obj *o, Elf64_Rela *rela, unsigned long size)
{
	unsigned long n = size / sizeof(Elf64_Rela);
	for (unsigned long i = 0; i < n; i++) {
		Elf64_Rela *r = &rela[i];
		unsigned long type = ELF64_R_TYPE(r->r_info);
		unsigned long symi = ELF64_R_SYM(r->r_info);
		unsigned long *where = (unsigned long *)(o->base + r->r_offset);

		switch (type) {
		case IMGACT_R_RELATIVE:
			/* B + A (base-relative), identical on aarch64/x86_64. */
			*where = o->base + (unsigned long)r->r_addend;
			break;

		case IMGACT_R_GLOB_DAT:
		case IMGACT_R_JUMP_SLOT: {
			/* S (+ A). The x86_64 JUMP_SLOT/GLOB_DAT addend is 0,
			 * so the shared S + A form is correct on both arches. */
			const char *name = o->strtab + o->symtab[symi].st_name;
			struct symres res = resolve_sym(name);
			if (!res.found) {
				/* Weak undefined symbols resolve to 0. */
				if (ELF64_ST_BIND(o->symtab[symi].st_info)
				    == STB_WEAK) {
					*where = (unsigned long)r->r_addend;
					break;
				}
				die_undsym(name);
			}
			*where = res.value + (unsigned long)r->r_addend;
			break;
		}

#ifdef IMGACT_R_TLSDESC
		case IMGACT_R_TLSDESC: {
			/* Fill the 2-word descriptor: [resolver, TP-rel offset].
			 * o->tls_offset carries the module block's signed
			 * TP-relative base (positive on Variant I, negative on
			 * Variant II), so the same arithmetic serves both. */
			unsigned long arg;
			if (symi) {
				const char *name =
					o->strtab + o->symtab[symi].st_name;
				struct symres res = resolve_sym(name);
				if (!res.found || !res.obj || !res.obj->has_tls)
					die_tlserr(name);
				arg = res.obj->tls_offset + res.sym->st_value
				      + (unsigned long)r->r_addend;
			} else {
				/* Local TLS symbol: offset within this module. */
				arg = o->tls_offset + (unsigned long)r->r_addend;
			}
			where[0] = (unsigned long)__tlsdesc_static;
			where[1] = arg;
			break;
		}
#endif

#ifdef IMGACT_R_TLS_DTPMOD
		case IMGACT_R_TLS_DTPMOD: {
			/* General Dynamic model (bead vms-e11, Alpha[A2]): fill
			 * the runtime module id __tls_get_addr() indexes g_objs[]
			 * with -- IMGACT's own numbering (1-based array index),
			 * assigned here exactly like a real ld.so assigns one at
			 * load time. */
			if (symi) {
				const char *name =
					o->strtab + o->symtab[symi].st_name;
				struct symres res = resolve_sym(name);
				if (!res.found || !res.obj || !res.obj->has_tls)
					die_tlserr(name);
				*where = (unsigned long)(res.obj - g_objs) + 1;
			} else {
				/* Local TLS symbol: this module's own id. */
				*where = (unsigned long)(o - g_objs) + 1;
			}
			break;
		}
		case IMGACT_R_TLS_DTPREL: {
			/* General Dynamic model: module-relative offset (S + A,
			 * matching the resolve_sym()-based cases' "value already
			 * carries the module-internal offset" contract). */
			if (symi) {
				const char *name =
					o->strtab + o->symtab[symi].st_name;
				struct symres res = resolve_sym(name);
				if (!res.found)
					die_undsym(name);
				*where = res.sym->st_value
					 + (unsigned long)r->r_addend;
			} else {
				*where = (unsigned long)r->r_addend;
			}
			break;
		}
#endif

		case IMGACT_R_ABS64: {
			const char *name = o->strtab + o->symtab[symi].st_name;
			struct symres res = resolve_sym(name);
			if (!res.found)
				die_undsym(name);
			*where = res.value + (unsigned long)r->r_addend;
			break;
		}

		default:
			die_reloc(o->name);
		}
	}
}

static void relocate_obj(struct obj *o)
{
	if (o->relocated)
		return;
	o->relocated = 1;
	if (o->rela)
		apply_rela(o, o->rela, o->relasz);
	if (o->jmprel)
		apply_rela(o, o->jmprel, o->pltrelsz);
}

static void run_init(struct obj *o)
{
	if (o->init)
		o->init();
	if (o->init_array) {
		unsigned long n = o->init_arraysz / sizeof(Elf64_Addr);
		for (unsigned long i = 0; i < n; i++) {
			void (*fn)(void) = (void (*)(void))o->init_array[i];
			if (fn)
				fn();
		}
	}
}

/* --------------------------------------------------------------------------
 * Self-relocation (IMGACT.EXE is static-PIE).
 *
 * Applied before any global/GOT access. Uses only the caller-supplied load
 * base, the hidden _DYNAMIC symbol (reached PC-relative, no GOT), and locals.
 * Only the RELATIVE relocation (R_AARCH64_RELATIVE / R_X86_64_RELATIVE)
 * appears in a -nostdlib static-PIE binary's own relocations. MUST NOT call
 * other functions or touch globals.
 * -------------------------------------------------------------------------- */

extern Elf64_Dyn _DYNAMIC[] __attribute__((visibility("hidden")));

__attribute__((no_stack_protector))
static void self_relocate(unsigned long base)
{
	Elf64_Dyn *d = _DYNAMIC;   /* PC-relative (hidden): no reloc needed */
	unsigned long rela = 0, relasz = 0;
	for (; d->d_tag != DT_NULL; d++) {
		if (d->d_tag == DT_RELA)
			rela = base + d->d_un.d_ptr;
		else if (d->d_tag == DT_RELASZ)
			relasz = d->d_un.d_val;
	}
	Elf64_Rela *r = (Elf64_Rela *)rela;
	unsigned long n = relasz / sizeof(Elf64_Rela);
	for (unsigned long i = 0; i < n; i++) {
		if (ELF64_R_TYPE(r[i].r_info) == IMGACT_R_RELATIVE) {
			unsigned long *w =
				(unsigned long *)(base + r[i].r_offset);
			*w = base + (unsigned long)r[i].r_addend;
		}
	}
}

/* --------------------------------------------------------------------------
 * Bootstrap: entered from _start (arch/aarch64/start.S) with the raw stack.
 * -------------------------------------------------------------------------- */

/* Compute the executable's load bias from PT_PHDR (0 for ET_EXEC). */
static unsigned long exec_bias(Elf64_Phdr *phdr, int phnum, unsigned long at_phdr)
{
	for (int i = 0; i < phnum; i++)
		if (phdr[i].p_type == PT_PHDR)
			return at_phdr - phdr[i].p_vaddr;
	return 0;
}

/* --------------------------------------------------------------------------
 * OVMX symbol-vector activation (bead vms-714).
 *
 * An image produced by LINK.EXE has no PT_DYNAMIC. A shareable image exports
 * universal symbols through a .vms$sv symbol vector; an executable imports them
 * through a .vms$imp table naming (producer soname, vector index). IMGACT binds
 * each import by vector POSITION + GSMATCH (ovmx_symvec.h), writing the resolved
 * address into the consumer's GOT cell — the VMS-native replacement for ELF
 * DT_HASH/DT_NEEDED resolution.
 * -------------------------------------------------------------------------- */

/* Find a section's load vaddr + size by name, via the file's section headers
 * (read over the ACP window, vms-3e8e). */
static int ovmx_find_section(struct imgsrc *src, const char *want,
			     unsigned long *addr, unsigned long *size)
{
	Elf64_Ehdr eh;
	if (imgsrc_pread(src, &eh, sizeof eh, 0) != (long)sizeof eh)
		return 0;
	if (eh.e_shnum == 0 || eh.e_shnum > 64 || eh.e_shstrndx >= eh.e_shnum)
		return 0;
	Elf64_Shdr sh[64];
	unsigned long ssz = (unsigned long)eh.e_shnum * sizeof(Elf64_Shdr);
	if (imgsrc_pread(src, sh, ssz, (long)eh.e_shoff) != (long)ssz)
		return 0;
	static char strtab[2048];
	unsigned long stsz = sh[eh.e_shstrndx].sh_size;
	if (stsz > sizeof strtab)
		return 0;
	if (imgsrc_pread(src, strtab, stsz, (long)sh[eh.e_shstrndx].sh_offset) != (long)stsz)
		return 0;
	for (int i = 0; i < eh.e_shnum; i++) {
		if (xstrcmp(strtab + sh[i].sh_name, want) == 0) {
			*addr = sh[i].sh_addr;
			*size = sh[i].sh_size;
			return 1;
		}
	}
	return 0;
}

/* Apply the .vms$rel self-relative fixups: add the load bias to every
 * image-relative slot LINK.EXE recorded (synthesized GOT cells, pointer data).
 * The VMS-native equivalent of processing R_AARCH64_RELATIVE, without a
 * PT_DYNAMIC. No-op for images with no .vms$rel. `src` must be open on the
 * image; `base` is its load bias. The target pages must already be writable. */
static void apply_vms_rel(struct imgsrc *src, unsigned long base)
{
	unsigned long rel_addr, rel_size;
	if (!ovmx_find_section(src, OVMX_REL_SECTION, &rel_addr, &rel_size))
		return;
	const struct ovmx_rel_header *rh =
		(const struct ovmx_rel_header *)(base + rel_addr);
	if (rh->magic != OVMX_REL_MAGIC || rel_size < sizeof *rh)
		return;
	const unsigned long *off =
		(const unsigned long *)((const char *)rh + sizeof *rh);
	for (unsigned k = 0; k < rh->count; k++)
		*(unsigned long *)(base + off[k]) += base;
}

struct ovmx_prod {
	char                         name[128];
	unsigned long                base;
	const struct ovmx_sv_header *sv;
	/* TLS (PT_TLS) + TLSDESC entries the activator must complete (.vms$tls). */
	int                          has_tls;
	unsigned long                tls_image;    /* runtime addr of .tdata image */
	unsigned long                tls_filesz;
	unsigned long                tls_memsz;
	unsigned long                tls_align;
	unsigned long                tls_offset;   /* assigned offset from TP        */
	const struct ovmx_tls_header *tlsdesc;     /* .vms$tls header (0 if none)     */
};
static struct ovmx_prod g_prods[32];
static int              g_nprods;

/* The executable module itself (its own PT_TLS + .vms$tls TLSDESC table), for
 * the symbol-vector path. Unlike a producer it has no .vms$sv (sv stays 0) and
 * is not in g_prods, so find_crtl_producer never touches it; it participates in
 * TLS setup exactly like a TLS-producing shareable does (bead vms-c86). */
static struct ovmx_prod g_exe;

/* Transitive-import machinery (vms-e65): a producer shareable may itself import
 * universals from another producer (a lib shareable -> DECC$SHR). These three
 * are mutually recursive (bind -> load -> resolve -> bind), so forward-declare. */
static struct ovmx_prod *load_ovmx_producer(const char *soname);
static void bind_imports(unsigned long base, const struct ovmx_imp_header *ih,
			 const char *whoami);
static void resolve_producer_imports(struct ovmx_prod *p, unsigned long imp_addr);

/* Map a producer shareable image's PT_LOAD segments and locate its .vms$sv. */
static struct ovmx_prod *load_ovmx_producer(const char *soname)
{
	for (int i = 0; i < g_nprods; i++)
		if (xstrcmp(g_prods[i].name, soname) == 0)
			return &g_prods[i];
	if (g_nprods >= 32)
		return 0;

	/* Search: SYS$SHARE fallback, then the name as given -- both read over
	 * the executive Files-11 ACP (vms-3e8e), never a /vms POSIX open. */
	char path[256];
	xstrcpy(path, IMGACT_FALLBACK_SYSLIB "/");
	xstrcat(path, soname);
	struct imgsrc src;
	if (imgsrc_open(&src, path) < 0 && imgsrc_open(&src, soname) < 0)
		return 0;

	Elf64_Ehdr eh;
	if (imgsrc_pread(&src, &eh, sizeof eh, 0) != (long)sizeof eh) { imgsrc_close(&src); return 0; }
	Elf64_Phdr ph[16];
	if (eh.e_phnum > 16) { imgsrc_close(&src); return 0; }
	if (imgsrc_pread(&src, ph, (unsigned long)eh.e_phnum * sizeof(Elf64_Phdr),
		      (long)eh.e_phoff) < 0) { imgsrc_close(&src); return 0; }

	unsigned long lo = ~0UL, hi = 0;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		if (PAGE_DOWN(ph[i].p_vaddr) < lo) lo = PAGE_DOWN(ph[i].p_vaddr);
		if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (lo == ~0UL) { imgsrc_close(&src); return 0; }
	unsigned long span = PAGE_UP(hi) - lo;
	void *map = sys_mmap(0, span, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) { imgsrc_close(&src); return 0; }
	unsigned long base = (unsigned long)map - lo;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0) continue;
		if (imgsrc_pread(&src, (void *)(base + ph[i].p_vaddr), ph[i].p_filesz,
			      (long)ph[i].p_offset) < 0) { imgsrc_close(&src); return 0; }
	}
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		int prot = 0;
		if (ph[i].p_flags & PF_R) prot |= PROT_READ;
		if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
		if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;
		unsigned long a = PAGE_DOWN(base + ph[i].p_vaddr);
		unsigned long z = PAGE_UP(base + ph[i].p_vaddr + ph[i].p_memsz);
		sys_mprotect((void *)a, z - a, prot);
	}

	unsigned long sv_addr, sv_size;
	int ok = ovmx_find_section(&src, OVMX_SV_SECTION, &sv_addr, &sv_size);
	/* Bias this producer's own self-relative slots (GOT cells, pointer data)
	 * before any of its universal code runs. Pages are RWX at this point. */
	if (ok)
		apply_vms_rel(&src, base);
	/* Locate the .vms$tls TLSDESC table (completed after TLS offsets assigned). */
	unsigned long tls_addr, tls_size;
	int have_tlsdesc = ovmx_find_section(&src, OVMX_TLS_SECTION, &tls_addr, &tls_size);
	/* Locate this producer's OWN .vms$imp: a lib shareable that itself imports
	 * from another producer (e.g. libc/pthread from DECC$SHR). Resolved
	 * transitively after registration below. (vms-e65) */
	unsigned long imp_addr, imp_size;
	int have_imp = ovmx_find_section(&src, OVMX_IMP_SECTION, &imp_addr, &imp_size);
	imgsrc_close(&src);
	if (!ok)
		return 0;

	struct ovmx_prod *p = &g_prods[g_nprods++];
	xstrcpy(p->name, soname);
	p->base = base;
	p->sv = (const struct ovmx_sv_header *)(base + sv_addr);
	if (p->sv->magic != OVMX_SV_MAGIC) { g_nprods--; return 0; }

	/* Record PT_TLS geometry (for the symbol-vector TLS setup pass). */
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_TLS) continue;
		p->has_tls    = 1;
		p->tls_image  = base + ph[i].p_vaddr;
		p->tls_filesz = ph[i].p_filesz;
		p->tls_memsz  = ph[i].p_memsz;
		p->tls_align  = ph[i].p_align ? ph[i].p_align : 1;
		break;
	}
	p->tlsdesc = have_tlsdesc
		? (const struct ovmx_tls_header *)(base + tls_addr) : 0;

	/* Transitively bind this producer's own imports. Done AFTER registration so a
	 * dependency cycle dedups through find-loaded, and AFTER apply_vms_rel above so
	 * the .vms$sv values it exports are already load-biased before a dependent
	 * reads them. Its GOT cells are writable (RWX PT_LOAD). (vms-e65) */
	if (have_imp)
		resolve_producer_imports(p, imp_addr);
	return p;
}

/* Bind every .vms$imp import at `ih` into image `base`: map each named producer
 * (recursively — transitive imports), GSMATCH-resolve the universal by vector
 * index, and store the run-time address into the importing image's GOT cell.
 * Shared by the executable's activation and a producer's transitive resolution
 * (a lib shareable that itself imports from DECC$SHR — vms-e65). */
static void bind_imports(unsigned long base, const struct ovmx_imp_header *ih,
			 const char *whoami)
{
	if (ih->magic != OVMX_IMP_MAGIC)
		die_imgfmterr(whoami);
	const struct ovmx_imp_entry *ie =
		(const struct ovmx_imp_entry *)((const char *)ih + sizeof *ih);
	const char *names = (const char *)ih + ih->names_off;
	for (unsigned k = 0; k < ih->count; k++) {
		const char *soname = names + ie[k].producer_off;
		struct ovmx_prod *p = load_ovmx_producer(soname);
		if (!p)
			die_imgnotfnd(soname);
		unsigned long addr = ovmx_sv_resolve(p->sv, ie[k].sv_index, p->base,
						     ie[k].req_major, ie[k].req_minor);
		if (!addr) {
			vms_fatal("GSMATCH", "shareable image version mismatch", soname);
			sys_exit(IMGACT_EXIT_FAIL);
		}
		*(unsigned long *)(base + ie[k].patch_off) = addr;
	}
}

/* Resolve a producer shareable's OWN cross-image imports (its .vms$imp) against
 * the producers it names — transitive binding, e.g. a lib shareable -> DECC$SHR.
 * The .vms$imp table lives in the producer's mapped image at base + imp_addr. */
static void resolve_producer_imports(struct ovmx_prod *p, unsigned long imp_addr)
{
	const struct ovmx_imp_header *ih =
		(const struct ovmx_imp_header *)(p->base + imp_addr);
	bind_imports(p->base, ih, p->name);
}

/* Copy a module's .tdata init image into the combined TLS block at its assigned
 * tls_offset and complete its static TLSDESC entries (resolver + TP-relative
 * offset). Shared by every TLS participant in the IMGACT-owned (no-C-RTL) path:
 * producer shareables AND the executable module (bead vms-c86). */
static void symvec_tls_place(struct ovmx_prod *p, char *tp)
{
	if (p->tls_filesz)
		memcpy(tp + p->tls_offset, (void *)p->tls_image, p->tls_filesz);
	/* .tbss stays zero (anonymous mapping). */
	if (!p->tlsdesc || p->tlsdesc->magic != OVMX_TLS_MAGIC)
		return;
	const unsigned long *eo =
		(const unsigned long *)((const char *)p->tlsdesc + sizeof *p->tlsdesc);
	for (unsigned k = 0; k < p->tlsdesc->count; k++) {
		unsigned long *entry = (unsigned long *)(p->base + eo[k]);
		entry[0] = (unsigned long)__tlsdesc_static;
		entry[1] += p->tls_offset;
	}
}

/* Symbol-vector TLS setup: assign each TLS module a block offset from TP,
 * allocate the thread's TLS block, copy per-module init images, program the
 * thread pointer, then complete each module's TLSDESC entries (resolver +
 * TP-relative offset). Mirrors the DT_HASH path (assign_tls_offsets/setup_tls +
 * the R_AARCH64_TLSDESC handler) but over the .vms$imp producers PLUS the
 * executable module itself (g_exe) when it carries its own TLS (bead vms-c86). */
static void setup_symvec_tls(void)
{
	/* Gather every TLS participant: producer shareables then the executable
	 * module. The executable is laid out exactly like a producer; only its
	 * source differs (kernel-mapped main image vs IMGACT-mapped shareable). */
	struct ovmx_prod *mods[33];
	int nmods = 0;
	for (int i = 0; i < g_nprods; i++)
		if (g_prods[i].has_tls)
			mods[nmods++] = &g_prods[i];
	if (g_exe.has_tls)
		mods[nmods++] = &g_exe;
	if (nmods == 0)
		return;

	/* Assign each module a signed TP-relative base offset, matching the
	 * DT_HASH path (assign_tls_offsets). */
	unsigned long tp_off, total;
#if IMGACT_TLS_VARIANT == 1
	unsigned long cursor = TLS_TCB_SIZE;   /* reserve TCB below the blocks */
	for (int i = 0; i < nmods; i++) {
		unsigned long off = ALIGN_UP(cursor, mods[i]->tls_align);
		mods[i]->tls_offset = off;                 /* positive */
		cursor = off + mods[i]->tls_memsz;
	}
	tp_off = 0;
	total  = cursor;
#else
	unsigned long run = 0, maxalign = 16;
	for (int i = 0; i < nmods; i++) {
		run = ALIGN_UP(run + mods[i]->tls_memsz, mods[i]->tls_align);
		mods[i]->tls_offset = (unsigned long)(-(long)run);   /* negative */
		if (mods[i]->tls_align > maxalign)
			maxalign = mods[i]->tls_align;
	}
	tp_off = ALIGN_UP(run, maxalign);
	total  = tp_off + TLS_TCB_SIZE;
#endif

	unsigned long len = PAGE_UP(total);
	void *area = sys_mmap(0, len, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		die_tlserr("TLS block");
	char *tp = (char *)area + tp_off;
#if IMGACT_TLS_VARIANT == 2
	*(void **)tp = tp;   /* self-pointer at %fs:0 (Variant II TCB) */
#endif
	imgact_set_tp(tp);

	for (int i = 0; i < nmods; i++)
		symvec_tls_place(mods[i], tp);
}

/* TLS producer coexisting with a C-RTL (bead vms-616).
 *
 * A real OVMX library shareable (e.g. VMSPROCESS$SHR) is BOTH a TLS producer —
 * it has its own .tdata/.tbss and TLSDESC entries — AND an importer of libc/
 * pthread universals from DECC$SHR. After drive_crtl_init(), musl (the C-RTL)
 * owns the thread pointer and laid out its own TCB/TLS; it knows nothing about
 * the producer's TLS module, and its static (dynamic-linker-free) build has no
 * __tls_get_new to grow the DTV. Rather than replicate musl's `struct pthread`
 * / DTV internals, IMGACT absorbs each producer's TLS module into the running
 * process's TLS view by allocating the module its own per-thread block and
 * resolving the producer's STATIC TLSDESC entries relative to MUSL's already-
 * programmed TP: a variable's returned offset is (block - TP) + module_offset,
 * so the standard TLSDESC access sequence (TP + returned offset) lands inside
 * the IMGACT-owned block. entry[0] therefore stays __tlsdesc_static; only
 * entry[1] (pre-filled by LINK.EXE with the module-relative offset) is biased.
 *
 * Scope: correct for the MAIN thread, which is all OVMX activation uses today.
 * A program that spawns pthreads would need the module laid into each new
 * thread's block by musl (true DTV registration via a resurrected __tls_get_new
 * path); that multi-thread producer-TLS case is deferred (vms-616 follow-up). */
/* Absorb one TLS module (a producer shareable OR the executable) into musl's
 * already-programmed TLS view: give the module its own per-thread block and bias
 * its static TLSDESC entries relative to musl's TP. `tp` is musl's thread
 * pointer. Shared by producers and the executable module (bead vms-c86). */
static void absorb_tls_over_crtl(struct ovmx_prod *p, unsigned long tp)
{
	/* Per-thread TLS instance: [.tdata init image | .tbss zero]. */
	unsigned long al  = p->tls_align ? p->tls_align : 1;
	unsigned long len = PAGE_UP(p->tls_memsz ? p->tls_memsz : 1);
	void *blk = sys_mmap(0, len, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (blk == MAP_FAILED)
		die_tlserr("producer TLS block");
	/* mmap is page-aligned, so any sane module alignment is satisfied. */
	if (al > PAGE_SIZE)
		die_tlserr("producer TLS overalignment");
	if (p->tls_filesz)
		memcpy(blk, (void *)p->tls_image, p->tls_filesz);
	/* .tbss stays zero (anonymous mapping). */

	/* Signed distance from musl's TP to this block's base. TLSDESC
	 * returns module_offset + delta; TP + that == blk + module_offset. */
	unsigned long delta = (unsigned long)blk - tp;
	p->tls_offset = delta;   /* recorded for symmetry/diagnostics */

	if (!p->tlsdesc || p->tlsdesc->magic != OVMX_TLS_MAGIC)
		return;
	const unsigned long *eo =
		(const unsigned long *)((const char *)p->tlsdesc +
					sizeof *p->tlsdesc);
	for (unsigned k = 0; k < p->tlsdesc->count; k++) {
		unsigned long *entry = (unsigned long *)(p->base + eo[k]);
		entry[0] = (unsigned long)__tlsdesc_static;
		entry[1] += delta;   /* module-relative -> TP-relative */
	}
}

static void setup_producer_tls_over_crtl(struct ovmx_prod *crtl)
{
	unsigned long tp = (unsigned long)imgact_get_tp();
	for (int i = 0; i < g_nprods; i++) {
		struct ovmx_prod *p = &g_prods[i];
		if (!p->has_tls || p == crtl)
			continue;
		absorb_tls_over_crtl(p, tp);
	}
	/* The executable module's own TLS (e.g. DCL's dcl_messages.o) is absorbed
	 * the same way — it is not in g_prods, so handle it explicitly (vms-c86). */
	if (g_exe.has_tls)
		absorb_tls_over_crtl(&g_exe, tp);
}

/* --------------------------------------------------------------------------
 * musl C-RTL (DECC$SHR) runtime initialization (bead vms-61f.2).
 *
 * A whole-archived musl libc packaged as an OVMX shareable (DECC$SHR.EXE) is
 * inert until musl's own bootstrap runs: musl expects to OWN the process thread
 * pointer and set up its per-thread control block (the `struct pthread` TCB it
 * reaches through TPIDR_EL0), the stack-guard canary, page size, and the malloc
 * state — all BEFORE any libc entry point is called. That bootstrap is musl's
 * `__init_libc()`, which internally runs `__init_tls`/`__init_tp` (allocate the
 * TCB + any main-program TLS, program the thread pointer musl-style) and
 * `__init_ssp` (from AT_RANDOM). It is exactly what musl's crt / `__libc_start_
 * main` / ld-musl's `__dls3` call before transferring to the program.
 *
 * Rather than fragile-replicate musl's private `struct pthread` layout inside
 * IMGACT, the activator DRIVES musl's own `__init_libc`, resolved by NAME from
 * the C-RTL producer's symbol vector, passing it the real process envp (so musl
 * recomputes the auxv it needs: AT_PHDR/AT_PHENT/AT_PHNUM, AT_RANDOM, AT_PAGESZ).
 * musl then owns the thread pointer and TLS — which is why a C-RTL producer and
 * an IMGACT-managed TLSDESC producer are mutually exclusive TLS owners in one
 * process (see activate_symbol_vector). Reading/mirroring musl's MIT-licensed
 * ldso/env sources to implement this is permitted (CLAUDE.md rule 8 concerns
 * VMS/VSI/HPE only).
 *
 * C-RTL *constructors* (`__libc_start_init`) are not run here: a shareable built
 * from pure musl libc.a + libgcc.a carries no .init_array and no crt `_init`, so
 * there is nothing to run, and calling `__libc_start_init` would invoke an
 * unresolved `_init`. Constructor execution belongs to the OVMX-lib migration
 * wave (vms-b65.*), where producers actually carry init_array.
 * -------------------------------------------------------------------------- */

/* Find a universal by NAME in a producer's symbol vector; return its run-time
 * address, or 0 if not exported. Binding is normally by index; the C-RTL
 * bootstrap symbol is not one the consumer imports, so IMGACT looks it up by
 * name (the .vms$sv name blob exists for exactly this + diagnostics). */
static unsigned long sv_find_named(const struct ovmx_prod *p, const char *name)
{
	const struct ovmx_sv_header *h = p->sv;
	const struct ovmx_sv_entry  *e = ovmx_sv_entries(h);
	const char                  *names = ovmx_sv_names(h);
	for (unsigned k = 0; k < h->count; k++) {
		if (e[k].kind == OVMX_SV_RETIRED)
			continue;
		if (xstrcmp(names + e[k].name_off, name) == 0)
			return p->base + e[k].value;
	}
	return 0;
}

/* The C-RTL producer is the one exporting musl's __init_libc bootstrap. */
static struct ovmx_prod *find_crtl_producer(void)
{
	for (int i = 0; i < g_nprods; i++)
		if (sv_find_named(&g_prods[i], "__init_libc"))
			return &g_prods[i];
	return 0;
}

/* Publish the producers IMGACT mapped into the resident LIBVMS$SHR registry
 * (vms-db2, design §A.8 remainder gap 1). IMGACT keeps producer bases in its
 * OWN private g_prods[] and discarded them at hand-off; the in-process activator
 * (imgact_activate, inside LIBVMS$SHR) therefore had an EMPTY resident-producer
 * registry at runtime and could not bind a later RUN'd image's .vms$imp imports
 * to the LIBVMS$SHR/DECC$SHR this process already holds. This hands them across:
 * resolve imgact_publish_producers (a LIBVMS$SHR universal) BY NAME from
 * whichever producer exports it, marshal g_prods[] into the published-producer
 * ABI (imgact_prodreg.h), and call it once. Best-effort: a consumer whose graph
 * has no producer exporting the symbol (a non-libvms executable, or an older
 * shareable) publishes nothing and activates exactly as before -- no regression.
 * The registry-population logic itself lives in LIBVMS$SHR and is proven in
 * isolation (test_imgact_publish); the fork fallback for real images
 * stays until the full real-image in-process flip lands. */
static void publish_resident_producers(void)
{
	unsigned long pub = 0;
	for (int i = 0; i < g_nprods; i++) {
		pub = sv_find_named(&g_prods[i], "imgact_publish_producers");
		if (pub)
			break;
	}
	if (!pub)
		return;   /* no LIBVMS$SHR in this graph: nothing to publish into */

	struct imgact_prod_pub list[32];
	int n = 0;
	for (int i = 0; i < g_nprods && n < 32; i++) {
		list[n].soname = g_prods[i].name;
		list[n].base   = g_prods[i].base;
		list[n].sv     = g_prods[i].sv;
		n++;
	}
	((uint32_t (*)(const struct imgact_prod_pub *, int))pub)(list, n);
}

/* Run musl's own libc bootstrap for a mapped C-RTL producer: program the thread
 * pointer, build the TCB + TLS musl-style, set the stack guard, and make malloc
 * usable. After this returns, every universal in the C-RTL is callable. */
static void drive_crtl_init(struct ovmx_prod *crtl)
{
	unsigned long init_libc = sv_find_named(crtl, "__init_libc");
	if (!init_libc)
		die_undsym("__init_libc");
	((void (*)(char **, char *))init_libc)(g_envp, g_argv0);
}

/* Resolve every .vms$imp import of the executable against producer symbol
 * vectors, patching the consumer's GOT cells. `ephdr`/`ephnum` are the
 * kernel-mapped main image's program headers (for its PT_TLS geometry). */
static void activate_symbol_vector(unsigned long exe_base, const char *execfn,
				   Elf64_Phdr *ephdr, int ephnum)
{
	if (!execfn)
		die_imgfmterr("IMAGE.EXE");
	/* The main image's LOAD segments are already kernel-mapped; its SECTION
	 * headers (.vms$imp/.vms$rel/.vms$tls) are not, so re-read them off the
	 * file -- now over the executive Files-11 ACP (vms-3e8e), not a /vms POSIX
	 * open. Fail-honest: not on the ACP volume -> honest %IMGACT error. */
	struct imgsrc src;
	if (imgsrc_open(&src, execfn) < 0)
		die_imgnotfnd(execfn);
	unsigned long imp_addr, imp_size;
	int ok = ovmx_find_section(&src, OVMX_IMP_SECTION, &imp_addr, &imp_size);
	/* Bias the executable's own self-relative slots (its GOT/pointer data), if
	 * any; harmless no-op for the current PLT-only executables. */
	if (ok)
		apply_vms_rel(&src, exe_base);

	/* Record the executable module's OWN TLS geometry (PT_TLS) + its .vms$tls
	 * TLSDESC table, so the executable participates in TLS setup exactly like a
	 * TLS-producing shareable. DCL.EXE is a single-TLS-object image
	 * (dcl_messages.o). Located while fd is still open. (bead vms-c86) */
	g_exe.base = exe_base;
	for (int i = 0; i < ephnum; i++) {
		if (ephdr[i].p_type != PT_TLS)
			continue;
		g_exe.has_tls    = 1;
		g_exe.tls_image  = exe_base + ephdr[i].p_vaddr;
		g_exe.tls_filesz = ephdr[i].p_filesz;
		g_exe.tls_memsz  = ephdr[i].p_memsz;
		g_exe.tls_align  = ephdr[i].p_align ? ephdr[i].p_align : 1;
		break;
	}
	if (g_exe.has_tls) {
		unsigned long tls_addr, tls_size;
		if (ovmx_find_section(&src, OVMX_TLS_SECTION, &tls_addr, &tls_size))
			g_exe.tlsdesc =
				(const struct ovmx_tls_header *)(exe_base + tls_addr);
	}
	imgsrc_close(&src);
	if (!ok)
		die_imgfmterr("IMAGE.EXE");

	/* Bind the executable's imports. load_ovmx_producer transitively resolves
	 * each producer's OWN imports too, so a lib shareable's libc/pthread calls
	 * are already bound to DECC$SHR by the time we return here. (vms-e65) */
	const struct ovmx_imp_header *ih =
		(const struct ovmx_imp_header *)(exe_base + imp_addr);
	bind_imports(exe_base, ih, "IMAGE.EXE");

	/* TLS ownership. There is exactly one thread pointer (TPIDR_EL0) per
	 * process, so whoever programs it defines the TLS coordinate system:
	 *  - C-RTL present -> musl owns TP + its own TCB/TLS (drive_crtl_init).
	 *    A TLS-bearing lib producer (its own .tdata/.tbss + TLSDESC) that also
	 *    imports from DECC$SHR then has its TLS module ABSORBED into that view:
	 *    IMGACT gives the module its own block and biases its static TLSDESC
	 *    entries relative to musl's TP (setup_producer_tls_over_crtl, vms-616).
	 *    The executable module's OWN TLS (e.g. DCL's dcl_messages.o) is absorbed
	 *    the same way in that pass (g_exe, vms-c86).
	 *  - no C-RTL      -> IMGACT owns TP for any TLSDESC producer AND the
	 *    executable's own TLS (setup_symvec_tls includes g_exe, vms-c86).
	 * (The truly-conflicting "two things both want to BE the TP owner" case does
	 * not arise: DECC$SHR carries no PT_TLS — musl's own TLS comes from the main
	 * program's PT_TLS, which OVMX consumers do not have.) */
	struct ovmx_prod *crtl = find_crtl_producer();
	if (crtl) {
		drive_crtl_init(crtl);            /* musl owns TP + its TCB/TLS  */
		setup_producer_tls_over_crtl(crtl);  /* absorb producer TLS modules */
	} else {
		/* Set up TLS for any producer that has thread-local storage, before the
		 * consumer (which may call into that producer's TLS-using code) runs. */
		setup_symvec_tls();
	}

	/* Hand the resident producer bases across to LIBVMS$SHR's registry so a
	 * later in-process RUN can bind to them (vms-db2, §A.8 gap 1). AFTER the
	 * C-RTL init above: imgact_publish_producers -> imgact_register_producer
	 * calls strlen/strncmp/strncpy, DECC$SHR universals that are only usable once
	 * musl (drive_crtl_init) has run and LIBVMS$SHR's own imports are bound. */
	publish_resident_producers();
}

unsigned long imgact_bootstrap(unsigned long *sp)
{
	/* ---- Parse the initial process stack (no globals yet). ---- */
	long argc = (long)sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;
	char **e = envp;
	while (*e)
		e++;
	Elf64_auxv_t *auxv = (Elf64_auxv_t *)(e + 1);

	unsigned long at_base = 0, at_phdr = 0, at_entry = 0;
	long at_phnum = 0;
	const char *at_execfn = 0;
	for (Elf64_auxv_t *a = auxv; a->a_type != AT_NULL; a++) {
		switch (a->a_type) {
		case AT_BASE:   at_base   = a->a_un.a_val; break;
		case AT_PHDR:   at_phdr   = a->a_un.a_val; break;
		case AT_PHNUM:  at_phnum  = (long)a->a_un.a_val; break;
		case AT_ENTRY:  at_entry  = a->a_un.a_val; break;
		case AT_EXECFN: at_execfn = (const char *)a->a_un.a_val; break;
		default: break;
		}
	}

	/* ---- Self-relocate IMGACT.EXE, then globals become usable. ---- */
	self_relocate(at_base);
	g_auxv  = auxv;
	g_envp  = envp;                         /* for the C-RTL __init_libc bootstrap */
	g_argv0 = argc > 0 ? argv[0] : 0;

	/* ---- Build the executable object from the kernel-provided phdrs. ----
	 * The kernel has already mapped the main executable's LOAD segments;
	 * IMGACT only needs to relocate it and load its shareable images.
	 * (This departs from design spec §2 step 3, which describes IMGACT
	 * mapping the executable — the kernel does that for the main image.) */
	Elf64_Phdr *ephdr = (Elf64_Phdr *)at_phdr;
	int ephnum = (int)at_phnum;
	unsigned long ebias = exec_bias(ephdr, ephnum, at_phdr);

	Elf64_Dyn *edyn = 0;
	for (int i = 0; i < ephnum; i++)
		if (ephdr[i].p_type == PT_DYNAMIC)
			edyn = (Elf64_Dyn *)(ebias + ephdr[i].p_vaddr);

	/* An OVMX symbol-vector image (LINK.EXE output) has no PT_DYNAMIC: it
	 * binds universal symbols through .vms$imp, not ELF DT_HASH. (vms-714) */
	if (!edyn) {
		activate_symbol_vector(ebias, at_execfn, ephdr, ephnum);
		return at_entry;
	}

	/* ---- Legacy ELF DT_NEEDED path (913.2 bootstrap). ---- */
	struct obj *exe = &g_objs[g_nobjs++];
	memset(exe, 0, sizeof(*exe));
	xstrcpy(exe->name, "IMAGE.EXE");
	exe->base = ebias;
	exe->dyn = edyn;
	parse_dynamic(exe);
	scan_tls(exe, ephdr, ephnum);

	/* ---- Load shareable images (recursively). ---- */
	for (int i = 0; i < g_nobjs; i++)
		load_deps(&g_objs[i]);
	known_db_shutdown(); /* KFE lookups are done; release the mmap/fd (vms-30d) */

	/* ---- Assign TLS offsets, relocate (leaves first), init TLS. ---- */
	assign_tls_offsets();
	for (int i = g_nobjs - 1; i >= 0; i--)
		relocate_obj(&g_objs[i]);
	setup_tls();

	/* ---- Run constructors (leaves first). ---- */
	for (int i = g_nobjs - 1; i >= 0; i--)
		run_init(&g_objs[i]);

	/* ---- Transfer control to the executable entry point. ---- */
	return at_entry;
}
