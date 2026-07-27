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
        } else if (o->sh[i].sh_type == SHT_SYMTAB) {
            o->sym  = (Elf64_Sym *)(o->buf + o->sh[i].sh_offset);
            o->nsym = o->sh[i].sh_size / sizeof(Elf64_Sym);
            o->str  = (const char *)(o->buf + o->sh[o->sh[i].sh_link].sh_offset);
        }
    }
    if (!o->text) die("input object has no .text section");
    if (!o->sym)  die("input object has no symbol table");

    /* MVP: refuse relocations against .text — we copy bytes verbatim. */
    for (int i = 0; i < o->nsh; i++) {
        if ((o->sh[i].sh_type == SHT_RELA || o->sh[i].sh_type == SHT_REL) &&
            o->sh[i].sh_info == (Elf64_Word)o->text_ndx &&
            o->sh[i].sh_size != 0)
            die("MVP cannot link a .text with relocations (needs a leaf object)");
    }
}

/* Resolve a declared universal symbol to its offset within .text. */
static void resolve_univ(struct obj *o, struct univ *u)
{
    for (int i = 0; i < o->nsym; i++) {
        const char *nm = o->str + o->sym[i].st_name;
        if (strcmp(nm, u->name) != 0)
            continue;
        if (o->sym[i].st_shndx != (Elf64_Section)o->text_ndx)
            die("universal symbol is not defined in .text (MVP limit)");
        u->value = o->sym[i].st_value;   /* offset within .text; +text vaddr later */
        u->resolved = 1;
        return;
    }
    die("universal symbol not found in input object");
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

int main(int argc, char **argv)
{
    const char *out = NULL;
    const char *in  = NULL;
    struct univ uv[MAX_UNIV];
    int nuniv = 0;
    int shareable = 0;
    uint32_t gk = OVMX_GSMATCH_EQUAL, gmaj = 0, gmin = 0;
    memset(uv, 0, sizeof uv);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--shareable") == 0) {
            shareable = 1;
        } else if (strcmp(argv[i], "--symbol-vector") == 0 && i + 1 < argc) {
            nuniv = parse_symbol_vector(argv[++i], uv);
        } else if (strcmp(argv[i], "--gsmatch") == 0 && i + 1 < argc) {
            parse_gsmatch(argv[++i], &gk, &gmaj, &gmin);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%%LINK-W-IGNORED, unknown option %s\n", argv[i]);
        } else {
            in = argv[i];
        }
    }
    if (!in)  die("no input object (usage: LINK.EXE --shareable "
                  "--symbol-vector \"f=PROCEDURE\" --gsmatch LEQUAL,1,0 -o X.EXE in.o)");
    if (!out) die("no -o output");
    if (!shareable) die("MVP only builds shareable images (--shareable)");
    if (nuniv == 0) die("a shareable image needs --symbol-vector");

    struct obj o;
    load_obj(in, &o);
    for (int i = 0; i < nuniv; i++)
        resolve_univ(&o, &uv[i]);

    /* ---- Layout (file offsets == the mapped image up to end of .vms$sv) ----
     * [ehdr][phdr:PT_LOAD][.text][.vms$sv][.shstrtab][shdrs]
     * The PT_LOAD covers ehdr..end(.vms$sv); shstrtab+shdrs trail, unmapped
     * (IMGACT reads section headers from the file to find .vms$sv). */
    uint64_t off_eh    = 0;
    uint64_t off_ph    = sizeof(Elf64_Ehdr);
    int      nph       = 1;
    uint64_t off_text  = ALIGN_UP(off_ph + nph * sizeof(Elf64_Phdr), 16);
    uint64_t text_sz   = o.text->sh_size;

    uint64_t off_sv    = ALIGN_UP(off_text + text_sz, 8);
    /* Build the .vms$sv payload: header + entries + name blob. */
    uint32_t names_size = 0;
    for (int i = 0; i < nuniv; i++)
        names_size += (uint32_t)strlen(uv[i].name) + 1;
    uint64_t sv_hdr_sz  = sizeof(struct ovmx_sv_header);
    uint64_t sv_ent_sz  = (uint64_t)nuniv * sizeof(struct ovmx_sv_entry);
    uint64_t sv_names_o = sv_hdr_sz + sv_ent_sz;
    uint64_t sv_size    = sv_names_o + names_size;

    uint64_t loaded_end = off_sv + sv_size;              /* end of PT_LOAD */
    uint64_t off_shstr  = ALIGN_UP(loaded_end, 4);

    /* .shstrtab contents */
    const char *secnames[] = { "", ".text", OVMX_SV_SECTION, ".shstrtab" };
    int nsec = 4;                                        /* incl. null section */
    uint64_t shstr_off[4];
    uint64_t shstr_sz = 0;
    for (int i = 0; i < nsec; i++) {
        shstr_off[i] = shstr_sz;
        shstr_sz += strlen(secnames[i]) + 1;
    }
    uint64_t off_shdr = ALIGN_UP(off_shstr + shstr_sz, 8);
    uint64_t file_sz  = off_shdr + (uint64_t)nsec * sizeof(Elf64_Shdr);

    uint8_t *img = calloc(1, file_sz);
    if (!img) die("oom building image");

    /* text vaddr == its file offset (identity map within the one PT_LOAD). */
    uint64_t text_vaddr = off_text;
    uint64_t sv_vaddr   = off_sv;

    /* Fill universal-symbol run-time-relative values. */
    for (int i = 0; i < nuniv; i++)
        uv[i].value += text_vaddr;     /* image-relative addr of the symbol */

    /* ELF header */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)(img + off_eh);
    memcpy(eh->e_ident, ELFMAG, SELFMAG);
    eh->e_ident[EI_CLASS]   = ELFCLASS64;
    eh->e_ident[EI_DATA]    = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type      = ET_DYN;
    eh->e_machine   = EM_AARCH64;
    eh->e_version   = EV_CURRENT;
    eh->e_entry     = 0;                 /* shareable image: no start entry */
    eh->e_phoff     = off_ph;
    eh->e_shoff     = off_shdr;
    eh->e_ehsize    = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum     = nph;
    eh->e_shentsize = sizeof(Elf64_Shdr);
    eh->e_shnum     = nsec;
    eh->e_shstrndx  = 3;                 /* .shstrtab is section index 3 */

    /* Program header: one PT_LOAD, R+X, covering ehdr..end(.vms$sv). */
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + off_ph);
    ph->p_type   = PT_LOAD;
    ph->p_flags  = PF_R | PF_X;
    ph->p_offset = 0;
    ph->p_vaddr  = 0;
    ph->p_paddr  = 0;
    ph->p_filesz = loaded_end;
    ph->p_memsz  = loaded_end;
    ph->p_align  = PAGE;

    /* .text bytes, copied verbatim. */
    memcpy(img + off_text, o.buf + o.text->sh_offset, text_sz);

    /* .vms$sv */
    struct ovmx_sv_header *svh = (struct ovmx_sv_header *)(img + off_sv);
    svh->magic         = OVMX_SV_MAGIC;
    svh->count         = nuniv;
    svh->gsmatch_kind  = gk;
    svh->gsmatch_major = gmaj;
    svh->gsmatch_minor = gmin;
    svh->names_off     = (uint32_t)sv_names_o;
    svh->names_size    = names_size;
    struct ovmx_sv_entry *sve =
        (struct ovmx_sv_entry *)(img + off_sv + sv_hdr_sz);
    char *nblob = (char *)(img + off_sv + sv_names_o);
    uint32_t noff = 0;
    for (int i = 0; i < nuniv; i++) {
        sve[i].value    = uv[i].value;   /* image-relative; IMGACT adds bias */
        sve[i].kind     = uv[i].kind;
        sve[i].name_off = noff;
        size_t l = strlen(uv[i].name) + 1;
        memcpy(nblob + noff, uv[i].name, l);
        noff += (uint32_t)l;
    }

    /* .shstrtab */
    char *shstr = (char *)(img + off_shstr);
    for (int i = 0; i < nsec; i++)
        memcpy(shstr + shstr_off[i], secnames[i], strlen(secnames[i]) + 1);

    /* Section headers: [0]=null [1]=.text [2]=.vms$sv [3]=.shstrtab */
    Elf64_Shdr *sh = (Elf64_Shdr *)(img + off_shdr);
    sh[1].sh_name   = shstr_off[1];
    sh[1].sh_type   = SHT_PROGBITS;
    sh[1].sh_flags  = SHF_ALLOC | SHF_EXECINSTR;
    sh[1].sh_addr   = text_vaddr;
    sh[1].sh_offset = off_text;
    sh[1].sh_size   = text_sz;
    sh[1].sh_addralign = 16;

    sh[2].sh_name   = shstr_off[2];
    sh[2].sh_type   = SHT_PROGBITS;
    sh[2].sh_flags  = SHF_ALLOC;
    sh[2].sh_addr   = sv_vaddr;
    sh[2].sh_offset = off_sv;
    sh[2].sh_size   = sv_size;
    sh[2].sh_addralign = 8;

    sh[3].sh_name   = shstr_off[3];
    sh[3].sh_type   = SHT_STRTAB;
    sh[3].sh_offset = off_shstr;
    sh[3].sh_size   = shstr_sz;
    sh[3].sh_addralign = 1;

    /* Write it out. */
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) die("cannot create output image");
    if (write(fd, img, file_sz) != (ssize_t)file_sz) die("short write output");
    close(fd);

    fprintf(stderr,
        "%%LINK-S-CREATED, %s: ET_DYN shareable image, %d universal symbol%s, "
        "GSMATCH=%s,%u,%u\n",
        out, nuniv, nuniv == 1 ? "" : "s",
        gk == OVMX_GSMATCH_ALWAYS ? "ALWAYS" :
        gk == OVMX_GSMATCH_EQUAL  ? "EQUAL"  : "LEQUAL", gmaj, gmin);
    free(img);
    free(o.buf);
    return 0;
}
