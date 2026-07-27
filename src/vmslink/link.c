/*
 * link.c — LINK.EXE, the OVMX/VMS linker (MVP, bead vms-9dd, pillar vms-ade).
 *
 * Operator ruling "no unix/linux, all VMS": LINK.EXE replaces `ld` for OVMX
 * shareable images. It reads gcc's ELF relocatable objects and emits an OVMX
 * shareable image — an ELF ET_DYN that carries a `.vms$sv` SYMBOL VECTOR of
 * universal symbols (see src/vmslink/include/ovmx_image.h and
 * docs/design-link-native-toolchain.md). IMGACT (SYS$IMGACT) later binds
 * consumers to these universal symbols by vector POSITION, not by ELF hash.
 *
 * Producer side. LINK.EXE merges N relocatable ELF objects (and/or whole `ar`
 * archives — parsed in-process, no `ld -r`) into a mappable ET_DYN image: it
 * lays out text/rodata/data/bss by ELF flags, applies PC-relative relocations
 * (local + cross-object), synthesizes a GOT for GOT-indirect references and a
 * TLSDESC table for thread-local access, resolves .rela.data ABS64 pointer
 * initializers, records every image-relative slot in `.vms$rel` for load-bias,
 * and emits `.vms$sv` (declared universals) + GSMATCH. Input arrays and the
 * per-object section tables are dynamically sized, so the whole musl libc.a
 * (1345 members / 3600+ sections) ingests without a static cap. With
 * --allow-undefined, references not yet defined (compiler runtime / crt) are
 * recorded as deferred imports for the C RTL to satisfy at activation (vms-61f).
 *
 * This program is a host tool built by the normal (bootstrap) toolchain; it is
 * the tool that makes VMS images, not itself a VMS image yet.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ovmx_image.h"
#include "ovmx_symvec.h"

/* ABS64 pointer-initializer relocation (S+A written as a 64-bit word — used in
 * .rela.data for pointer tables like stdio FILE structs). Guarded. (vms-004) */
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64           257
#endif

/* Additional PC-relative .text relocation types musl-scale code emits. Guarded:
 * older <elf.h> may lack them. (vms-004) */
#ifndef R_AARCH64_LD_PREL_LO19
#define R_AARCH64_LD_PREL_LO19    273
#endif
#ifndef R_AARCH64_ADR_PREL_LO21
#define R_AARCH64_ADR_PREL_LO21   274
#endif
#ifndef R_AARCH64_TSTBR14
#define R_AARCH64_TSTBR14         279
#endif
#ifndef R_AARCH64_CONDBR19
#define R_AARCH64_CONDBR19        280
#endif

/* GOT-relative relocation types (aarch64). Guarded: older <elf.h> may lack them. */
#ifndef R_AARCH64_ADR_GOT_PAGE
#define R_AARCH64_ADR_GOT_PAGE    311
#endif
#ifndef R_AARCH64_LD64_GOT_LO12_NC
#define R_AARCH64_LD64_GOT_LO12_NC 312
#endif

/* TLSDESC (general-dynamic TLS) relocation types (aarch64). */
#ifndef R_AARCH64_TLSDESC_ADR_PAGE21
#define R_AARCH64_TLSDESC_ADR_PAGE21 562
#endif
#ifndef R_AARCH64_TLSDESC_LD64_LO12
#define R_AARCH64_TLSDESC_LD64_LO12  563
#endif
#ifndef R_AARCH64_TLSDESC_ADD_LO12
#define R_AARCH64_TLSDESC_ADD_LO12   564
#endif
#ifndef R_AARCH64_TLSDESC_CALL
#define R_AARCH64_TLSDESC_CALL       569
#endif

#define IMGACT_INTERP "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE"

/* --------------------------------------------------------------------------
 * Declared universal symbols (from SYMBOL_VECTOR=).
 * -------------------------------------------------------------------------- */
struct univ {
    char     name[256];
    uint32_t kind;          /* enum ovmx_sv_kind */
    uint64_t value;         /* image-relative address, filled during layout */
    int      resolved;
};

#define MAX_UNIV 512

static void die(const char *msg)
{
    fprintf(stderr, "%%LINK-F-ERROR, %s\n", msg);
    exit(1);
}

/* Section bucket: input sections are classified + merged by ELF flags, not by
 * exact name, so gcc's split sections (.text.unlikely, .rodata.str1.8,
 * .rodata.cst8, .data.rel.ro, ...) all land in the right output region. (vms-fa1) */
enum { B_NONE = 0, B_TEXT, B_RODATA, B_DATA, B_BSS, B_TDATA, B_TBSS };

/* One relocation, tagged with the section it patches (site = sec_va + off). */
struct reloc { uint64_t off; uint64_t info; int64_t add; int sec; };

/* --------------------------------------------------------------------------
 * Input object: slurp the file and index the ELF structures we need.
 * -------------------------------------------------------------------------- */
struct obj {
    uint8_t      *buf;
    size_t        size;
    Elf64_Ehdr   *eh;
    Elf64_Shdr   *sh;       /* section headers */
    int           nsh;
    const char   *shstr;    /* section header string table */
    Elf64_Sym    *sym;      /* .symtab */
    int           nsym;
    const char   *str;      /* .strtab */
    /* Legacy single-section handles (the *first* of each), used by the simple
     * single-.text consumer path (emit_executable). */
    int           text_ndx; /* .text section index */
    Elf64_Shdr   *text;     /* .text section header */
    int           rodata_ndx; /* .rodata section index (0 if none) */
    Elf64_Shdr   *rodata;   /* .rodata section header (0 if none) */
    int           data_ndx; /* .data section index (0 if none) */
    Elf64_Shdr   *data;     /* .data section header (0 if none) */
    int           bss_ndx;  /* .bss section index (0 if none) */
    Elf64_Shdr   *bss;      /* .bss section header (0 if none) */
    int           tdata_ndx; /* .tdata (TLS init data) index (0 if none) */
    Elf64_Shdr   *tdata;    /* .tdata section header (0 if none) */
    int           tbss_ndx; /* .tbss (TLS zero data) index (0 if none) */
    Elf64_Shdr   *tbss;     /* .tbss section header (0 if none) */
    Elf64_Rela   *rela;     /* relocations against the first .text (SHT_RELA) */
    int           nrela;
    /* Per-section classification + placement (the shareable path, vms-fa1).
     * Dynamically sized to nsh — no fixed cap, so whole-archive combines with
     * thousands of sections ingest without a %LINK-F "too many sections". (vms-004) */
    uint8_t      *sec_bucket; /* [nsh] B_* per section index (0 = unplaced) */
    uint64_t     *sec_va;     /* [nsh] assigned image vaddr, filled at layout */
    struct reloc *relocs;   /* relocs against every code AND data section */
    int           nreloc;
};

static void *xat(struct obj *o, uint64_t off, uint64_t sz, const char *what)
{
    if (off + sz > o->size)
        die(what);
    return o->buf + off;
}

/* Parse an ELF relocatable object already resident in memory. `buf` must remain
 * live for the whole run (o->buf points into it) — the caller owns/keeps it.
 * `name` is a diagnostic label (file path or "archive.a(member.o)"). (vms-004) */
static void parse_obj(struct obj *o, uint8_t *buf, size_t size, const char *name)
{
    memset(o, 0, sizeof *o);
    o->buf = buf;
    o->size = size;

    if (o->size < sizeof(Elf64_Ehdr) || memcmp(o->buf, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "%%LINK-F-ERROR, %s: input is not ELF\n", name);
        exit(1);
    }
    o->eh = (Elf64_Ehdr *)o->buf;
    if (o->eh->e_ident[EI_CLASS] != ELFCLASS64)
        die("input is not ELF64");
    if (o->eh->e_type != ET_REL)
        die("input is not a relocatable object (.o)");
    if (o->eh->e_machine != EM_AARCH64)
        die("MVP supports aarch64 objects only (x86_64 is a later bead)");

    o->sh = (Elf64_Shdr *)xat(o, o->eh->e_shoff,
                              (uint64_t)o->eh->e_shnum * sizeof(Elf64_Shdr),
                              "bad section header table");
    o->nsh = o->eh->e_shnum;
    o->shstr = (const char *)(o->buf + o->sh[o->eh->e_shstrndx].sh_offset);

    /* Per-section arrays sized to this object's section count (no fixed cap). */
    o->sec_bucket = calloc((size_t)o->nsh, 1);
    o->sec_va     = calloc((size_t)o->nsh, sizeof *o->sec_va);
    if (!o->sec_bucket || !o->sec_va) die("oom allocating per-section tables");

    /* Find .text, .symtab, .strtab. */
    for (int i = 0; i < o->nsh; i++) {
        const char *nm = o->shstr + o->sh[i].sh_name;
        if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".text") == 0) {
            o->text_ndx = i;
            o->text = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".rodata") == 0) {
            o->rodata_ndx = i;
            o->rodata = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".data") == 0) {
            o->data_ndx = i;
            o->data = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_NOBITS && strcmp(nm, ".bss") == 0) {
            o->bss_ndx = i;
            o->bss = &o->sh[i];
        } else if ((o->sh[i].sh_flags & SHF_TLS) &&
                   o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".tdata") == 0) {
            o->tdata_ndx = i;
            o->tdata = &o->sh[i];
        } else if ((o->sh[i].sh_flags & SHF_TLS) &&
                   o->sh[i].sh_type == SHT_NOBITS && strcmp(nm, ".tbss") == 0) {
            o->tbss_ndx = i;
            o->tbss = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_SYMTAB) {
            o->sym  = (Elf64_Sym *)(o->buf + o->sh[i].sh_offset);
            o->nsym = o->sh[i].sh_size / sizeof(Elf64_Sym);
            o->str  = (const char *)(o->buf + o->sh[o->sh[i].sh_link].sh_offset);
        }
    }
    /* A data-only archive member (e.g. musl's stdout.lo) legitimately has no
     * .text; only the single-object executable path requires one (checked
     * there). An archive can also carry EMPTY stub members with no symbol table
     * at all — libgcc's config-disabled objects (_trampoline.o, _xf_to_dd.o, ...)
     * are all-zero-size ELF placeholders. Such a member defines and references
     * nothing, so it contributes nothing to the link: treat it as inert (no
     * symbols, no relocations) rather than aborting, so the whole-archive C-RTL
     * build (DECC$SHR, vms-61f.1) can pull every member of libc.a AND libgcc.a
     * unconditionally. (vms-61f.1, relaxing the vms-004 hard check.) */
    if (!o->sym) { o->nsym = 0; o->nreloc = 0; o->relocs = NULL; return; }

    /* Collect relocations against the first .text (SHT_RELA), for the simple
     * consumer path. REL (implicit-addend) is not emitted by aarch64 gcc. */
    for (int i = 0; i < o->nsh; i++) {
        if (o->sh[i].sh_info != (Elf64_Word)o->text_ndx || o->sh[i].sh_size == 0)
            continue;
        if (o->sh[i].sh_type == SHT_REL)
            die("REL relocations against .text are unsupported (expected RELA)");
        if (o->sh[i].sh_type == SHT_RELA) {
            o->rela  = (Elf64_Rela *)(o->buf + o->sh[i].sh_offset);
            o->nrela = o->sh[i].sh_size / sizeof(Elf64_Rela);
        }
    }

    /* Classify every allocatable section by ELF flags (not by name), so gcc's
     * split sections merge into the right output region. (vms-fa1) */
    for (int i = 0; i < o->nsh; i++) {
        Elf64_Shdr *s = &o->sh[i];
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_flags & SHF_TLS)
            o->sec_bucket[i] = (s->sh_type == SHT_NOBITS) ? B_TBSS : B_TDATA;
        else if (s->sh_type == SHT_NOBITS)
            o->sec_bucket[i] = B_BSS;
        else if (s->sh_type == SHT_PROGBITS)
            o->sec_bucket[i] = (s->sh_flags & SHF_EXECINSTR) ? B_TEXT
                             : (s->sh_flags & SHF_WRITE)     ? B_DATA
                             :                                 B_RODATA;
        /* Other allocatable types (SHT_INIT_ARRAY, SHT_NOTE, ...) stay B_NONE;
         * a relocation into one dies loudly rather than silently misplacing. */
    }

    /* Collect relocations against every code AND data section into one flat
     * list, each tagged with the section it patches. Data-section relocations
     * are .rela.data ABS64 pointer initializers (stdio FILE structs, locale
     * ptables, *_lockptr sets, ...) — resolved + biased at emit time. (vms-004
     * folds in vms-a17; formerly only B_TEXT was collected.) */
    int cap = 0;
    for (int i = 0; i < o->nsh; i++) {
        if (o->sh[i].sh_type != SHT_RELA) continue;
        int t = (int)o->sh[i].sh_info;
        if (t >= 0 && t < o->nsh &&
            (o->sec_bucket[t] == B_TEXT || o->sec_bucket[t] == B_DATA))
            cap += o->sh[i].sh_size / sizeof(Elf64_Rela);
    }
    o->relocs = cap ? malloc((size_t)cap * sizeof(struct reloc)) : NULL;
    if (cap && !o->relocs) die("oom collecting relocations");
    o->nreloc = 0;
    for (int i = 0; i < o->nsh; i++) {
        int t = (int)o->sh[i].sh_info;
        if (t < 0 || t >= o->nsh ||
            (o->sec_bucket[t] != B_TEXT && o->sec_bucket[t] != B_DATA)) continue;
        if (o->sh[i].sh_type == SHT_REL)
            die("REL relocations are unsupported (expected RELA)");
        if (o->sh[i].sh_type != SHT_RELA) continue;
        Elf64_Rela *ra = (Elf64_Rela *)(o->buf + o->sh[i].sh_offset);
        int n = o->sh[i].sh_size / sizeof(Elf64_Rela);
        for (int j = 0; j < n; j++)
            o->relocs[o->nreloc++] = (struct reloc){
                ra[j].r_offset, ra[j].r_info, ra[j].r_addend, t };
    }
}

/* Read a whole file into a fresh malloc buffer (kept live for the run). */
static uint8_t *slurp(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input file");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat input");
    size_t sz = (size_t)st.st_size;
    uint8_t *buf = malloc(sz ? sz : 1);
    if (!buf) die("oom reading file");
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf + got, sz - got);
        if (r <= 0) die("short read input");
        got += (size_t)r;
    }
    close(fd);
    *out_size = sz;
    return buf;
}

/* Load a single relocatable object from a file path. */
static void load_obj(const char *path, struct obj *o)
{
    size_t sz;
    uint8_t *buf = slurp(path, &sz);
    parse_obj(o, buf, sz, path);
}

/* --------------------------------------------------------------------------
 * `ar` archive ingestion (whole-archive, VMS-native, in-process). (vms-004)
 *
 * Operator ruling "no unix/linux, all VMS": LINK.EXE parses the System V/GNU
 * `ar` format itself and pulls EVERY object member — never `ld -r`. Each member
 * is copied to an 8-byte-aligned buffer (aarch64-safe ELF field access) and
 * parsed like a standalone .o. The `/` symbol table and `//` long-name string
 * table members are skipped; long member names (`/<offset>`) are ignored for
 * labeling (resolution is by symbol, not filename).
 * -------------------------------------------------------------------------- */
#define AR_MAGIC "!<arch>\n"
#define AR_MAGIC_LEN 8
#define AR_HDR_SIZE  60

/* Read a right-space-padded decimal field of `len` bytes. */
static uint64_t ar_dec(const char *f, int len)
{
    uint64_t v = 0;
    for (int i = 0; i < len && f[i] >= '0' && f[i] <= '9'; i++)
        v = v * 10 + (uint64_t)(f[i] - '0');
    return v;
}

/* Grow the objs array by one and return the (zeroed) new slot. */
static struct obj *push_obj(struct obj **objs, int *n, int *cap)
{
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *objs = realloc(*objs, (size_t)*cap * sizeof(struct obj));
        if (!*objs) die("oom growing object table");
    }
    return &(*objs)[(*n)++];
}

/* Parse every object member of an `ar` archive into the growable objs array. */
static void load_archive(const char *path, struct obj **objs, int *n, int *cap)
{
    size_t asize;
    uint8_t *abuf = slurp(path, &asize);
    size_t pos = AR_MAGIC_LEN;
    int members = 0;
    while (pos + AR_HDR_SIZE <= asize) {
        const char *h = (const char *)(abuf + pos);
        if (h[58] != '`' || h[59] != '\n')
            die("malformed ar member header");
        uint64_t msize = ar_dec(h + 48, 10);
        size_t mdata = pos + AR_HDR_SIZE;
        if (mdata + msize > asize) die("ar member extends past end of archive");

        /* Member name: 16-byte field, trailing spaces (GNU trims a '/'). */
        char nm[17];
        memcpy(nm, h, 16); nm[16] = '\0';
        int e = 16; while (e > 0 && nm[e - 1] == ' ') nm[--e] = '\0';

        /* Skip the symbol table ("/"/"/SYM64/") and long-name table ("//"). */
        int is_special = (strcmp(nm, "/") == 0 || strcmp(nm, "//") == 0 ||
                          strcmp(nm, "/SYM64/") == 0);
        if (!is_special) {
            /* Copy to an 8-aligned buffer for safe ELF field access on arm64. */
            uint8_t *mb = malloc(msize ? msize : 1);
            if (!mb) die("oom copying ar member");
            memcpy(mb, abuf + mdata, msize);
            char label[300];
            snprintf(label, sizeof label, "%s(%s)", path, nm);
            struct obj *o = push_obj(objs, n, cap);
            parse_obj(o, mb, (size_t)msize, label);
            members++;
        }
        pos = mdata + msize;
        if (pos & 1) pos++;   /* members are 2-byte aligned */
    }
    free(abuf);
    fprintf(stderr, "%%LINK-I-ARCHIVE, %s: %d object member%s pulled (whole-archive)\n",
            path, members, members == 1 ? "" : "s");
}

/* --------------------------------------------------------------------------
 * Option parsing.
 * -------------------------------------------------------------------------- */
static uint32_t parse_kind(const char *k)
{
    if (strcmp(k, "PROCEDURE") == 0 || strcmp(k, "PRIVATE_PROCEDURE") == 0)
        return OVMX_SV_PROCEDURE;
    if (strcmp(k, "DATA") == 0 || strcmp(k, "PRIVATE_DATA") == 0)
        return OVMX_SV_DATA;
    die("unknown SYMBOL_VECTOR keyword (want PROCEDURE|DATA)");
    return 0;
}

/* Parse "name=PROCEDURE,name2=DATA,..." into the univ[] table. */
static int parse_symbol_vector(char *spec, struct univ *uv)
{
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(spec, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) die("SYMBOL_VECTOR entry needs name=KEYWORD");
        *eq = '\0';
        if (n >= MAX_UNIV) die("too many universal symbols");
        snprintf(uv[n].name, sizeof uv[n].name, "%s", tok);
        uv[n].kind = parse_kind(eq + 1);
        n++;
    }
    if (n == 0) die("SYMBOL_VECTOR= is empty");
    return n;
}

/* Parse GSMATCH "KEYWORD,major,minor". */
static void parse_gsmatch(char *spec, uint32_t *kind, uint32_t *maj, uint32_t *min)
{
    char *save = NULL;
    char *k = strtok_r(spec, ",", &save);
    char *a = strtok_r(NULL, ",", &save);
    char *b = strtok_r(NULL, ",", &save);
    if (!k) die("GSMATCH needs KEYWORD,major,minor");
    if (strcmp(k, "ALWAYS") == 0)      *kind = OVMX_GSMATCH_ALWAYS;
    else if (strcmp(k, "EQUAL") == 0)  *kind = OVMX_GSMATCH_EQUAL;
    else if (strcmp(k, "LEQUAL") == 0) *kind = OVMX_GSMATCH_LEQUAL;
    else die("GSMATCH keyword must be ALWAYS|EQUAL|LEQUAL");
    *maj = a ? (uint32_t)strtoul(a, NULL, 0) : 0;
    *min = b ? (uint32_t)strtoul(b, NULL, 0) : 0;
}

/* --------------------------------------------------------------------------
 * Image emit.
 * -------------------------------------------------------------------------- */
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define PAGE 0x1000u

/* --------------------------------------------------------------------------
 * Consumer/executable linking: bind imports to producer symbol vectors.
 * -------------------------------------------------------------------------- */

/* aarch64 instruction encoders for the PLT stub + call patch. */
static uint32_t enc_adrp(int rd, int64_t page_delta)
{
    uint32_t immlo = (uint32_t)(page_delta & 0x3);
    uint32_t immhi = (uint32_t)((page_delta >> 2) & 0x7FFFF);
    return 0x90000000u | (immlo << 29) | (immhi << 5) | (uint32_t)rd;
}
static uint32_t enc_ldr_u64(int rt, int rn, uint32_t off /*8-aligned*/)
{
    return 0xF9400000u | ((off / 8) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_br(int rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }

/* A loaded producer shareable image (read to bind universal symbols). */
struct producer {
    char                    name[256];   /* soname used in .vms$imp (basename) */
    uint8_t                *buf;
    struct ovmx_sv_header  *sv;
};

static void load_producer(const char *path, struct producer *p)
{
    memset(p, 0, sizeof *p);
    const char *base = strrchr(path, '/');
    snprintf(p->name, sizeof p->name, "%s", base ? base + 1 : path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open producer image (--use)");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat producer");
    p->buf = malloc(st.st_size);
    if (!p->buf || read(fd, p->buf, st.st_size) != (ssize_t)st.st_size)
        die("read producer");
    close(fd);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)p->buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_type != ET_DYN)
        die("producer is not an OVMX shareable image (ET_DYN)");
    Elf64_Shdr *sh = (Elf64_Shdr *)(p->buf + eh->e_shoff);
    const char *shstr = (const char *)(p->buf + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; i++)
        if (strcmp(shstr + sh[i].sh_name, OVMX_SV_SECTION) == 0)
            p->sv = (struct ovmx_sv_header *)(p->buf + sh[i].sh_offset);
    if (!p->sv || p->sv->magic != OVMX_SV_MAGIC)
        die("producer image has no .vms$sv symbol vector");
}

/* Find universal `name` across producers -> (producer index, vector index). */
static int find_universal(struct producer *ps, int np, const char *name,
                          int *pidx, uint32_t *svidx)
{
    for (int p = 0; p < np; p++) {
        const struct ovmx_sv_entry *e = ovmx_sv_entries(ps[p].sv);
        const char *nm = ovmx_sv_names(ps[p].sv);
        for (uint32_t i = 0; i < ps[p].sv->count; i++) {
            if (e[i].kind == OVMX_SV_RETIRED) continue;
            if (strcmp(nm + e[i].name_off, name) == 0) {
                *pidx = p; *svidx = i; return 1;
            }
        }
    }
    return 0;
}

struct import {
    char     name[256];
    int      pidx;         /* producer index */
    uint32_t svidx;        /* vector index within that producer */
    uint64_t plt_va;       /* PLT stub address (assigned at layout) */
    uint64_t got_va;       /* GOT cell address (assigned at layout) */
    int      is_data;      /* 1 = DATA import (GOT-read), 0 = call import (PLT) */
};

/* Patch an ADR_GOT_PAGE / LD64_GOT_LO12_NC pair to reach `slot` PC-relatively
 * (defined below; forward-declared for emit_executable's data imports). */
static void patch_got(uint32_t type, uint32_t *insn, uint64_t site, uint64_t slot);

/* Emit an OVMX executable image (ET_DYN, PT_INTERP=IMGACT.EXE) whose imports
 * bind to producer symbol vectors via .vms$imp. IMGACT resolves each import at
 * activation (ovmx_symvec.h) and writes the address into the GOT cell. */
static void emit_executable(struct obj *o, struct producer *ps, int np,
                            const char *out)
{
    /* Resolve _start. */
    uint64_t start_off = 0; int start_found = 0;
    for (int i = 0; i < o->nsym; i++)
        if (strcmp(o->str + o->sym[i].st_name, "_start") == 0 &&
            o->sym[i].st_shndx == (Elf64_Section)o->text_ndx) {
            start_off = o->sym[i].st_value; start_found = 1;
        }
    if (!start_found) die("executable object has no _start in .text");

    /* Collect imports: CALL26/JUMP26 -> PLT (call) import; ADR_GOT_PAGE /
     * LD64_GOT_LO12_NC to an undefined symbol -> GOT (DATA) import. Both bind to
     * a producer universal via .vms$imp; IMGACT fills the GOT cell. */
    struct import imp[256];
    int nimp = 0;
    for (int i = 0; i < o->nrela; i++) {
        uint32_t type = ELF64_R_TYPE(o->rela[i].r_info);
        uint32_t si   = ELF64_R_SYM(o->rela[i].r_info);
        const char *nm = o->str + o->sym[si].st_name;
        int is_call = (type == R_AARCH64_CALL26 || type == R_AARCH64_JUMP26);
        int is_data = (type == R_AARCH64_ADR_GOT_PAGE ||
                       type == R_AARCH64_LD64_GOT_LO12_NC);
        if (!is_call && !is_data)
            die("consumer supports only CALL26/JUMP26 (calls) and GOT (data) relocs in .text");
        if (o->sym[si].st_shndx != SHN_UNDEF)
            die("consumer supports only external (imported) references");
        int found = -1;
        for (int k = 0; k < nimp; k++)
            if (strcmp(imp[k].name, nm) == 0) found = k;
        if (found < 0) {
            if (nimp >= 256) die("too many imports");
            found = nimp++;
            snprintf(imp[found].name, sizeof imp[found].name, "%s", nm);
            imp[found].is_data = is_data;
            if (!find_universal(ps, np, nm, &imp[found].pidx, &imp[found].svidx))
                die("unresolved universal symbol (not in any --use image)");
        }
    }
    if (nimp == 0) die("consumer imports nothing (no external references to bind)");

    /* ---- Layout ----
     * [ehdr][phdr x2][.interp][.text][.plt][.got][.vms$imp][.shstrtab][shdrs] */
    uint64_t off_eh     = 0;
    uint64_t off_ph     = sizeof(Elf64_Ehdr);
    int      nph        = 3;   /* PT_PHDR, PT_INTERP, PT_LOAD */
    uint64_t off_interp = off_ph + nph * sizeof(Elf64_Phdr);
    uint64_t interp_sz  = strlen(IMGACT_INTERP) + 1;
    uint64_t off_text   = ALIGN_UP(off_interp + interp_sz, 16);
    uint64_t text_sz    = o->text->sh_size;
    uint64_t off_plt    = ALIGN_UP(off_text + text_sz, 4);
    uint64_t plt_sz     = (uint64_t)nimp * 12;
    uint64_t off_got    = ALIGN_UP(off_plt + plt_sz, 8);
    uint64_t got_sz     = (uint64_t)nimp * 8;
    uint64_t off_imp    = ALIGN_UP(off_got + got_sz, 8);

    /* .vms$imp: dedup producer sonames into a name blob. */
    char     names[4096]; uint32_t names_sz = 0;
    uint32_t prod_off[64];
    for (int p = 0; p < np; p++) {
        prod_off[p] = names_sz;
        size_t l = strlen(ps[p].name) + 1;
        memcpy(names + names_sz, ps[p].name, l);
        names_sz += (uint32_t)l;
    }
    uint64_t imp_hdr    = sizeof(struct ovmx_imp_header);
    uint64_t imp_ents   = (uint64_t)nimp * sizeof(struct ovmx_imp_entry);
    uint64_t imp_names_o = imp_hdr + imp_ents;
    uint64_t imp_size   = imp_names_o + names_sz;

    uint64_t loaded_end = off_imp + imp_size;
    uint64_t off_shstr  = ALIGN_UP(loaded_end, 4);
    const char *secn[] = { "", ".interp", ".text", ".plt", ".got",
                           OVMX_IMP_SECTION, ".shstrtab" };
    int nsec = 7;
    uint64_t sn_off[7]; uint64_t sn_sz = 0;
    for (int i = 0; i < nsec; i++) { sn_off[i] = sn_sz; sn_sz += strlen(secn[i]) + 1; }
    uint64_t off_shdr = ALIGN_UP(off_shstr + sn_sz, 8);
    uint64_t file_sz  = off_shdr + (uint64_t)nsec * sizeof(Elf64_Shdr);

    uint8_t *img = calloc(1, file_sz);
    if (!img) die("oom building executable");

    /* vaddr == file offset (single identity-mapped PT_LOAD). */
    uint64_t text_va = off_text, plt_va = off_plt, got_va = off_got;
    for (int i = 0; i < nimp; i++) {
        imp[i].plt_va = plt_va + (uint64_t)i * 12;
        imp[i].got_va = got_va + (uint64_t)i * 8;
    }

    /* ELF header */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)(img + off_eh);
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA]  = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_DYN; eh->e_machine = EM_AARCH64; eh->e_version = EV_CURRENT;
    eh->e_entry = text_va + start_off;
    eh->e_phoff = off_ph; eh->e_shoff = off_shdr;
    eh->e_ehsize = sizeof *eh; eh->e_phentsize = sizeof(Elf64_Phdr); eh->e_phnum = nph;
    eh->e_shentsize = sizeof(Elf64_Shdr); eh->e_shnum = nsec; eh->e_shstrndx = 6;

    /* Program headers: PT_PHDR (so the activator can derive the load bias),
     * PT_INTERP, PT_LOAD (RWX: GOT is written at activation). */
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + off_ph);
    ph[0].p_type = PT_PHDR; ph[0].p_flags = PF_R;
    ph[0].p_offset = off_ph; ph[0].p_vaddr = off_ph; ph[0].p_paddr = off_ph;
    ph[0].p_filesz = (uint64_t)nph * sizeof(Elf64_Phdr);
    ph[0].p_memsz  = ph[0].p_filesz; ph[0].p_align = 8;
    ph[1].p_type = PT_INTERP; ph[1].p_flags = PF_R;
    ph[1].p_offset = off_interp; ph[1].p_vaddr = off_interp; ph[1].p_paddr = off_interp;
    ph[1].p_filesz = interp_sz; ph[1].p_memsz = interp_sz; ph[1].p_align = 1;
    ph[2].p_type = PT_LOAD; ph[2].p_flags = PF_R | PF_W | PF_X;
    ph[2].p_offset = 0; ph[2].p_vaddr = 0; ph[2].p_paddr = 0;
    ph[2].p_filesz = loaded_end; ph[2].p_memsz = loaded_end; ph[2].p_align = PAGE;

    memcpy(img + off_interp, IMGACT_INTERP, interp_sz);
    memcpy(img + off_text, o->buf + o->text->sh_offset, text_sz);

    /* Patch reference sites: calls branch to a PLT stub; DATA reads (the GOT
     * ADRP/LDR pair) address the import's GOT cell directly. */
    for (int i = 0; i < o->nrela; i++) {
        uint32_t type = ELF64_R_TYPE(o->rela[i].r_info);
        const char *nm = o->str + o->sym[ELF64_R_SYM(o->rela[i].r_info)].st_name;
        int k = -1;
        for (int j = 0; j < nimp; j++) if (strcmp(imp[j].name, nm) == 0) k = j;
        uint64_t site = text_va + o->rela[i].r_offset;
        uint32_t *insn = (uint32_t *)(img + off_text + o->rela[i].r_offset);
        if (type == R_AARCH64_ADR_GOT_PAGE || type == R_AARCH64_LD64_GOT_LO12_NC) {
            patch_got(type, insn, site, imp[k].got_va);
        } else {
            int64_t  disp = (int64_t)imp[k].plt_va - (int64_t)site;
            uint32_t imm26 = (uint32_t)((disp >> 2) & 0x03FFFFFF);
            uint32_t op = (type == R_AARCH64_JUMP26) ? 0x14000000u : 0x94000000u;
            *insn = op | imm26;
        }
    }

    /* PLT stubs for call imports: adrp x16,GOT ; ldr x16,[x16,#lo12] ; br x16.
     * (DATA imports leave their stub bytes unused — the site reads the GOT cell
     * directly — but a slot is still reserved to keep the layout uniform.) */
    for (int i = 0; i < nimp; i++) {
        if (imp[i].is_data) continue;
        uint32_t *s = (uint32_t *)(img + off_plt + (uint64_t)i * 12);
        int64_t pd = (int64_t)(imp[i].got_va >> 12) - (int64_t)(imp[i].plt_va >> 12);
        s[0] = enc_adrp(16, pd);
        s[1] = enc_ldr_u64(16, 16, (uint32_t)(imp[i].got_va & 0xfff));
        s[2] = enc_br(16);
    }
    /* GOT cells left zero; IMGACT fills them at activation. */

    /* .vms$imp */
    struct ovmx_imp_header *ih = (struct ovmx_imp_header *)(img + off_imp);
    ih->magic = OVMX_IMP_MAGIC; ih->count = nimp;
    ih->names_off = (uint32_t)imp_names_o; ih->names_size = names_sz;
    struct ovmx_imp_entry *ie =
        (struct ovmx_imp_entry *)(img + off_imp + imp_hdr);
    for (int i = 0; i < nimp; i++) {
        ie[i].producer_off = prod_off[imp[i].pidx];
        ie[i].sv_index = imp[i].svidx;
        ie[i].patch_off = imp[i].got_va;
        /* Record the producer version we linked against, for GSMATCH. */
        ie[i].req_major = ps[imp[i].pidx].sv->gsmatch_major;
        ie[i].req_minor = ps[imp[i].pidx].sv->gsmatch_minor;
    }
    memcpy(img + off_imp + imp_names_o, names, names_sz);

    /* .shstrtab + section headers */
    char *shstr = (char *)(img + off_shstr);
    for (int i = 0; i < nsec; i++) memcpy(shstr + sn_off[i], secn[i], strlen(secn[i]) + 1);
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + off_shdr);
    struct { int idx; uint32_t type; uint64_t flags, addr, off, size, align; } S[] = {
        { 1, SHT_PROGBITS, SHF_ALLOC,                    off_interp, off_interp, interp_sz, 1 },
        { 2, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,    text_va,    off_text,   text_sz,   16 },
        { 3, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,    plt_va,     off_plt,    plt_sz,    4 },
        { 4, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,        got_va,     off_got,    got_sz,    8 },
        { 5, SHT_PROGBITS, SHF_ALLOC,                    off_imp,    off_imp,    imp_size,  8 },
        { 6, SHT_STRTAB,   0,                            0,          off_shstr,  sn_sz,     1 },
    };
    for (unsigned i = 0; i < sizeof S / sizeof S[0]; i++) {
        sh[S[i].idx].sh_name = sn_off[S[i].idx];
        sh[S[i].idx].sh_type = S[i].type;
        sh[S[i].idx].sh_flags = S[i].flags;
        sh[S[i].idx].sh_addr = S[i].addr;
        sh[S[i].idx].sh_offset = S[i].off;
        sh[S[i].idx].sh_size = S[i].size;
        sh[S[i].idx].sh_addralign = S[i].align;
    }

    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) die("cannot create output image");
    if (write(fd, img, file_sz) != (ssize_t)file_sz) die("short write output");
    close(fd);
    fprintf(stderr,
        "%%LINK-S-CREATED, %s: ET_DYN executable image, %d import%s, "
        "PT_INTERP=%s\n",
        out, nimp, nimp == 1 ? "" : "s", IMGACT_INTERP);
    free(img);
}

/* Patch one PC-relative relocation instruction to reach `target`. */
static void patch_pcrel(uint32_t type, uint32_t *insn, uint64_t site, uint64_t target)
{
    switch (type) {
    case R_AARCH64_CALL26:
    case R_AARCH64_JUMP26: {
        int64_t d = (int64_t)target - (int64_t)site;
        *insn = (*insn & ~0x03FFFFFFu) | (uint32_t)((d >> 2) & 0x03FFFFFF);
        break;
    }
    case R_AARCH64_ADR_PREL_PG_HI21: {
        int64_t d = (int64_t)(target >> 12) - (int64_t)(site >> 12);
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
        break;
    }
    case R_AARCH64_ADR_PREL_LO21: {   /* adr Xd, sym : 21-bit byte displacement */
        int64_t d = (int64_t)target - (int64_t)site;
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
        break;
    }
    case R_AARCH64_CONDBR19:          /* b.cond / cbz / cbnz : imm19 << 5, *4 */
    case R_AARCH64_LD_PREL_LO19: {    /* ldr (literal)      : imm19 << 5, *4 */
        int64_t d = ((int64_t)target - (int64_t)site) >> 2;
        *insn = (*insn & ~(0x7FFFFu << 5)) | (((uint32_t)d & 0x7FFFF) << 5);
        break;
    }
    case R_AARCH64_TSTBR14: {         /* tbz / tbnz : imm14 << 5, *4 */
        int64_t d = ((int64_t)target - (int64_t)site) >> 2;
        *insn = (*insn & ~(0x3FFFu << 5)) | (((uint32_t)d & 0x3FFF) << 5);
        break;
    }
    case R_AARCH64_ADD_ABS_LO12_NC:
        *insn = (*insn & ~(0xFFFu << 10)) | (((uint32_t)target & 0xFFF) << 10);
        break;
    case R_AARCH64_LDST8_ABS_LO12_NC:
    case R_AARCH64_LDST16_ABS_LO12_NC:
    case R_AARCH64_LDST32_ABS_LO12_NC:
    case R_AARCH64_LDST64_ABS_LO12_NC:
    case R_AARCH64_LDST128_ABS_LO12_NC: {
        int scale = type == R_AARCH64_LDST8_ABS_LO12_NC  ? 0 :
                    type == R_AARCH64_LDST16_ABS_LO12_NC ? 1 :
                    type == R_AARCH64_LDST32_ABS_LO12_NC ? 2 :
                    type == R_AARCH64_LDST64_ABS_LO12_NC ? 3 : 4;
        uint32_t imm = (uint32_t)((target & 0xFFF) >> scale);
        *insn = (*insn & ~(0xFFFu << 10)) | (imm << 10);
        break;
    }
    default:
        die("unsupported .text relocation (need a PC-relative type)");
    }
}

/* --------------------------------------------------------------------------
 * Global defined-symbol hash. Whole-archive musl links 1345 members with tens
 * of thousands of cross-object references; a linear symbol scan per reference
 * is O(refs · members · syms) and does not finish under emulation. A single
 * name -> (object, symbol) hash makes every cross-object resolution O(1) and is
 * what lets LINK.EXE ingest the whole archive in seconds, not minutes. (vms-004)
 * -------------------------------------------------------------------------- */
struct symref { const char *name; int oi; int ki; unsigned char bind; };
static struct symref *g_syms;
static size_t g_syms_cap;          /* power of two */

/* Undefined-external policy: with --allow-undefined a reference not defined by
 * any input object is a *deferred import* (satisfied later by the C RTL / a
 * companion shareable — vms-61f), recorded loudly rather than aborting. Off by
 * default: a normal single-shareable build still fails hard on a dangling ref. */
static int  g_allow_undef;
static long g_deferred;            /* count of deferred (unresolved) externals */

/* Names referenced with a WEAK undefined reference and defined by NO input
 * object. Standard ELF semantics resolve a weak undefined symbol to address 0 —
 * it is NOT a deferred import and NOT an error. For DECC$SHR these are exactly
 * the linker-defined section-boundary symbols __init_array_start/__init_array_end,
 * __fini_array_start/__fini_array_end and _DYNAMIC: musl's libc.a + libgcc.a
 * carry no .init_array/.fini_array sections and no dynamic section, so an empty
 * (start == end == 0) constructor range and a null _DYNAMIC are the *correct*
 * values — the C-RTL startup loop iterates zero static constructors. Resolving
 * them to 0 is what lets DECC$SHR link with ZERO deferred externals. (vms-61f.1) */
static char **g_weak;
static int    g_nweak;

static int weak_has(const char *name)
{
    for (int i = 0; i < g_nweak; i++)
        if (strcmp(g_weak[i], name) == 0) return 1;
    return 0;
}

static void weak_add(const char *name)
{
    if (weak_has(name)) return;
    g_weak = realloc(g_weak, (size_t)(g_nweak + 1) * sizeof *g_weak);
    if (!g_weak) die("oom recording weak-undefined symbol");
    g_weak[g_nweak++] = strdup(name);
}

static size_t djb2(const char *s)
{
    size_t h = 5381; int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (size_t)c;
    return h;
}

/* Insert a defined global; prefer a STRONG (GLOBAL) def over a WEAK one. */
static void sym_insert(const char *name, int oi, int ki, unsigned char bind)
{
    size_t mask = g_syms_cap - 1;
    size_t i = djb2(name) & mask;
    for (;;) {
        if (!g_syms[i].name) {
            g_syms[i] = (struct symref){ name, oi, ki, bind };
            return;
        }
        if (strcmp(g_syms[i].name, name) == 0) {
            if (g_syms[i].bind == STB_WEAK && bind == STB_GLOBAL)
                g_syms[i] = (struct symref){ name, oi, ki, bind };  /* strong wins */
            return;
        }
        i = (i + 1) & mask;
    }
}

/* Look up a defined global by name -> (object index, symbol index), or -1. */
static int sym_lookup(const char *name, int *oi, int *ki)
{
    if (!g_syms) return 0;
    size_t mask = g_syms_cap - 1;
    size_t i = djb2(name) & mask;
    for (;;) {
        if (!g_syms[i].name) return 0;
        if (strcmp(g_syms[i].name, name) == 0) {
            *oi = g_syms[i].oi; *ki = g_syms[i].ki; return 1;
        }
        i = (i + 1) & mask;
    }
}

/* Build the hash over every defined (non-LOCAL, non-UNDEF) global symbol. */
static void build_symhash(struct obj *objs, int nobj)
{
    size_t total = 0;
    for (int i = 0; i < nobj; i++) total += (size_t)objs[i].nsym;
    size_t cap = 64;
    while (cap < total * 2 + 16) cap <<= 1;
    g_syms_cap = cap;
    g_syms = calloc(cap, sizeof *g_syms);
    if (!g_syms) die("oom building symbol hash");
    for (int i = 0; i < nobj; i++)
        for (int k = 0; k < objs[i].nsym; k++) {
            Elf64_Sym *s = &objs[i].sym[k];
            unsigned char bind = ELF64_ST_BIND(s->st_info);
            if (bind == STB_LOCAL || s->st_shndx == SHN_UNDEF) continue;
            const char *nm = objs[i].str + s->st_name;
            if (!nm[0]) continue;
            sym_insert(nm, i, k, bind);
        }
    /* Second pass: a WEAK undefined reference to a symbol no object defines
     * resolves to address 0 (ELF weak-undef semantics). Record such names so the
     * resolver returns 0 legitimately instead of aborting / deferring. (vms-61f.1) */
    for (int i = 0; i < nobj; i++)
        for (int k = 0; k < objs[i].nsym; k++) {
            Elf64_Sym *s = &objs[i].sym[k];
            if (s->st_shndx != SHN_UNDEF) continue;
            if (ELF64_ST_BIND(s->st_info) != STB_WEAK) continue;
            const char *nm = objs[i].str + s->st_name;
            if (!nm[0]) continue;
            int doi, dki;
            if (!sym_lookup(nm, &doi, &dki)) weak_add(nm);
        }
}

/* Map a symbol defined in object `d` to its final image vaddr, using the
 * per-section placement filled at layout. Returns 0 for a symbol in a section
 * this linker does not place flat (unplaced, or TLS — addressed via TLSDESC). */
static uint64_t placed_addr(struct obj *d, Elf64_Sym *s)
{
    int sh = (int)s->st_shndx;
    if (sh <= 0 || sh >= d->nsh) return 0;
    switch (d->sec_bucket[sh]) {
    case B_TEXT: case B_RODATA: case B_DATA: case B_BSS:
        return d->sec_va[sh] + s->st_value;
    default:
        return 0;
    }
}

/* Resolve a relocation's symbol to a final image vaddr (local or cross-object).
 * Returns 0 for a deferred external under --allow-undefined (caller skips the
 * patch); otherwise dies on a dangling reference. */
static uint64_t resolve_ref(struct obj *objs, int nobj, int oi, uint32_t symidx)
{
    struct obj *o = &objs[oi];
    Elf64_Sym *s = &o->sym[symidx];
    /* Weak-override (ELF symbol resolution): a WEAK symbol DEFINED in this
     * object must yield to a STRONG global definition of the same name in
     * another object. A relocation whose symtab entry is the locally-defined
     * WEAK symbol must therefore bind to the strong def, not the local weak one.
     *
     * This is load-bearing for whole-archive musl: lite_malloc.lo defines
     * __libc_malloc_impl as a WEAK alias of its __simple_malloc bump allocator,
     * and mallocng's malloc.lo defines __libc_malloc_impl STRONG. gcc emits the
     * intra-object JUMP26 in __libc_malloc / malloc against the *locally-defined
     * weak* __libc_malloc_impl, so binding it to the local address would route
     * the exported malloc to the simple allocator while free stays mallocng —
     * the freed pointer then carries no mallocng metadata and free's get_meta
     * dereferences p[-4] off the mapping (SIGSEGV). build_symhash already keeps
     * the STRONG def for a name, so resolving weak-defined references by name
     * through the global hash restores correct override. (vms-36a) */
    if (s->st_shndx != SHN_UNDEF &&
        ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        const char *nm = o->str + s->st_name;
        int doi, dki;
        if (nm[0] && sym_lookup(nm, &doi, &dki) &&
            ELF64_ST_BIND(objs[doi].sym[dki].st_info) == STB_GLOBAL) {
            uint64_t da = placed_addr(&objs[doi], &objs[doi].sym[dki]);
            if (da) return da;
        }
    }
    uint64_t a = placed_addr(o, s);
    if (a) return a;
    if (s->st_shndx == SHN_UNDEF) {
        const char *nm = o->str + s->st_name;
        if (!nm[0]) die("undefined unnamed symbol in relocation");
        int doi, dki;
        if (sym_lookup(nm, &doi, &dki)) {
            uint64_t da = placed_addr(&objs[doi], &objs[doi].sym[dki]);
            if (da) return da;
        }
        if (weak_has(nm)) return 0;   /* weak-undef resolves to 0 (ELF semantics) */
        if (g_allow_undef) { g_deferred++; return 0; }
        die("unresolved external symbol (needs the C RTL -- vms-61f; "
            "pass --allow-undefined to record it as a deferred import)");
    }
    (void)nobj;
    /* Defined, but in a section this linker doesn't place flat (TLS, or an
     * allocatable type like SHT_INIT_ARRAY): a pointer into it is deferred
     * under --allow-undefined, otherwise a hard error. */
    if (g_allow_undef) { g_deferred++; return 0; }
    die("relocation against an unsupported section");
    return 0;
}

/* Resolve a symbol by NAME (global def) -> image vaddr. Returns 0 when not
 * defined by any input object (caller decides: die for a declared universal,
 * defer for a GOT import under --allow-undefined). */
static uint64_t resolve_named(struct obj *objs, int nobj,
                              const char *name, const char *whaterr)
{
    (void)nobj;
    int doi, dki;
    if (sym_lookup(name, &doi, &dki)) {
        uint64_t da = placed_addr(&objs[doi], &objs[doi].sym[dki]);
        if (da) return da;
    }
    if (whaterr) die(whaterr);
    return 0;
}

/* A synthesized GOT slot: one per distinct symbol referenced GOT-indirectly. */
struct gotslot { char name[256]; uint64_t va; uint64_t value; };

static int find_got(struct gotslot *g, int ng, const char *name)
{
    for (int i = 0; i < ng; i++) if (strcmp(g[i].name, name) == 0) return i;
    return -1;
}

/* Patch an ADR_GOT_PAGE / LD64_GOT_LO12_NC pair to reach `slot` PC-relatively.
 * Identical bit-layout to ADR_PREL_PG_HI21 / LDST64_ABS_LO12_NC, but the target
 * is the GOT cell rather than the symbol. */
static void patch_got(uint32_t type, uint32_t *insn, uint64_t site, uint64_t slot)
{
    if (type == R_AARCH64_ADR_GOT_PAGE) {
        int64_t d = (int64_t)(slot >> 12) - (int64_t)(site >> 12);
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
    } else { /* R_AARCH64_LD64_GOT_LO12_NC: 8-byte load, scale 3 */
        uint32_t imm = (uint32_t)((slot & 0xFFF) >> 3);
        *insn = (*insn & ~(0xFFFu << 10)) | (imm << 10);
    }
}

static int is_got_reloc(uint32_t type)
{
    return type == R_AARCH64_ADR_GOT_PAGE || type == R_AARCH64_LD64_GOT_LO12_NC;
}

/* A synthesized TLSDESC entry (two quadwords): [0]=resolver (IMGACT fills with
 * __tlsdesc_static), [1]=TP-relative offset (LINK pre-fills the module-relative
 * part; IMGACT adds the module's assigned TLS block offset). */
struct tlsslot { char name[256]; int64_t addend; uint64_t va; uint64_t modoff; };

static int find_tls(struct tlsslot *t, int nt, const char *name)
{
    for (int i = 0; i < nt; i++) if (strcmp(t[i].name, name) == 0) return i;
    return -1;
}

static int is_tlsdesc_reloc(uint32_t type)
{
    return type == R_AARCH64_TLSDESC_ADR_PAGE21 ||
           type == R_AARCH64_TLSDESC_LD64_LO12 ||
           type == R_AARCH64_TLSDESC_ADD_LO12 ||
           type == R_AARCH64_TLSDESC_CALL;
}

/* Patch a TLSDESC ADR_PAGE21 / LD64_LO12 / ADD_LO12 to reach the 2-word TLSDESC
 * entry PC-relatively (same encodings as ADRP / LDR64 / ADD-imm12). TLSDESC_CALL
 * is a marker at the blr and needs no patch. */
static void patch_tlsdesc(uint32_t type, uint32_t *insn, uint64_t site, uint64_t slot)
{
    if (type == R_AARCH64_TLSDESC_ADR_PAGE21) {
        int64_t d = (int64_t)(slot >> 12) - (int64_t)(site >> 12);
        uint32_t immlo = (uint32_t)(d & 3), immhi = (uint32_t)((d >> 2) & 0x7FFFF);
        *insn = (*insn & ~((3u << 29) | (0x7FFFFu << 5))) | (immlo << 29) | (immhi << 5);
    } else if (type == R_AARCH64_TLSDESC_LD64_LO12) {
        uint32_t imm = (uint32_t)((slot & 0xFFF) >> 3);
        *insn = (*insn & ~(0xFFFu << 10)) | (imm << 10);
    } else if (type == R_AARCH64_TLSDESC_ADD_LO12) {
        *insn = (*insn & ~(0xFFFu << 10)) | (((uint32_t)slot & 0xFFF) << 10);
    }
    /* R_AARCH64_TLSDESC_CALL: no-op. */
}

/* Module-relative TLS offset of a TLS symbol: 0-based within the module's
 * [.tdata | .tbss] block. tbss_base is where .tbss begins (aligned .tdata size). */
static uint64_t tls_module_offset(struct obj *objs, int nobj, uint64_t tbss_base,
                                  const char *name, int64_t addend)
{
    for (int j = 0; j < nobj; j++) {
        struct obj *d = &objs[j];
        for (int k = 0; k < d->nsym; k++) {
            Elf64_Sym *s = &d->sym[k];
            if (strcmp(d->str + s->st_name, name) != 0) continue;
            if (d->tdata && s->st_shndx == (Elf64_Section)d->tdata_ndx)
                return s->st_value + (uint64_t)addend;
            if (d->tbss && s->st_shndx == (Elf64_Section)d->tbss_ndx)
                return tbss_base + s->st_value + (uint64_t)addend;
        }
    }
    die("TLS symbol not defined in any input .tdata/.tbss");
    return 0;
}

/* Emit an OVMX shareable image from N objects: merge .text/.rodata/.data/.bss,
 * apply PC-relative relocations (local + cross-object), synthesize a GOT for
 * GOT-indirect global references, and record every image-relative slot that
 * needs +load_bias in .vms$rel. Exports declared universals via .vms$sv. (vms-20b) */
static void emit_shareable(struct obj *objs, int nobj, struct univ *uv, int nuniv,
                           uint32_t gk, uint32_t gmaj, uint32_t gmin,
                           int allow_undef, const char *out)
{
    g_allow_undef = allow_undef;
    g_deferred = 0;
    build_symhash(objs, nobj);

    int has_ro = 0, has_data = 0, has_bss = 0;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++) {
            if (objs[i].sh[s].sh_size == 0) continue;
            if (objs[i].sec_bucket[s] == B_RODATA) has_ro = 1;
            if (objs[i].sec_bucket[s] == B_DATA)   has_data = 1;
            if (objs[i].sec_bucket[s] == B_BSS)    has_bss = 1;
        }

    /* TLS geometry (single TLS-bearing object per image for now). */
    int tls_obj = -1;
    for (int i = 0; i < nobj; i++) {
        if ((objs[i].tdata && objs[i].tdata->sh_size) ||
            (objs[i].tbss && objs[i].tbss->sh_size)) {
            if (tls_obj >= 0)
                die("multi-module TLS not supported yet (one TLS object per image)");
            tls_obj = i;
        }
    }
    int has_tls = (tls_obj >= 0);
    uint64_t tdata_sz = (has_tls && objs[tls_obj].tdata) ? objs[tls_obj].tdata->sh_size : 0;
    uint64_t tbss_sz  = (has_tls && objs[tls_obj].tbss)  ? objs[tls_obj].tbss->sh_size  : 0;
    uint64_t tdata_al = (has_tls && objs[tls_obj].tdata && objs[tls_obj].tdata->sh_addralign)
                        ? objs[tls_obj].tdata->sh_addralign : 8;
    uint64_t tbss_al  = (has_tls && objs[tls_obj].tbss && objs[tls_obj].tbss->sh_addralign)
                        ? objs[tls_obj].tbss->sh_addralign : 1;
    uint64_t tls_align = tdata_al > tbss_al ? tdata_al : tbss_al;
    if (tls_align == 0) tls_align = 1;
    uint64_t tbss_base = tbss_sz ? ALIGN_UP(tdata_sz, tbss_al) : tdata_sz;
    uint64_t tls_memsz = tbss_base + tbss_sz;

    /* Collect the distinct GOT-referenced symbols (across all code sections).
     * Growable — musl references hundreds of globals GOT-indirectly. (vms-004) */
    struct gotslot *got = NULL; int ngot = 0, got_cap = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++) {
            uint32_t type = ELF64_R_TYPE(objs[i].relocs[r].info);
            if (!is_got_reloc(type)) continue;
            uint32_t si = ELF64_R_SYM(objs[i].relocs[r].info);
            const char *nm = objs[i].str + objs[i].sym[si].st_name;
            if (find_got(got, ngot, nm) < 0) {
                if (ngot >= got_cap) {
                    got_cap = got_cap ? got_cap * 2 : 256;
                    got = realloc(got, (size_t)got_cap * sizeof *got);
                    if (!got) die("oom growing GOT table");
                }
                snprintf(got[ngot].name, sizeof got[ngot].name, "%s", nm);
                ngot++;
            }
        }

    /* Collect the distinct TLSDESC-referenced symbols (one 2-word entry each). */
    struct tlsslot *tls = NULL; int ntls = 0, tls_cap = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++) {
            uint32_t type = ELF64_R_TYPE(objs[i].relocs[r].info);
            if (!is_tlsdesc_reloc(type)) continue;
            uint32_t si = ELF64_R_SYM(objs[i].relocs[r].info);
            const char *nm = objs[i].str + objs[i].sym[si].st_name;
            if (find_tls(tls, ntls, nm) < 0) {
                if (ntls >= tls_cap) {
                    tls_cap = tls_cap ? tls_cap * 2 : 64;
                    tls = realloc(tls, (size_t)tls_cap * sizeof *tls);
                    if (!tls) die("oom growing TLSDESC table");
                }
                snprintf(tls[ntls].name, sizeof tls[ntls].name, "%s", nm);
                tls[ntls].addend = objs[i].relocs[r].add;
                ntls++;
            }
        }

    /* Count ABS64 data-pointer relocations up front — each needs a .vms$rel
     * slot (image-relative pointer biased at activation), so the section must
     * be sized before layout. (vms-004) */
    int nabs = 0;
    for (int i = 0; i < nobj; i++)
        for (int r = 0; r < objs[i].nreloc; r++)
            if (ELF64_R_TYPE(objs[i].relocs[r].info) == R_AARCH64_ABS64)
                nabs++;

    /* ---- Layout: [ehdr][phdr] text|rodata|got|tlsdesc|data|tdata|sv|rel|tls|bss --- */
    uint64_t off_ph   = sizeof(Elf64_Ehdr);
    int      nph      = has_tls ? 2 : 1;   /* PT_LOAD (+ PT_TLS) */
    uint64_t cur      = ALIGN_UP(off_ph + nph * sizeof(Elf64_Phdr), 16);

    /* Place every input section of a bucket contiguously, recording each
     * section's assigned image vaddr in objs[i].sec_va[] for symbol resolution.
     * `bkt` selects which bucket; returns the end cursor. */
    uint64_t text_beg = cur;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++)
            if (objs[i].sec_bucket[s] == B_TEXT && objs[i].sh[s].sh_size) {
                uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 4;
                cur = ALIGN_UP(cur, al < 4 ? 4 : al);
                objs[i].sec_va[s] = cur;
                cur += objs[i].sh[s].sh_size;
            }
    uint64_t text_end = cur;
    uint64_t ro_beg = cur;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++)
            if (objs[i].sec_bucket[s] == B_RODATA && objs[i].sh[s].sh_size) {
                uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                cur = ALIGN_UP(cur, al);
                objs[i].sec_va[s] = cur;
                cur += objs[i].sh[s].sh_size;
            }
    uint64_t ro_end = cur;

    /* GOT cells (writable, 8-aligned). */
    uint64_t got_beg = ALIGN_UP(cur, 8);
    for (int i = 0; i < ngot; i++) got[i].va = got_beg + (uint64_t)i * 8;
    uint64_t got_end = got_beg + (uint64_t)ngot * 8;
    cur = got_end;

    /* TLSDESC entries (writable, 2 quadwords each, 8-aligned). */
    uint64_t tlsdesc_beg = ALIGN_UP(cur, 8);
    for (int i = 0; i < ntls; i++) tls[i].va = tlsdesc_beg + (uint64_t)i * 16;
    uint64_t tlsdesc_end = tlsdesc_beg + (uint64_t)ntls * 16;
    cur = tlsdesc_end;

    /* .data (writable, initialized). */
    uint64_t data_beg = cur;
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++)
            if (objs[i].sec_bucket[s] == B_DATA && objs[i].sh[s].sh_size) {
                uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                cur = ALIGN_UP(cur, al);
                objs[i].sec_va[s] = cur;
                cur += objs[i].sh[s].sh_size;
            }
    uint64_t data_end = cur;

    /* .tdata (TLS init image, file-backed). PT_TLS references it; a reserved
     * vaddr is assigned even for a pure-.tbss image (tdata_sz == 0). */
    uint64_t tdata_va = 0, tdata_end = data_end;
    if (has_tls) {
        cur = ALIGN_UP(cur, tls_align);
        tdata_va = cur;
        cur += tdata_sz;
        tdata_end = cur;
    }

    uint64_t off_sv = ALIGN_UP(cur, 8);

    for (int i = 0; i < nuniv; i++)
        uv[i].value = resolve_named(objs, nobj, uv[i].name,
                                    "universal symbol not defined in any input object");

    uint32_t names_size = 0;
    for (int i = 0; i < nuniv; i++) names_size += (uint32_t)strlen(uv[i].name) + 1;
    uint64_t sv_hdr_sz = sizeof(struct ovmx_sv_header);
    uint64_t sv_names_o = sv_hdr_sz + (uint64_t)nuniv * sizeof(struct ovmx_sv_entry);
    uint64_t sv_size = sv_names_o + names_size;

    /* .vms$rel: one image-relative offset per GOT slot AND per ABS64 data
     * pointer — all hold an image-relative address biased at activation. Sized
     * for the upper bound (ngot + nabs); the header count records how many were
     * actually resolved (deferred imports are excluded). (vms-004) */
    int nrel = ngot + nabs;
    uint64_t off_rel = 0, rel_size = 0;
    if (nrel) {
        off_rel = ALIGN_UP(off_sv + sv_size, 8);
        rel_size = sizeof(struct ovmx_rel_header) + (uint64_t)nrel * 8;
    }

    /* .vms$tls: one image-relative offset per TLSDESC entry. */
    uint64_t after_rel = nrel ? off_rel + rel_size : off_sv + sv_size;
    int ntlsdesc = ntls;
    uint64_t off_tls = 0, tls_sec_size = 0;
    if (ntlsdesc) {
        off_tls = ALIGN_UP(after_rel, 8);
        tls_sec_size = sizeof(struct ovmx_tls_header) + (uint64_t)ntlsdesc * 8;
    }

    /* End of file-backed loaded content; .bss (NOBITS) extends memsz beyond it. */
    uint64_t file_loaded_end = ntlsdesc ? off_tls + tls_sec_size : after_rel;
    uint64_t bss_beg = file_loaded_end, bss_end = file_loaded_end;
    if (has_bss) {
        int first = 1;
        for (int i = 0; i < nobj; i++)
            for (int s = 0; s < objs[i].nsh; s++)
                if (objs[i].sec_bucket[s] == B_BSS && objs[i].sh[s].sh_size) {
                    uint64_t al = objs[i].sh[s].sh_addralign ? objs[i].sh[s].sh_addralign : 8;
                    bss_end = ALIGN_UP(bss_end, al);
                    if (first) { bss_beg = bss_end; first = 0; }
                    objs[i].sec_va[s] = bss_end;
                    bss_end += objs[i].sh[s].sh_size;
                }
    }

    uint64_t off_shstr = ALIGN_UP(file_loaded_end, 4);

    const char *secn[16]; int nsec = 0;
    secn[nsec++] = "";
    int ix_text = nsec; secn[nsec++] = ".text";
    int ix_ro = -1;   if (has_ro)   { ix_ro   = nsec; secn[nsec++] = ".rodata"; }
    int ix_got = -1;  if (ngot)     { ix_got  = nsec; secn[nsec++] = ".got"; }
    int ix_tlsd = -1; if (ntls)     { ix_tlsd = nsec; secn[nsec++] = ".tlsdesc"; }
    int ix_data = -1; if (has_data) { ix_data = nsec; secn[nsec++] = ".data"; }
    int ix_tdata = -1; if (has_tls && tdata_sz) { ix_tdata = nsec; secn[nsec++] = ".tdata"; }
    int ix_sv = nsec; secn[nsec++] = OVMX_SV_SECTION;
    int ix_rel = -1;  if (nrel)     { ix_rel  = nsec; secn[nsec++] = OVMX_REL_SECTION; }
    int ix_tls = -1;  if (ntlsdesc) { ix_tls  = nsec; secn[nsec++] = OVMX_TLS_SECTION; }
    int ix_bss = -1;  if (has_bss)  { ix_bss  = nsec; secn[nsec++] = ".bss"; }
    int ix_tbss = -1; if (has_tls && tbss_sz) { ix_tbss = nsec; secn[nsec++] = ".tbss"; }
    int ix_str = nsec; secn[nsec++] = ".shstrtab";
    uint64_t sn_off[16]; uint64_t sn_sz = 0;
    for (int i = 0; i < nsec; i++) { sn_off[i] = sn_sz; sn_sz += strlen(secn[i]) + 1; }
    uint64_t off_shdr = ALIGN_UP(off_shstr + sn_sz, 8);
    uint64_t file_sz = off_shdr + (uint64_t)nsec * sizeof(Elf64_Shdr);

    uint8_t *img = calloc(1, file_sz);
    if (!img) die("oom building image");

    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS] = ELFCLASS64; eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_DYN; eh->e_machine = EM_AARCH64; eh->e_version = EV_CURRENT;
    eh->e_phoff = off_ph; eh->e_shoff = off_shdr;
    eh->e_ehsize = sizeof *eh; eh->e_phentsize = sizeof(Elf64_Phdr); eh->e_phnum = nph;
    eh->e_shentsize = sizeof(Elf64_Shdr); eh->e_shnum = nsec; eh->e_shstrndx = ix_str;

    /* One PT_LOAD. RWX when it carries a writable GOT/TLSDESC/.data/.bss (the
     * activator writes those in place); R+X for a pure leaf/rodata image.
     * A PT_TLS follows when the image has thread-local storage. */
    int writable = (ngot || ntls || has_data || has_bss || has_tls);
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + off_ph);
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = writable ? (PF_R | PF_W | PF_X) : (PF_R | PF_X);
    ph[0].p_filesz = file_loaded_end; ph[0].p_memsz = bss_end; ph[0].p_align = PAGE;
    if (has_tls) {
        ph[1].p_type = PT_TLS; ph[1].p_flags = PF_R;
        ph[1].p_offset = tdata_va; ph[1].p_vaddr = tdata_va; ph[1].p_paddr = tdata_va;
        ph[1].p_filesz = tdata_sz; ph[1].p_memsz = tls_memsz; ph[1].p_align = tls_align;
    }

    /* Copy each placed PROGBITS section (text/rodata/data) to its vaddr. */
    for (int i = 0; i < nobj; i++)
        for (int s = 0; s < objs[i].nsh; s++) {
            int b = objs[i].sec_bucket[s];
            if ((b == B_TEXT || b == B_RODATA || b == B_DATA) &&
                objs[i].sh[s].sh_size)
                memcpy(img + objs[i].sec_va[s],
                       objs[i].buf + objs[i].sh[s].sh_offset, objs[i].sh[s].sh_size);
        }

    /* Copy the TLS init image (.tdata); .tbss is zero-filled per thread. */
    if (has_tls && tdata_sz)
        memcpy(img + tdata_va, objs[tls_obj].buf + objs[tls_obj].tdata->sh_offset, tdata_sz);

    /* Image-relative slots (GOT cells + ABS64 data pointers) to bias at
     * activation; filled as they resolve, header count set at the end. */
    uint64_t *rel_off = nrel ? calloc((size_t)nrel, 8) : NULL;
    int nrel_filled = 0;

    /* Fill GOT cells with image-relative target addresses. A GOT symbol not
     * defined by any input object is a deferred import (vms-61f): under
     * --allow-undefined its cell stays 0 and is NOT recorded in .vms$rel. */
    for (int i = 0; i < ngot; i++) {
        got[i].value = resolve_named(objs, nobj, got[i].name, NULL);
        if (got[i].value) {
            *(uint64_t *)(img + got[i].va) = got[i].value;
            rel_off[nrel_filled++] = got[i].va;   /* image-relative -> bias at activation */
            continue;
        }
        /* Undefined GOT symbol. A WEAK reference (no definition anywhere) is a
         * legitimate address-0 resolution — the linker-defined empty
         * __init_array/__fini_array bounds and null _DYNAMIC of a C-RTL with no
         * static constructors. Otherwise it is a deferred import
         * (--allow-undefined) or, strict, a hard error. (vms-61f.1) */
        *(uint64_t *)(img + got[i].va) = 0;   /* cell = 0, NOT biased/recorded */
        if (weak_has(got[i].name))      { /* correct 0; nothing to defer */ }
        else if (g_allow_undef)         { g_deferred++; }
        else die("GOT symbol undefined (cross-image DATA import is a later increment)");
    }

    /* Fill TLSDESC entries: [0]=0 (IMGACT sets the resolver), [1]=module offset. */
    for (int i = 0; i < ntls; i++) {
        tls[i].modoff = tls_module_offset(objs, nobj, tbss_base,
                                          tls[i].name, tls[i].addend);
        uint64_t *e = (uint64_t *)(img + tls[i].va);
        e[0] = 0;
        e[1] = tls[i].modoff;
    }

    /* Apply relocations across every code section: GOT-indirect pairs -> GOT
     * slot; TLSDESC -> TLSDESC entry; the rest PC-relative (with addend). */
    for (int i = 0; i < nobj; i++) {
        for (int r = 0; r < objs[i].nreloc; r++) {
            struct reloc *rl = &objs[i].relocs[r];
            uint32_t type = ELF64_R_TYPE(rl->info);
            uint64_t site = objs[i].sec_va[rl->sec] + rl->off;
            uint32_t *insn = (uint32_t *)(img + site);
            const char *nm = objs[i].str +
                             objs[i].sym[ELF64_R_SYM(rl->info)].st_name;
            if (is_got_reloc(type)) {
                int gi = find_got(got, ngot, nm);
                if (gi < 0) die("internal: GOT slot missing for symbol");
                patch_got(type, insn, site, got[gi].va);
            } else if (is_tlsdesc_reloc(type)) {
                int ti = find_tls(tls, ntls, nm);
                if (ti < 0) die("internal: TLSDESC slot missing for symbol");
                patch_tlsdesc(type, insn, site, tls[ti].va);
            } else if (type == R_AARCH64_ABS64) {
                /* Pointer initializer (.rela.data): write S+A as a 64-bit
                 * image-relative address and record the slot in .vms$rel so
                 * the activator adds the load bias. A deferred external leaves
                 * the slot 0 (unbiased). (vms-004, folds in vms-a17) */
                uint64_t s = resolve_ref(objs, nobj, i, ELF64_R_SYM(rl->info));
                if (s == 0) continue;   /* deferred (counted in resolve_ref) */
                uint64_t value = s + (uint64_t)rl->add;
                *(uint64_t *)(img + site) = value;
                rel_off[nrel_filled++] = site;
            } else {
                uint64_t target = resolve_ref(objs, nobj, i, ELF64_R_SYM(rl->info));
                if (target == 0) continue;   /* deferred external, skip patch */
                target += (uint64_t)rl->add;
                patch_pcrel(type, insn, site, target);
            }
        }
    }

    struct ovmx_sv_header *svh = (struct ovmx_sv_header *)(img + off_sv);
    svh->magic = OVMX_SV_MAGIC; svh->count = nuniv;
    svh->gsmatch_kind = gk; svh->gsmatch_major = gmaj; svh->gsmatch_minor = gmin;
    svh->names_off = (uint32_t)sv_names_o; svh->names_size = names_size;
    struct ovmx_sv_entry *sve = (struct ovmx_sv_entry *)(img + off_sv + sv_hdr_sz);
    char *nblob = (char *)(img + off_sv + sv_names_o); uint32_t noff = 0;
    for (int i = 0; i < nuniv; i++) {
        sve[i].value = uv[i].value; sve[i].kind = uv[i].kind; sve[i].name_off = noff;
        size_t l = strlen(uv[i].name) + 1; memcpy(nblob + noff, uv[i].name, l);
        noff += (uint32_t)l;
    }

    if (nrel) {
        struct ovmx_rel_header *rh = (struct ovmx_rel_header *)(img + off_rel);
        rh->magic = OVMX_REL_MAGIC; rh->count = (uint32_t)nrel_filled;
        uint64_t *off = (uint64_t *)(img + off_rel + sizeof *rh);
        for (int i = 0; i < nrel_filled; i++) off[i] = rel_off[i];
    }

    if (ntlsdesc) {
        struct ovmx_tls_header *th = (struct ovmx_tls_header *)(img + off_tls);
        th->magic = OVMX_TLS_MAGIC; th->count = (uint32_t)ntlsdesc;
        uint64_t *eo = (uint64_t *)(img + off_tls + sizeof *th);
        for (int i = 0; i < ntls; i++) eo[i] = tls[i].va;
    }

    char *shstr = (char *)(img + off_shstr);
    for (int i = 0; i < nsec; i++) memcpy(shstr + sn_off[i], secn[i], strlen(secn[i]) + 1);
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + off_shdr);
    sh[ix_text].sh_name = sn_off[ix_text]; sh[ix_text].sh_type = SHT_PROGBITS;
    sh[ix_text].sh_flags = SHF_ALLOC | SHF_EXECINSTR; sh[ix_text].sh_addr = text_beg;
    sh[ix_text].sh_offset = text_beg; sh[ix_text].sh_size = text_end - text_beg;
    sh[ix_text].sh_addralign = 16;
    if (has_ro) {
        sh[ix_ro].sh_name = sn_off[ix_ro]; sh[ix_ro].sh_type = SHT_PROGBITS;
        sh[ix_ro].sh_flags = SHF_ALLOC; sh[ix_ro].sh_addr = ro_beg;
        sh[ix_ro].sh_offset = ro_beg; sh[ix_ro].sh_size = ro_end - ro_beg;
        sh[ix_ro].sh_addralign = 16;
    }
    if (ngot) {
        sh[ix_got].sh_name = sn_off[ix_got]; sh[ix_got].sh_type = SHT_PROGBITS;
        sh[ix_got].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_got].sh_addr = got_beg;
        sh[ix_got].sh_offset = got_beg; sh[ix_got].sh_size = got_end - got_beg;
        sh[ix_got].sh_addralign = 8;
    }
    if (ntls) {
        sh[ix_tlsd].sh_name = sn_off[ix_tlsd]; sh[ix_tlsd].sh_type = SHT_PROGBITS;
        sh[ix_tlsd].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_tlsd].sh_addr = tlsdesc_beg;
        sh[ix_tlsd].sh_offset = tlsdesc_beg; sh[ix_tlsd].sh_size = tlsdesc_end - tlsdesc_beg;
        sh[ix_tlsd].sh_addralign = 8;
    }
    if (has_data) {
        sh[ix_data].sh_name = sn_off[ix_data]; sh[ix_data].sh_type = SHT_PROGBITS;
        sh[ix_data].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_data].sh_addr = data_beg;
        sh[ix_data].sh_offset = data_beg; sh[ix_data].sh_size = data_end - data_beg;
        sh[ix_data].sh_addralign = 8;
    }
    if (has_tls && tdata_sz) {
        sh[ix_tdata].sh_name = sn_off[ix_tdata]; sh[ix_tdata].sh_type = SHT_PROGBITS;
        sh[ix_tdata].sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
        sh[ix_tdata].sh_addr = tdata_va; sh[ix_tdata].sh_offset = tdata_va;
        sh[ix_tdata].sh_size = tdata_sz; sh[ix_tdata].sh_addralign = tdata_al;
    }
    sh[ix_sv].sh_name = sn_off[ix_sv]; sh[ix_sv].sh_type = SHT_PROGBITS;
    sh[ix_sv].sh_flags = SHF_ALLOC; sh[ix_sv].sh_addr = off_sv;
    sh[ix_sv].sh_offset = off_sv; sh[ix_sv].sh_size = sv_size; sh[ix_sv].sh_addralign = 8;
    if (nrel) {
        sh[ix_rel].sh_name = sn_off[ix_rel]; sh[ix_rel].sh_type = SHT_PROGBITS;
        sh[ix_rel].sh_flags = SHF_ALLOC; sh[ix_rel].sh_addr = off_rel;
        sh[ix_rel].sh_offset = off_rel; sh[ix_rel].sh_size = rel_size;
        sh[ix_rel].sh_addralign = 8;
    }
    if (ntlsdesc) {
        sh[ix_tls].sh_name = sn_off[ix_tls]; sh[ix_tls].sh_type = SHT_PROGBITS;
        sh[ix_tls].sh_flags = SHF_ALLOC; sh[ix_tls].sh_addr = off_tls;
        sh[ix_tls].sh_offset = off_tls; sh[ix_tls].sh_size = tls_sec_size;
        sh[ix_tls].sh_addralign = 8;
    }
    if (has_tls && tbss_sz) {
        sh[ix_tbss].sh_name = sn_off[ix_tbss]; sh[ix_tbss].sh_type = SHT_NOBITS;
        sh[ix_tbss].sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
        sh[ix_tbss].sh_addr = tdata_va + tbss_base; sh[ix_tbss].sh_offset = tdata_end;
        sh[ix_tbss].sh_size = tbss_sz; sh[ix_tbss].sh_addralign = tbss_al;
    }
    if (has_bss) {
        sh[ix_bss].sh_name = sn_off[ix_bss]; sh[ix_bss].sh_type = SHT_NOBITS;
        sh[ix_bss].sh_flags = SHF_ALLOC | SHF_WRITE; sh[ix_bss].sh_addr = bss_beg;
        sh[ix_bss].sh_offset = file_loaded_end; sh[ix_bss].sh_size = bss_end - bss_beg;
        sh[ix_bss].sh_addralign = 8;
    }
    sh[ix_str].sh_name = sn_off[ix_str]; sh[ix_str].sh_type = SHT_STRTAB;
    sh[ix_str].sh_offset = off_shstr; sh[ix_str].sh_size = sn_sz; sh[ix_str].sh_addralign = 1;

    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) die("cannot create output image");
    if (write(fd, img, file_sz) != (ssize_t)file_sz) die("short write output");
    close(fd);
    int totrel = 0; for (int i = 0; i < nobj; i++) totrel += objs[i].nreloc;
    /* ABS64 data pointers written = filled .vms$rel slots minus the resolved
     * GOT cells (GOT slots are pushed into rel_off first). */
    int abs_applied = nrel_filled;
    for (int i = 0; i < ngot; i++) if (got[i].value) abs_applied--;
    fprintf(stderr,
        "%%LINK-S-CREATED, %s: ET_DYN shareable image, %d object%s, %d universal%s, "
        "%d reloc%s, %d GOT, %d TLS, %d ABS64-ptr, GSMATCH=%s,%u,%u\n",
        out, nobj, nobj==1?"":"s", nuniv, nuniv==1?"":"s", totrel, totrel==1?"":"s",
        ngot, ntls, abs_applied,
        gk == OVMX_GSMATCH_ALWAYS ? "ALWAYS" :
        gk == OVMX_GSMATCH_EQUAL  ? "EQUAL"  : "LEQUAL", gmaj, gmin);
    if (g_deferred)
        fprintf(stderr, "%%LINK-I-DEFEXT, %ld external reference%s left unresolved "
                "(deferred imports — satisfied by the C RTL / a companion "
                "shareable at activation, vms-61f)\n",
                g_deferred, g_deferred == 1 ? "" : "s");
    free(rel_off); free(got); free(tls); free(g_syms); g_syms = NULL;
    free(img);
}


/* Peek an input file's first bytes to tell an `ar` archive from a bare .o. */
static int file_is_archive(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input file");
    char m[AR_MAGIC_LEN];
    ssize_t r = read(fd, m, AR_MAGIC_LEN);
    close(fd);
    return r == AR_MAGIC_LEN && memcmp(m, AR_MAGIC, AR_MAGIC_LEN) == 0;
}

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char **ins = calloc((size_t)argc, sizeof *ins);  /* <= argc inputs */
    int nin = 0;
    struct univ uv[MAX_UNIV];
    int nuniv = 0;
    int shareable = 0, executable = 0, allow_undef = 0;
    struct producer *producers = calloc((size_t)argc, sizeof *producers);
    int np = 0;
    uint32_t gk = OVMX_GSMATCH_EQUAL, gmaj = 0, gmin = 0;
    if (!ins || !producers) die("oom parsing arguments");
    memset(uv, 0, sizeof uv);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--shareable") == 0) {
            shareable = 1;
        } else if (strcmp(argv[i], "--executable") == 0) {
            executable = 1;
        } else if (strcmp(argv[i], "--allow-undefined") == 0) {
            allow_undef = 1;
        } else if (strcmp(argv[i], "--use") == 0 && i + 1 < argc) {
            load_producer(argv[++i], &producers[np++]);
        } else if (strcmp(argv[i], "--symbol-vector") == 0 && i + 1 < argc) {
            nuniv = parse_symbol_vector(argv[++i], uv);
        } else if (strcmp(argv[i], "--gsmatch") == 0 && i + 1 < argc) {
            parse_gsmatch(argv[++i], &gk, &gmaj, &gmin);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%%LINK-W-IGNORED, unknown option %s\n", argv[i]);
        } else {
            ins[nin++] = argv[i];
        }
    }
    if (nin == 0) die("no input (usage: LINK.EXE --shareable "
                  "--symbol-vector \"f=PROCEDURE\" --gsmatch LEQUAL,1,0 [--allow-undefined] "
                  "-o X.EXE a.o [b.o | lib.a ...] "
                  "| LINK.EXE --executable --use LIB$SHR.EXE -o PROG.EXE prog.o)");
    if (!out) die("no -o output");
    if (shareable == executable)
        die("specify exactly one of --shareable / --executable");

    if (executable) {
        struct obj o;
        load_obj(ins[0], &o);       /* single-object executable for now */
        if (np == 0) die("--executable needs at least one --use producer image");
        emit_executable(&o, producers, np, out);
        return 0;
    }

    /* ---- Shareable image: one or more objects and/or whole `.a` archives ----
     * Inputs grow dynamically; an archive expands to all of its object members
     * in-process (whole-archive, no `ld -r`). (vms-004) */
    if (nuniv == 0) die("a shareable image needs --symbol-vector");
    struct obj *objs = NULL; int nobj = 0, cap = 0;
    for (int i = 0; i < nin; i++) {
        if (file_is_archive(ins[i]))
            load_archive(ins[i], &objs, &nobj, &cap);
        else
            load_obj(ins[i], push_obj(&objs, &nobj, &cap));
    }
    if (nobj == 0) die("no object members found in inputs");
    emit_shareable(objs, nobj, uv, nuniv, gk, gmaj, gmin, allow_undef, out);
    return 0;
}
