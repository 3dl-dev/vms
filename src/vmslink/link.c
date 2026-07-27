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
 * MVP SCOPE (vms-9dd): the producer side. Input is one relocatable ELF object
 * whose exported functions/data are self-contained (position-independent, no
 * relocations in the linked section — a leaf). LINK.EXE lays out a minimal
 * mappable ET_DYN image (one PT_LOAD) and emits `.vms$sv` with one entry per
 * declared universal symbol, plus GSMATCH. Full multi-object linking, external
 * references, relocations and TLS are later beads (vms-142/b65).
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
    int           text_ndx; /* .text section index */
    Elf64_Shdr   *text;     /* .text section header */
    int           rodata_ndx; /* .rodata section index (0 if none) */
    Elf64_Shdr   *rodata;   /* .rodata section header (0 if none) */
    Elf64_Rela   *rela;     /* relocations against .text (SHT_RELA) */
    int           nrela;
};

static void *xat(struct obj *o, uint64_t off, uint64_t sz, const char *what)
{
    if (off + sz > o->size)
        die(what);
    return o->buf + off;
}

static void load_obj(const char *path, struct obj *o)
{
    memset(o, 0, sizeof *o);
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open input object");
    struct stat st;
    if (fstat(fd, &st) < 0) die("fstat input");
    o->size = (size_t)st.st_size;
    o->buf = malloc(o->size);
    if (!o->buf) die("oom reading object");
    if (read(fd, o->buf, o->size) != (ssize_t)o->size) die("short read object");
    close(fd);

    if (o->size < sizeof(Elf64_Ehdr) || memcmp(o->buf, ELFMAG, SELFMAG) != 0)
        die("input is not ELF");
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

    /* Find .text, .symtab, .strtab. */
    for (int i = 0; i < o->nsh; i++) {
        const char *nm = o->shstr + o->sh[i].sh_name;
        if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".text") == 0) {
            o->text_ndx = i;
            o->text = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_PROGBITS && strcmp(nm, ".rodata") == 0) {
            o->rodata_ndx = i;
            o->rodata = &o->sh[i];
        } else if (o->sh[i].sh_type == SHT_SYMTAB) {
            o->sym  = (Elf64_Sym *)(o->buf + o->sh[i].sh_offset);
            o->nsym = o->sh[i].sh_size / sizeof(Elf64_Sym);
            o->str  = (const char *)(o->buf + o->sh[o->sh[i].sh_link].sh_offset);
        }
    }
    if (!o->text) die("input object has no .text section");
    if (!o->sym)  die("input object has no symbol table");

    /* Collect relocations against .text (SHT_RELA). REL (implicit-addend) is
     * not emitted by aarch64 gcc; reject it if seen. */
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
};

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

    /* Collect imports from CALL26/JUMP26 relocations to undefined symbols. */
    struct import imp[256];
    int nimp = 0;
    for (int i = 0; i < o->nrela; i++) {
        uint32_t type = ELF64_R_TYPE(o->rela[i].r_info);
        uint32_t si   = ELF64_R_SYM(o->rela[i].r_info);
        const char *nm = o->str + o->sym[si].st_name;
        if (type != R_AARCH64_CALL26 && type != R_AARCH64_JUMP26)
            die("MVP consumer supports only CALL26/JUMP26 relocs in .text");
        if (o->sym[si].st_shndx != SHN_UNDEF)
            die("MVP consumer supports only external (imported) calls");
        int found = -1;
        for (int k = 0; k < nimp; k++)
            if (strcmp(imp[k].name, nm) == 0) found = k;
        if (found < 0) {
            if (nimp >= 256) die("too many imports");
            found = nimp++;
            snprintf(imp[found].name, sizeof imp[found].name, "%s", nm);
            if (!find_universal(ps, np, nm, &imp[found].pidx, &imp[found].svidx))
                die("unresolved universal symbol (not in any --use image)");
        }
    }
    if (nimp == 0) die("consumer imports nothing (no external calls to bind)");

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

    /* Patch CALL26/JUMP26 sites to branch to their PLT stubs. */
    for (int i = 0; i < o->nrela; i++) {
        uint32_t type = ELF64_R_TYPE(o->rela[i].r_info);
        const char *nm = o->str + o->sym[ELF64_R_SYM(o->rela[i].r_info)].st_name;
        int k = -1;
        for (int j = 0; j < nimp; j++) if (strcmp(imp[j].name, nm) == 0) k = j;
        uint64_t site = text_va + o->rela[i].r_offset;
        int64_t  disp = (int64_t)imp[k].plt_va - (int64_t)site;
        uint32_t imm26 = (uint32_t)((disp >> 2) & 0x03FFFFFF);
        uint32_t op = (type == R_AARCH64_JUMP26) ? 0x14000000u : 0x94000000u;
        uint32_t *insn = (uint32_t *)(img + off_text + o->rela[i].r_offset);
        *insn = op | imm26;
    }

    /* PLT stubs: adrp x16,GOT ; ldr x16,[x16,#lo12] ; br x16. */
    for (int i = 0; i < nimp; i++) {
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

/* Per-object placement of the merged sections. */
struct placed { uint64_t text_va, ro_va; };

/* Resolve a relocation's symbol to a final image vaddr (local or cross-object). */
static uint64_t resolve_ref(struct obj *objs, int nobj, struct placed *pl,
                            int oi, uint32_t symidx)
{
    struct obj *o = &objs[oi];
    Elf64_Sym *s = &o->sym[symidx];
    if (s->st_shndx == (Elf64_Section)o->text_ndx)
        return pl[oi].text_va + s->st_value;
    if (o->rodata && s->st_shndx == (Elf64_Section)o->rodata_ndx)
        return pl[oi].ro_va + s->st_value;
    if (s->st_shndx == SHN_UNDEF) {
        const char *nm = o->str + s->st_name;
        if (!nm[0]) die("undefined unnamed symbol in relocation");
        for (int j = 0; j < nobj; j++) {
            struct obj *d = &objs[j];
            for (int k = 0; k < d->nsym; k++) {
                Elf64_Sym *ds = &d->sym[k];
                if (ds->st_shndx == SHN_UNDEF ||
                    ELF64_ST_BIND(ds->st_info) == STB_LOCAL) continue;
                if (strcmp(d->str + ds->st_name, nm) != 0) continue;
                if (ds->st_shndx == (Elf64_Section)d->text_ndx)
                    return pl[j].text_va + ds->st_value;
                if (d->rodata && ds->st_shndx == (Elf64_Section)d->rodata_ndx)
                    return pl[j].ro_va + ds->st_value;
            }
        }
        die("unresolved external symbol (needs the C RTL -- vms-61f)");
    }
    die("relocation against an unsupported section");
    return 0;
}

/* Find a declared universal by name across all objects -> image vaddr. */
static uint64_t resolve_univ_multi(struct obj *objs, int nobj, struct placed *pl,
                                   const char *name)
{
    for (int j = 0; j < nobj; j++) {
        struct obj *d = &objs[j];
        for (int k = 0; k < d->nsym; k++) {
            Elf64_Sym *ds = &d->sym[k];
            if (ELF64_ST_BIND(ds->st_info) == STB_LOCAL) continue;
            if (strcmp(d->str + ds->st_name, name) != 0) continue;
            if (ds->st_shndx == (Elf64_Section)d->text_ndx)
                return pl[j].text_va + ds->st_value;
            if (d->rodata && ds->st_shndx == (Elf64_Section)d->rodata_ndx)
                return pl[j].ro_va + ds->st_value;
        }
    }
    die("universal symbol not defined in any input object");
    return 0;
}

/* Emit an OVMX shareable image from N objects: merge .text + .rodata, apply
 * PC-relative relocations (local + cross-object), export universals. (vms-20b) */
static void emit_shareable(struct obj *objs, int nobj, struct univ *uv, int nuniv,
                           uint32_t gk, uint32_t gmaj, uint32_t gmin, const char *out)
{
    struct placed pl[64];
    int has_ro = 0;
    for (int i = 0; i < nobj; i++)
        if (objs[i].rodata && objs[i].rodata->sh_size) has_ro = 1;

    uint64_t off_ph   = sizeof(Elf64_Ehdr);
    int      nph      = 1;
    uint64_t cur      = ALIGN_UP(off_ph + nph * sizeof(Elf64_Phdr), 16);
    uint64_t text_beg = cur;
    for (int i = 0; i < nobj; i++) {
        cur = ALIGN_UP(cur, 16);
        pl[i].text_va = cur;
        cur += objs[i].text->sh_size;
    }
    uint64_t text_end = cur;
    uint64_t ro_beg = cur;
    for (int i = 0; i < nobj; i++) {
        if (objs[i].rodata && objs[i].rodata->sh_size) {
            cur = ALIGN_UP(cur, 16);
            pl[i].ro_va = cur;
            cur += objs[i].rodata->sh_size;
        } else pl[i].ro_va = 0;
    }
    uint64_t ro_end = cur;
    uint64_t off_sv = ALIGN_UP(cur, 8);

    for (int i = 0; i < nuniv; i++)
        uv[i].value = resolve_univ_multi(objs, nobj, pl, uv[i].name);

    uint32_t names_size = 0;
    for (int i = 0; i < nuniv; i++) names_size += (uint32_t)strlen(uv[i].name) + 1;
    uint64_t sv_hdr_sz = sizeof(struct ovmx_sv_header);
    uint64_t sv_names_o = sv_hdr_sz + (uint64_t)nuniv * sizeof(struct ovmx_sv_entry);
    uint64_t sv_size = sv_names_o + names_size;
    uint64_t loaded_end = off_sv + sv_size;
    uint64_t off_shstr = ALIGN_UP(loaded_end, 4);

    const char *secn[6]; int nsec = 0;
    secn[nsec++] = "";
    int ix_text = nsec; secn[nsec++] = ".text";
    int ix_ro = -1; if (has_ro) { ix_ro = nsec; secn[nsec++] = ".rodata"; }
    int ix_sv = nsec; secn[nsec++] = OVMX_SV_SECTION;
    int ix_str = nsec; secn[nsec++] = ".shstrtab";
    uint64_t sn_off[6]; uint64_t sn_sz = 0;
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

    Elf64_Phdr *ph = (Elf64_Phdr *)(img + off_ph);
    ph->p_type = PT_LOAD; ph->p_flags = PF_R | PF_X;
    ph->p_filesz = loaded_end; ph->p_memsz = loaded_end; ph->p_align = PAGE;

    for (int i = 0; i < nobj; i++) {
        memcpy(img + pl[i].text_va, objs[i].buf + objs[i].text->sh_offset,
               objs[i].text->sh_size);
        if (objs[i].rodata && objs[i].rodata->sh_size)
            memcpy(img + pl[i].ro_va, objs[i].buf + objs[i].rodata->sh_offset,
                   objs[i].rodata->sh_size);
    }
    for (int i = 0; i < nobj; i++) {
        for (int r = 0; r < objs[i].nrela; r++) {
            Elf64_Rela *rl = &objs[i].rela[r];
            uint64_t target = resolve_ref(objs, nobj, pl, i, ELF64_R_SYM(rl->r_info));
            uint64_t site = pl[i].text_va + rl->r_offset;
            patch_pcrel(ELF64_R_TYPE(rl->r_info),
                        (uint32_t *)(img + pl[i].text_va + rl->r_offset), site, target);
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
    sh[ix_sv].sh_name = sn_off[ix_sv]; sh[ix_sv].sh_type = SHT_PROGBITS;
    sh[ix_sv].sh_flags = SHF_ALLOC; sh[ix_sv].sh_addr = off_sv;
    sh[ix_sv].sh_offset = off_sv; sh[ix_sv].sh_size = sv_size; sh[ix_sv].sh_addralign = 8;
    sh[ix_str].sh_name = sn_off[ix_str]; sh[ix_str].sh_type = SHT_STRTAB;
    sh[ix_str].sh_offset = off_shstr; sh[ix_str].sh_size = sn_sz; sh[ix_str].sh_addralign = 1;

    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) die("cannot create output image");
    if (write(fd, img, file_sz) != (ssize_t)file_sz) die("short write output");
    close(fd);
    int totrel = 0; for (int i = 0; i < nobj; i++) totrel += objs[i].nrela;
    fprintf(stderr,
        "%%LINK-S-CREATED, %s: ET_DYN shareable image, %d object%s, %d universal%s, "
        "%d reloc%s, GSMATCH=%s,%u,%u\n",
        out, nobj, nobj==1?"":"s", nuniv, nuniv==1?"":"s", totrel, totrel==1?"":"s",
        gk == OVMX_GSMATCH_ALWAYS ? "ALWAYS" :
        gk == OVMX_GSMATCH_EQUAL  ? "EQUAL"  : "LEQUAL", gmaj, gmin);
    free(img);
}


int main(int argc, char **argv)
{
    const char *out = NULL;
    const char *ins[64];
    int nin = 0;
    struct univ uv[MAX_UNIV];
    int nuniv = 0;
    int shareable = 0, executable = 0;
    struct producer producers[64];
    int np = 0;
    uint32_t gk = OVMX_GSMATCH_EQUAL, gmaj = 0, gmin = 0;
    memset(uv, 0, sizeof uv);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--shareable") == 0) {
            shareable = 1;
        } else if (strcmp(argv[i], "--executable") == 0) {
            executable = 1;
        } else if (strcmp(argv[i], "--use") == 0 && i + 1 < argc) {
            if (np >= 64) die("too many --use producer images");
            load_producer(argv[++i], &producers[np++]);
        } else if (strcmp(argv[i], "--symbol-vector") == 0 && i + 1 < argc) {
            nuniv = parse_symbol_vector(argv[++i], uv);
        } else if (strcmp(argv[i], "--gsmatch") == 0 && i + 1 < argc) {
            parse_gsmatch(argv[++i], &gk, &gmaj, &gmin);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%%LINK-W-IGNORED, unknown option %s\n", argv[i]);
        } else {
            if (nin >= 64) die("too many input objects");
            ins[nin++] = argv[i];
        }
    }
    if (nin == 0) die("no input object (usage: LINK.EXE --shareable "
                  "--symbol-vector \"f=PROCEDURE\" --gsmatch LEQUAL,1,0 -o X.EXE a.o [b.o ...] "
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

    /* ---- Shareable image (one or more objects) ---- */
    if (nuniv == 0) die("a shareable image needs --symbol-vector");
    static struct obj objs[64];
    for (int i = 0; i < nin; i++)
        load_obj(ins[i], &objs[i]);
    emit_shareable(objs, nin, uv, nuniv, gk, gmaj, gmin, out);
    return 0;
}
