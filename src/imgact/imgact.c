/*
 * IMGACT.EXE — OVMX image activator (aarch64).
 *
 * Bead vms-913.2. Design contract: docs/design-image-activation.md.
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
 * Scope (vms-913.2): aarch64 core activator + minimal proof harness. Out of
 * scope and deferred to sibling beads: GSMATCH enforcement (913.4), the
 * INSTALL known-image DB (913.5), CMake OVMX_IMGACT mode (913.3), x86_64
 * (913.11). GNU_HASH is not yet supported; OVMX images are linked
 * --hash-style=sysv (DT_HASH) for now.
 */

#include <elf.h>

#include "arch/aarch64/imgact_arch.h"
#include "ovmx_image.h"   /* OVMX symbol-vector image format (LINK.EXE) */
#include "ovmx_symvec.h"  /* shared resolver + GSMATCH (bead vms-8d5)  */

#ifndef AT_EXECFN
#define AT_EXECFN 31
#endif

#ifndef O_RDONLY
#define O_RDONLY 0
#endif

/* --------------------------------------------------------------------------
 * Freestanding syscall layer (no libc; IMGACT.EXE is -nostdlib).
 * -------------------------------------------------------------------------- */

#define SYS_openat      56
#define SYS_close       57
#define SYS_read        63
#define SYS_pread64     67
#define SYS_write       64
#define SYS_mmap        222
#define SYS_mprotect    226
#define SYS_munmap      215
#define SYS_exit_group  94

static long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc 0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

static long sys_openat(const char *path, int flags)
{
	/* AT_FDCWD = -100 */
	return syscall6(SYS_openat, -100, (long)path, flags, 0, 0, 0);
}
static long sys_close(int fd) { return syscall6(SYS_close, fd, 0, 0, 0, 0, 0); }
static long sys_pread(int fd, void *buf, unsigned long n, long off)
{
	return syscall6(SYS_pread64, fd, (long)buf, n, off, 0, 0);
}
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

#ifndef PROT_READ
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
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

struct obj {
	char           name[64];
	unsigned long  base;          /* load bias */
	Elf64_Dyn     *dyn;
	Elf64_Sym     *symtab;
	const char    *strtab;
	Elf64_Word    *hash;          /* DT_HASH */
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

struct builtin_sym { const char *name; unsigned long addr; };
static const struct builtin_sym g_builtins[] = {
	{ "imgact_probe",   (unsigned long)imgact_probe   },
	{ "imgact_counter", (unsigned long)&imgact_counter },
	{ "__getauxval",    (unsigned long)__getauxval    },
};
#define N_BUILTINS (sizeof(g_builtins) / sizeof(g_builtins[0]))

/* --------------------------------------------------------------------------
 * Symbol resolution.
 * -------------------------------------------------------------------------- */

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
	Elf64_Word nbucket = o->hash[0];
	Elf64_Word *bucket = o->hash + 2;
	Elf64_Word *chain  = bucket + nbucket;
	if (!nbucket)
		return 0;
	for (Elf64_Word i = bucket[hash % nbucket]; i; i = chain[i]) {
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
	o->hash         = hash ? (Elf64_Word *)(o->base + hash) : 0;
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

/* Map a shareable image file into memory; fill base/dyn. Returns obj index. */
static struct obj *load_object(const char *soname, const char *path)
{
	if (g_nobjs >= MAX_OBJS)
		die_mapfail(soname);

	int fd = (int)sys_openat(path, O_RDONLY);
	if (fd < 0)
		return 0;

	Elf64_Ehdr eh;
	if (sys_pread(fd, &eh, sizeof(eh), 0) != (long)sizeof(eh) ||
	    eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
	    eh.e_ident[2] != 'L'  || eh.e_ident[3] != 'F') {
		sys_close(fd);
		die_imgfmterr(soname);
	}

	Elf64_Phdr ph[32];
	if (eh.e_phnum > 32) {
		sys_close(fd);
		die_imgfmterr(soname);
	}
	if (sys_pread(fd, ph, (unsigned long)eh.e_phnum * eh.e_phentsize,
		      (long)eh.e_phoff) < 0) {
		sys_close(fd);
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
		sys_close(fd);
		die_imgfmterr(soname);
	}
	unsigned long span = PAGE_UP(hi) - lo;

	/* Reserve the whole span R/W anonymous, then read segments into place.
	 * Anonymous memory is zero-filled, so .bss needs no explicit clear. */
	void *map = sys_mmap(0, span, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) {
		sys_close(fd);
		die_mapfail(soname);
	}
	unsigned long base = (unsigned long)map - lo;

	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
			continue;
		if (sys_pread(fd, (void *)(base + ph[i].p_vaddr),
			      ph[i].p_filesz, (long)ph[i].p_offset) < 0) {
			sys_close(fd);
			die_mapfail(soname);
		}
	}
	sys_close(fd);

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

#define IMGACT_FALLBACK_SYSLIB "/vms/SYS0/SYSCOMMON/SYSLIB"

static struct obj *find_loaded(const char *soname)
{
	for (int i = 0; i < g_nobjs; i++)
		if (xstrcmp(g_objs[i].name, soname) == 0)
			return &g_objs[i];
	return 0;
}

/* Resolve a SONAME to a path and map it. Search order per design spec §4;
 * for vms-913.2 only the hardcoded SYS$SHARE fallback is implemented (the
 * Known Image DB is bead 913.5, SYS$SHARE logical translation needs VMSLNMD). */
static struct obj *load_needed(const char *soname)
{
	struct obj *existing = find_loaded(soname);
	if (existing)
		return existing;

	char path[512];
	path[0] = 0;
	xstrcat(path, IMGACT_FALLBACK_SYSLIB);
	xstrcat(path, "/");
	xstrcat(path, soname);

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
 * TLS layout (Variant I / TLS_ABOVE_TP).
 * -------------------------------------------------------------------------- */

static unsigned long g_tls_total;

static void assign_tls_offsets(void)
{
	unsigned long cursor = TLS_TCB_SIZE;   /* reserve TCB above TP */
	for (int i = 0; i < g_nobjs; i++) {
		struct obj *o = &g_objs[i];
		if (!o->has_tls)
			continue;
		unsigned long off = ALIGN_UP(cursor, o->tls_align);
		o->tls_offset = off;
		cursor = off + o->tls_memsz;
	}
	g_tls_total = cursor;
}

/* Allocate the TLS block, copy per-module init images, program TPIDR_EL0. */
static void setup_tls(void)
{
	unsigned long len = PAGE_UP(g_tls_total);
	void *area = sys_mmap(0, len, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		die_tlserr("TLS block");

	for (int i = 0; i < g_nobjs; i++) {
		struct obj *o = &g_objs[i];
		if (!o->has_tls)
			continue;
		memcpy((char *)area + o->tls_offset,
		       (void *)o->tls_image, o->tls_filesz);
		/* .tbss already zero (anonymous mapping). */
	}
	imgact_set_tp(area);
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
		case R_AARCH64_RELATIVE:
			*where = o->base + (unsigned long)r->r_addend;
			break;

		case R_AARCH64_GLOB_DAT:
		case R_AARCH64_JUMP_SLOT: {
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

		case R_AARCH64_TLSDESC: {
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

		case R_AARCH64_ABS64: {
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
 * Only R_AARCH64_RELATIVE appears in a -nostdlib static-PIE binary's own
 * relocations. MUST NOT call other functions or touch globals.
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
		if (ELF64_R_TYPE(r[i].r_info) == R_AARCH64_RELATIVE) {
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

/* Find a section's load vaddr + size by name, via the file's section headers. */
static int ovmx_find_section(int fd, const char *want,
			     unsigned long *addr, unsigned long *size)
{
	Elf64_Ehdr eh;
	if (sys_pread(fd, &eh, sizeof eh, 0) != (long)sizeof eh)
		return 0;
	if (eh.e_shnum == 0 || eh.e_shnum > 64 || eh.e_shstrndx >= eh.e_shnum)
		return 0;
	Elf64_Shdr sh[64];
	unsigned long ssz = (unsigned long)eh.e_shnum * sizeof(Elf64_Shdr);
	if (sys_pread(fd, sh, ssz, (long)eh.e_shoff) != (long)ssz)
		return 0;
	static char strtab[2048];
	unsigned long stsz = sh[eh.e_shstrndx].sh_size;
	if (stsz > sizeof strtab)
		return 0;
	if (sys_pread(fd, strtab, stsz, (long)sh[eh.e_shstrndx].sh_offset) != (long)stsz)
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
 * PT_DYNAMIC. No-op for images with no .vms$rel. `fd` must be open on the image;
 * `base` is its load bias. The target pages must already be writable. */
static void apply_vms_rel(int fd, unsigned long base)
{
	unsigned long rel_addr, rel_size;
	if (!ovmx_find_section(fd, OVMX_REL_SECTION, &rel_addr, &rel_size))
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

/* Map a producer shareable image's PT_LOAD segments and locate its .vms$sv. */
static struct ovmx_prod *load_ovmx_producer(const char *soname)
{
	for (int i = 0; i < g_nprods; i++)
		if (xstrcmp(g_prods[i].name, soname) == 0)
			return &g_prods[i];
	if (g_nprods >= 32)
		return 0;

	/* Search: SYS$SHARE fallback, then the name as given. */
	char path[256];
	xstrcpy(path, IMGACT_FALLBACK_SYSLIB "/");
	xstrcat(path, soname);
	int fd = (int)sys_openat(path, O_RDONLY);
	if (fd < 0)
		fd = (int)sys_openat(soname, O_RDONLY);
	if (fd < 0)
		return 0;

	Elf64_Ehdr eh;
	if (sys_pread(fd, &eh, sizeof eh, 0) != (long)sizeof eh) { sys_close(fd); return 0; }
	Elf64_Phdr ph[16];
	if (eh.e_phnum > 16) { sys_close(fd); return 0; }
	if (sys_pread(fd, ph, (unsigned long)eh.e_phnum * sizeof(Elf64_Phdr),
		      (long)eh.e_phoff) < 0) { sys_close(fd); return 0; }

	unsigned long lo = ~0UL, hi = 0;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD) continue;
		if (PAGE_DOWN(ph[i].p_vaddr) < lo) lo = PAGE_DOWN(ph[i].p_vaddr);
		if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
	}
	if (lo == ~0UL) { sys_close(fd); return 0; }
	unsigned long span = PAGE_UP(hi) - lo;
	void *map = sys_mmap(0, span, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED) { sys_close(fd); return 0; }
	unsigned long base = (unsigned long)map - lo;
	for (int i = 0; i < eh.e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0) continue;
		if (sys_pread(fd, (void *)(base + ph[i].p_vaddr), ph[i].p_filesz,
			      (long)ph[i].p_offset) < 0) { sys_close(fd); return 0; }
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
	int ok = ovmx_find_section(fd, OVMX_SV_SECTION, &sv_addr, &sv_size);
	/* Bias this producer's own self-relative slots (GOT cells, pointer data)
	 * before any of its universal code runs. Pages are RWX at this point. */
	if (ok)
		apply_vms_rel(fd, base);
	/* Locate the .vms$tls TLSDESC table (completed after TLS offsets assigned). */
	unsigned long tls_addr, tls_size;
	int have_tlsdesc = ovmx_find_section(fd, OVMX_TLS_SECTION, &tls_addr, &tls_size);
	sys_close(fd);
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
	return p;
}

/* Symbol-vector TLS setup: assign each producer's TLS block an offset from TP,
 * allocate the thread's TLS block, copy per-module init images, program the
 * thread pointer, then complete each producer's TLSDESC entries (resolver +
 * TP-relative offset). Mirrors the DT_HASH path (assign_tls_offsets/setup_tls +
 * the R_AARCH64_TLSDESC handler) but over the .vms$imp producers. */
static void setup_symvec_tls(void)
{
	unsigned long cursor = TLS_TCB_SIZE;   /* reserve the TCB above TP */
	int any = 0;
	for (int i = 0; i < g_nprods; i++) {
		struct ovmx_prod *p = &g_prods[i];
		if (!p->has_tls)
			continue;
		unsigned long off = ALIGN_UP(cursor, p->tls_align);
		p->tls_offset = off;
		cursor = off + p->tls_memsz;
		any = 1;
	}
	if (!any)
		return;

	unsigned long len = PAGE_UP(cursor);
	void *area = sys_mmap(0, len, PROT_READ | PROT_WRITE,
			      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (area == MAP_FAILED)
		die_tlserr("TLS block");
	for (int i = 0; i < g_nprods; i++) {
		struct ovmx_prod *p = &g_prods[i];
		if (!p->has_tls)
			continue;
		memcpy((char *)area + p->tls_offset,
		       (void *)p->tls_image, p->tls_filesz);
		/* .tbss stays zero (anonymous mapping). */
	}
	imgact_set_tp(area);

	for (int i = 0; i < g_nprods; i++) {
		struct ovmx_prod *p = &g_prods[i];
		if (!p->tlsdesc || p->tlsdesc->magic != OVMX_TLS_MAGIC)
			continue;
		const unsigned long *eo =
			(const unsigned long *)((const char *)p->tlsdesc +
						sizeof *p->tlsdesc);
		for (unsigned k = 0; k < p->tlsdesc->count; k++) {
			unsigned long *entry = (unsigned long *)(p->base + eo[k]);
			entry[0] = (unsigned long)__tlsdesc_static;
			entry[1] += p->tls_offset;
		}
	}
}

/* Resolve every .vms$imp import of the executable against producer symbol
 * vectors, patching the consumer's GOT cells. */
static void activate_symbol_vector(unsigned long exe_base, const char *execfn)
{
	if (!execfn)
		die_imgfmterr("IMAGE.EXE");
	int fd = (int)sys_openat(execfn, O_RDONLY);
	if (fd < 0)
		die_imgnotfnd(execfn);
	unsigned long imp_addr, imp_size;
	int ok = ovmx_find_section(fd, OVMX_IMP_SECTION, &imp_addr, &imp_size);
	/* Bias the executable's own self-relative slots (its GOT/pointer data), if
	 * any; harmless no-op for the current PLT-only executables. */
	if (ok)
		apply_vms_rel(fd, exe_base);
	sys_close(fd);
	if (!ok)
		die_imgfmterr("IMAGE.EXE");

	const struct ovmx_imp_header *ih =
		(const struct ovmx_imp_header *)(exe_base + imp_addr);
	if (ih->magic != OVMX_IMP_MAGIC)
		die_imgfmterr("IMAGE.EXE");
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
		*(unsigned long *)(exe_base + ie[k].patch_off) = addr;
	}

	/* Set up TLS for any producer that has thread-local storage, before the
	 * consumer (which may call into that producer's TLS-using code) runs. */
	setup_symvec_tls();
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
	g_auxv = auxv;

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
		activate_symbol_vector(ebias, at_execfn);
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
