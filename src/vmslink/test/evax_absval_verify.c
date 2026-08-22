/*
 * evax_absval_verify.c — end-to-end check for an ABSOLUTE global symbol (a VMS
 * "globalvalue") DEFINED IN an EVAX object and REFERENCED across objects (bead
 * vms-1bc). This is the concrete blocker to linking a real OpenVMS GCC-port
 * `main()` object: the port emits `__gcc_main_flags = <flags>` (an absolute
 * global whose ADDRESS is the flags word) and its crt0 reads
 * `(unsigned __int64)&__gcc_main_flags`.
 *
 * Two independent axes are checked:
 *
 *  A. READER (evax_read): re-parse the two fixtures and assert the reader
 *     detected the absolute marker from the real EGSD bytes —
 *     __gcc_main_flags is_abs==1 with value==3 (DEF set, REL clear), while the
 *     normal psect-relative global REFUSER is is_abs==0. This is the
 *     discriminating new capability: without the reader change the flag does
 *     not exist. The constant 3 is FOLDED from the object's own EGSD `value`
 *     field, never hardcoded (INV-6).
 *
 *  B. LINKER (LINK.EXE EVAX path): read the linked ELF ET_DYN image and assert
 *     the `&__gcc_main_flags` site holds exactly the absolute constant 3 (no
 *     psect base, no load-bias), and — the no-regression control — the
 *     `&REFUSER` site in the SAME object still resolves to REFUSER's
 *     psect-relative image address (section base + value). The EVAX image path
 *     emits no .vms$rel load-bias list at all, so we also assert there is no
 *     such section biasing the absolute site.
 *
 * argv[1] = linked image; argv[2] = absval_def.obj; argv[3] = absval_ref.obj.
 * Expected slot offsets are re-derived from the linker's $DATA$ section header
 * address plus the fixtures' documented intra-$DATA$ layout (def first: REFUSER
 * at +0x00; ref next: FLAGSLOT at +0x08, NORMREF at +0x10) — see absval_def.s /
 * absval_ref.s. Pure byte parsing; no Alpha toolchain needed at test time.
 * Exit 0 = all assertions pass.
 */
#include "../evax_read.h"

#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef EM_ALPHA
#define EM_ALPHA 0x9026
#endif

/* Fixture-derived intra-$DATA$ layout (see absval_def.s / absval_ref.s). def's
 * $DATA$ (0x8: REFUSER) is placed first, ref's $DATA$ (0x10: FLAGSLOT, NORMREF)
 * next. */
#define REFUSER_OFF_IN_DATA   0x00
#define FLAGSLOT_OFF_IN_DATA  0x08
#define NORMREF_OFF_IN_DATA   0x10
#define ABS_FLAGS_CONST       3      /* __gcc_main_flags = 3 (absolute)          */

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
    else         { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint8_t *g_img;
static size_t   g_len;

static uint64_t rdl64(uint64_t off)
{
    if (off + 8 > g_len) { printf("FAIL: read past image at 0x%llx\n", (unsigned long long)off); exit(2); }
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)g_img[off + i] << (8 * i);
    return v;
}

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read %s\n", path); exit(2); }
    fclose(f); *n = (size_t)sz; return b;
}

/* Find the file offset (== vaddr, identity-mapped) + size of a named section. */
static int find_section(const char *want, uint64_t *addr, uint64_t *size)
{
    Elf64_Ehdr *eh = (Elf64_Ehdr *)g_img;
    Elf64_Shdr *sh = (Elf64_Shdr *)(g_img + eh->e_shoff);
    const char *shstr = (const char *)(g_img + sh[eh->e_shstrndx].sh_offset);
    for (int i = 0; i < eh->e_shnum; i++) {
        if (strcmp(shstr + sh[i].sh_name, want) == 0) {
            if (addr) *addr = sh[i].sh_addr;
            if (size) *size = sh[i].sh_size;
            return 1;
        }
    }
    return 0;
}

static const struct evax_symbol *find_sym(const struct evax_object *o, const char *n)
{
    for (int i = 0; i < o->nsym; i++)
        if (strcmp(o->sym[i].name, n) == 0) return &o->sym[i];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s IMAGE absval_def.obj absval_ref.obj\n", argv[0]);
        return 2;
    }

    /* ---- Axis A: reader detected the absolute marker from real EGSD bytes ---- */
    size_t dn, rn;
    uint8_t *db = slurp(argv[2], &dn);
    uint8_t *rb = slurp(argv[3], &rn);
    struct evax_object def, ref;
    if (evax_read(db, dn, &def) != 0) { printf("FAIL: evax_read def: %s\n", evax_last_error()); return 1; }
    if (evax_read(rb, rn, &ref) != 0) { printf("FAIL: evax_read ref: %s\n", evax_last_error()); return 1; }

    const struct evax_symbol *gmf = find_sym(&def, "__gcc_main_flags");
    const struct evax_symbol *ru  = find_sym(&def, "REFUSER");
    CHECK(gmf != NULL, "__gcc_main_flags present in absval_def.obj");
    CHECK(gmf && gmf->defined,  "__gcc_main_flags is a DEFINITION");
    CHECK(gmf && gmf->is_abs,   "__gcc_main_flags flagged ABSOLUTE (DEF set, REL clear)");
    CHECK(gmf && gmf->value == ABS_FLAGS_CONST,
          "__gcc_main_flags absolute value == %d (folded from EGSD)", ABS_FLAGS_CONST);
    CHECK(ru  && ru->defined && !ru->is_abs,
          "REFUSER is a normal psect-relative global (is_abs==0)");
    /* The reference object sees __gcc_main_flags as a pure undefined external. */
    const struct evax_symbol *ext = find_sym(&ref, "__gcc_main_flags");
    CHECK(ext && !ext->defined && !ext->is_abs,
          "__gcc_main_flags is an UNDEFINED external in absval_ref.obj");

    /* ---- Axis B: LINK.EXE folded the absolute site, biased the normal one ---- */
    g_img = slurp(argv[1], &g_len);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)g_img;
    CHECK(memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0, "output is ELF");
    CHECK(eh->e_type == ET_DYN, "output is ET_DYN");
    CHECK(eh->e_machine == EM_ALPHA, "output is EM_ALPHA");

    uint64_t data_addr = 0, data_size = 0;
    CHECK(find_section("$DATA$", &data_addr, &data_size), "$DATA$ section present");
    CHECK(data_size >= NORMREF_OFF_IN_DATA + 8, "$DATA$ large enough for all three slots");

    uint64_t refuser_addr = data_addr + REFUSER_OFF_IN_DATA;
    uint64_t flagslot     = rdl64(data_addr + FLAGSLOT_OFF_IN_DATA);
    uint64_t normref      = rdl64(data_addr + NORMREF_OFF_IN_DATA);

    printf("info: $DATA$@0x%llx  REFUSER@0x%llx  FLAGSLOT=0x%llx  NORMREF=0x%llx\n",
           (unsigned long long)data_addr, (unsigned long long)refuser_addr,
           (unsigned long long)flagslot, (unsigned long long)normref);

    /* The absolute globalvalue site holds the CONSTANT, not psect_base+const. */
    CHECK(flagslot == ABS_FLAGS_CONST,
          "&__gcc_main_flags folded to absolute constant %d (not psect_base+%d)",
          ABS_FLAGS_CONST, ABS_FLAGS_CONST);
    /* No-regression: the normal cross-object symbol resolved to its BIASED
     * image address (section base + value), which is a real vaddr, not 3. */
    CHECK(normref == refuser_addr,
          "&REFUSER resolved to REFUSER's psect-relative image address 0x%llx",
          (unsigned long long)refuser_addr);
    CHECK(refuser_addr != ABS_FLAGS_CONST,
          "REFUSER's image address distinguishes the biased path from the fold");

    /* The EVAX image path emits no .vms$rel load-bias list; assert the absolute
     * site is therefore not enrolled for biasing (there is no such section). */
    CHECK(!find_section(".vms$rel", NULL, NULL),
          "no .vms$rel section biases the absolute site (EVAX path emits none)");

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX ABSOLUTE-GLOBALVALUE (vms-1bc) CHECKS PASSED\n");
    return 0;
}
