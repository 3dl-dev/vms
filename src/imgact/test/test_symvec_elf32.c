/*
 * test_symvec_elf32.c — host unit proof that IMGACT's elf32 symbol-vector
 * resolve path binds a real elf32-vax `.vms$sv`.
 *
 * rd vms-73b2, epic vms-404. Design done-condition (docs/design/
 * vax-symbol-vector-imgact.md §4 P2): "a host/ctest-level elf32 unit test
 * parses an elf32-vax ET_DYN's .vms$sv and resolves an entry by index."
 *
 * This is NOT a mock of the resolver. It exercises the ACTUAL backend code:
 *   - imgact_elf.h (the real class-width abstraction the activator compiles
 *     with) is included with IMGACT_FORCE_ELFCLASS32, so ElfW()/ELFW_* resolve
 *     to the Elf32 types exactly as they do under __vax__ in imgact.c; the
 *     section-header walk below is written against that same abstraction, the
 *     way imgact.c's ovmx_find_section() is;
 *   - ovmx_symvec.h's ovmx_sv_resolve()/ovmx_sv_at()/ovmx_gsmatch_ok() are the
 *     byte-for-byte resolver IMGACT.EXE runs at activation time.
 *
 * The fixture is a hand-built (the design permits this — P3/LINK.EXE is a
 * separate item) but STRUCTURALLY REAL elf32-vax ET_DYN: EI_CLASS=ELFCLASS32,
 * e_machine=EM_VAX, ET_DYN, a section-header string table, and a `.vms$sv`
 * section carrying a proper ovmx_sv_header + entries + names. The test walks
 * its Elf32 section headers to LOCATE `.vms$sv` (proving the elf32 container
 * parse) then RESOLVES entries by vector index through the real resolver.
 *
 * WHAT BREAKS THE TEST IF THE elf32 BACKEND IS WRONG:
 *   - if ElfW() did not size to Elf32 (a mis-generalization that would misparse
 *     an elf32-vax section table with Elf64-sized headers), the section walk
 *     reads the wrong offsets and fails to find `.vms$sv` -> FAIL;
 *   - if the resolver's index/GSMATCH/load_bias arithmetic is wrong, the
 *     resolved values mismatch -> FAIL.
 * A compile-time assertion also pins sizeof(ElfW(Shdr)) to the Elf32 value, so
 * a silent fallback to the Elf64 path cannot pass unnoticed.
 */
#define IMGACT_FORCE_ELFCLASS32 1
#include "imgact_elf.h"          /* real backend width abstraction (vms-73b2) */
#include "ovmx_image.h"          /* real .vms$sv on-disk format               */
#include "ovmx_symvec.h"         /* real resolver IMGACT.EXE runs             */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* The forced Elf32 abstraction must actually be Elf32 (SysV Elf32_Shdr == 40
 * bytes; Elf64_Shdr == 64). A regression that dropped through to the Elf64 path
 * would silently misparse an elf32-vax image; catch it at compile time. */
_Static_assert(sizeof(ElfW(Shdr)) == 40, "elf32 abstraction not active");
_Static_assert(sizeof(ElfW(Ehdr)) == 52, "elf32 abstraction not active");

/* Vector layout used by the fixture (index -> symbol). */
enum { SV_MYADD = 0, SV_MYMUL = 1, SV_GV = 2, SV_COUNT = 3 };

#define LOAD_BIAS   0x00400000u   /* pretend run-time load bias                */
#define VAL_MYADD   0x00000100u   /* image-relative code address (PROCEDURE)   */
#define VAL_MYMUL   0x00000200u   /* image-relative data address (DATA)        */
#define VAL_GV      0x0000002au   /* absolute globalvalue (bound WITHOUT bias) */

/* Build a real elf32-vax ET_DYN with a `.vms$sv` section into `buf`; returns
 * total size and the file offset of the `.vms$sv` payload. */
static size_t build_fixture(uint8_t *buf, size_t cap, uint32_t *sv_file_off)
{
    memset(buf, 0, cap);

    /* --- .vms$sv payload --------------------------------------------------- */
    static const char names[] = "myadd\0mymul\0GV";  /* offsets 0, 6, 12 */
    const uint32_t entries_off = (uint32_t)sizeof(struct ovmx_sv_header);
    const uint32_t names_off   = entries_off +
                                 SV_COUNT * (uint32_t)sizeof(struct ovmx_sv_entry);
    const uint32_t sv_size     = names_off + (uint32_t)sizeof(names);

    /* Place the ELF header, then the shstrtab, then the .vms$sv payload, then
     * the section-header table. Offsets are byte-exact so the Elf32 walk under
     * test has to compute them from the real header fields. */
    const uint32_t eh_size = (uint32_t)sizeof(ElfW(Ehdr));
    static const char shstr[] = "\0.shstrtab\0.vms$sv"; /* names at 1 and 11 */
    const uint32_t shstr_off = eh_size;
    const uint32_t sv_off    = shstr_off + (uint32_t)sizeof(shstr);
    const uint32_t shoff     = sv_off + sv_size;
    const uint32_t shnum     = 3;   /* NULL, .vms$sv, .shstrtab */

    assert(shoff + shnum * sizeof(ElfW(Shdr)) <= cap);
    *sv_file_off = sv_off;

    /* ELF header. */
    ElfW(Ehdr) *eh = (ElfW(Ehdr) *)buf;
    eh->e_ident[EI_MAG0] = ELFMAG0;
    eh->e_ident[EI_MAG1] = ELFMAG1;
    eh->e_ident[EI_MAG2] = ELFMAG2;
    eh->e_ident[EI_MAG3] = ELFMAG3;
    eh->e_ident[EI_CLASS] = ELFCLASS32;
    eh->e_ident[EI_DATA]  = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type    = ET_DYN;
    eh->e_machine = EM_VAX;
    eh->e_version = EV_CURRENT;
    eh->e_ehsize  = (uint16_t)eh_size;
    eh->e_shentsize = (uint16_t)sizeof(ElfW(Shdr));
    eh->e_shnum   = (uint16_t)shnum;
    eh->e_shoff   = shoff;
    eh->e_shstrndx = 2;   /* section[2] is .shstrtab */

    /* shstrtab + .vms$sv payload. */
    memcpy(buf + shstr_off, shstr, sizeof(shstr));

    struct ovmx_sv_header *h = (struct ovmx_sv_header *)(buf + sv_off);
    h->magic = OVMX_SV_MAGIC;
    h->count = SV_COUNT;
    h->gsmatch_kind  = OVMX_GSMATCH_LEQUAL;
    h->gsmatch_major = 1;
    h->gsmatch_minor = 100;
    h->names_off  = names_off;
    h->names_size = (uint32_t)sizeof(names);

    struct ovmx_sv_entry *e =
        (struct ovmx_sv_entry *)(buf + sv_off + entries_off);
    e[SV_MYADD] = (struct ovmx_sv_entry){ .value = VAL_MYADD,
                                          .kind = OVMX_SV_PROCEDURE, .name_off = 0 };
    e[SV_MYMUL] = (struct ovmx_sv_entry){ .value = VAL_MYMUL,
                                          .kind = OVMX_SV_DATA, .name_off = 6 };
    e[SV_GV]    = (struct ovmx_sv_entry){ .value = VAL_GV,
                                          .kind = OVMX_SV_GLOBALVALUE, .name_off = 12 };
    memcpy(buf + sv_off + names_off, names, sizeof(names));

    /* Section headers: [0]=NULL, [1]=.vms$sv, [2]=.shstrtab. */
    ElfW(Shdr) *sh = (ElfW(Shdr) *)(buf + shoff);
    sh[1].sh_name   = 11;               /* ".vms$sv" in shstr */
    sh[1].sh_type   = SHT_PROGBITS;
    sh[1].sh_addr   = sv_off;           /* image-relative vaddr */
    sh[1].sh_offset = sv_off;
    sh[1].sh_size   = sv_size;
    sh[2].sh_name   = 1;                /* ".shstrtab" */
    sh[2].sh_type   = SHT_STRTAB;
    sh[2].sh_offset = shstr_off;
    sh[2].sh_size   = (uint32_t)sizeof(shstr);

    return shoff + shnum * sizeof(ElfW(Shdr));
}

/* The elf32 container walk under test: locate a section by name via the Elf32
 * section-header table, mirroring imgact.c ovmx_find_section() over the SAME
 * ElfW() abstraction. Returns the section's file offset, or 0 if not found. */
static uint32_t find_section_off(const uint8_t *buf, const char *want)
{
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)buf;
    if (eh->e_ident[EI_CLASS] != ELFCLASS32) return 0;
    if (eh->e_machine != EM_VAX)             return 0;   /* really elf32-vax  */
    if (eh->e_shentsize != sizeof(ElfW(Shdr))) return 0; /* Elf32-shaped table*/

    const ElfW(Shdr) *sh = (const ElfW(Shdr) *)(buf + eh->e_shoff);
    const ElfW(Shdr) *shstr = &sh[eh->e_shstrndx];
    const char *stab = (const char *)(buf + shstr->sh_offset);

    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (strcmp(stab + sh[i].sh_name, want) == 0)
            return (uint32_t)sh[i].sh_offset;
    }
    return 0;
}

int main(void)
{
    static uint8_t buf[4096];
    uint32_t sv_off = 0;
    size_t total = build_fixture(buf, sizeof buf, &sv_off);
    (void)total;

    /* 1) The elf32 container walk finds .vms$sv (would FAIL if ElfW() were the
     *    wrong width and misparsed the Elf32 section table). */
    uint32_t found = find_section_off(buf, OVMX_SV_SECTION);
    if (found == 0) { fprintf(stderr, "FAIL: .vms$sv not located in elf32-vax image\n"); return 1; }
    if (found != sv_off) { fprintf(stderr, "FAIL: wrong .vms$sv offset %u != %u\n", found, sv_off); return 1; }

    const struct ovmx_sv_header *h =
        (const struct ovmx_sv_header *)(buf + found);
    if (h->magic != OVMX_SV_MAGIC) { fprintf(stderr, "FAIL: bad .vms$sv magic\n"); return 1; }

    /* 2) Resolve by vector index through the REAL resolver. */
    uint64_t a = ovmx_sv_resolve(h, SV_MYADD, LOAD_BIAS, 1, 50);
    if (a != (uint64_t)LOAD_BIAS + VAL_MYADD) {
        fprintf(stderr, "FAIL: PROCEDURE resolve = 0x%llx (want 0x%x)\n",
                (unsigned long long)a, LOAD_BIAS + VAL_MYADD); return 1;
    }
    uint64_t d = ovmx_sv_resolve(h, SV_MYMUL, LOAD_BIAS, 1, 50);
    if (d != (uint64_t)LOAD_BIAS + VAL_MYMUL) {
        fprintf(stderr, "FAIL: DATA resolve = 0x%llx\n", (unsigned long long)d); return 1;
    }
    /* GLOBALVALUE binds WITHOUT the load bias (VMS globalvalue semantics). */
    uint64_t g = ovmx_sv_resolve(h, SV_GV, LOAD_BIAS, 1, 50);
    if (g != VAL_GV) {
        fprintf(stderr, "FAIL: GLOBALVALUE resolve = 0x%llx (want 0x%x)\n",
                (unsigned long long)g, VAL_GV); return 1;
    }

    /* 3) Bad index is refused (VMS "bad symbol-vector index"). */
    if (ovmx_sv_resolve(h, SV_COUNT, LOAD_BIAS, 1, 50) != 0) {
        fprintf(stderr, "FAIL: out-of-range index resolved non-zero\n"); return 1;
    }
    uint32_t bad_index = h->count + 500;   /* runtime-derived: no bogus -Warray-bounds */
    if (ovmx_sv_at(h, bad_index) != 0) {
        fprintf(stderr, "FAIL: ovmx_sv_at() out-of-range not NULL\n"); return 1;
    }

    /* 4) GSMATCH teeth: a major-version mismatch, and a LEQUAL minor that is
     *    newer than the image, are both refused. */
    if (ovmx_sv_resolve(h, SV_MYADD, LOAD_BIAS, 2, 50) != 0) {
        fprintf(stderr, "FAIL: GSMATCH major mismatch resolved non-zero\n"); return 1;
    }
    if (ovmx_sv_resolve(h, SV_MYADD, LOAD_BIAS, 1, 200) != 0) {
        fprintf(stderr, "FAIL: GSMATCH LEQUAL newer-than-image resolved non-zero\n"); return 1;
    }
    /* Same/older minor is accepted (LEQUAL: image minor 100 >= requested). */
    if (ovmx_sv_resolve(h, SV_MYADD, LOAD_BIAS, 1, 100) != (uint64_t)LOAD_BIAS + VAL_MYADD) {
        fprintf(stderr, "FAIL: GSMATCH LEQUAL exact-minor rejected\n"); return 1;
    }

    printf("PASS: elf32-vax .vms$sv resolved (myadd@0x%x, mymul@0x%x, GV=0x%x) "
           "via the real IMGACT resolver + Elf32 width abstraction\n",
           LOAD_BIAS + VAL_MYADD, LOAD_BIAS + VAL_MYMUL, VAL_GV);
    return 0;
}
