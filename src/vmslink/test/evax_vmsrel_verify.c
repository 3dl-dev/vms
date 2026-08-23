/*
 * evax_vmsrel_verify.c — verify-first check for LINK.EXE's EVAX/Alpha .vms$rel
 * load-bias fixup table (bead vms-b5a0).
 *
 * THE BUG THIS CLOSES: a cc1-compiled program with an initialized global pointer
 * (`const char *g_ptr = greeting;` -> a REFQUAD in $DATA$ pointing at greeting)
 * links via LINK.EXE's EVAX path. Before this fix the image carried NO .vms$rel,
 * so g_ptr held greeting's IMAGE-RELATIVE address UNBIASED — correct only at load
 * bias 0. Under a non-zero IMGACT load bias B, greeting moves to B+off but g_ptr
 * still held off -> a wrong pointer. The fix makes LINK.EXE record every unbiased
 * image-relative pointer slot in .vms$rel; IMGACT's apply_vms_rel adds the bias.
 *
 * This verifier proves BOTH halves — EMIT and CONSUME — so a .vms$rel that IMGACT
 * would ignore cannot pass as a fix (INV-6):
 *
 *   emit-consume <image> <biashex>
 *     EMIT:    the image HAS a .vms$rel with magic 'REL1', count>=1, and the slot
 *              holding g_ptr ($DATA$+0) is in its offset list; that slot's stored
 *              value (pre-bias) is greeting's image-relative address ($READONLY$).
 *     CONSUME: replicate IMGACT's EXACT re-bias walk (imgact.c apply_vms_rel:938,
 *              `for each off: *(base+off) += base`) at the non-zero bias B, then
 *              assert g_ptr resolves to B + greeting_off — the actual runtime
 *              address. Before bias g_ptr == greeting_off; after, B+greeting_off.
 *
 *   no-rel <image>
 *     Assert the image has NO .vms$rel section. Used on a globalvalue-only fixture
 *     (`.quad C$_EXIT1`, folded to an ABSOLUTE link-time constant): a folded
 *     globalvalue is NOT an image-relative address, so it must NOT be recorded.
 *
 *   import-exclude <image>
 *     Assert every .vms$imp cross-image import patch_off slot is ABSENT from
 *     .vms$rel (IMGACT fills those with final addresses; biasing them would
 *     double-count), while at least one section-relative slot IS recorded.
 *
 * Pure ELF byte parsing — architecture-independent, no Alpha toolchain needed.
 * Exit 0 = all assertions pass.
 */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ovmx_image.h"

#ifndef EM_ALPHA
#define EM_ALPHA 0x9026
#endif

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
static uint32_t rdl32(uint64_t off)
{
    if (off + 4 > g_len) { printf("FAIL: read past image at 0x%llx\n", (unsigned long long)off); exit(2); }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)g_img[off + i] << (8 * i);
    return v;
}

/* Find a section by name; returns 1 and sets addr and size, else 0. Mirrors how
 * IMGACT's ovmx_find_section locates .vms$rel — by NAME in the section headers. */
static int find_sec(const char *name, uint64_t *addr, uint64_t *size)
{
    Elf64_Ehdr eh; memcpy(&eh, g_img, sizeof eh);
    const Elf64_Shdr *shs = (const Elf64_Shdr *)(g_img + eh.e_shoff);
    const char *shstr = (const char *)(g_img + shs[eh.e_shstrndx].sh_offset);
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh; memcpy(&sh, g_img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        if (!strcmp(shstr + sh.sh_name, name)) {
            if (addr) *addr = sh.sh_addr;
            if (size) *size = sh.sh_size;
            return 1;
        }
    }
    return 0;
}

/* Load the whole .vms$rel offset list into out[] (cap entries); returns count,
 * -1 if the section is absent. Asserts magic == 'REL1'. */
static int load_rel(uint64_t *out, int cap)
{
    uint64_t rel, relsz;
    if (!find_sec(OVMX_REL_SECTION, &rel, &relsz)) return -1;
    CHECK(rdl32(rel) == OVMX_REL_MAGIC, ".vms$rel magic == 'REL1' (got 0x%08x)", rdl32(rel));
    int n = (int)rdl32(rel + 4);
    for (int i = 0; i < n && i < cap; i++)
        out[i] = rdl64(rel + sizeof(struct ovmx_rel_header) + (uint64_t)i * 8);
    return n;
}

static int in_list(uint64_t v, const uint64_t *a, int n)
{
    for (int i = 0; i < n; i++) if (a[i] == v) return 1;
    return 0;
}

static void load_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "empty image\n"); exit(2); }
    g_img = malloc((size_t)sz); g_len = (size_t)sz;
    if (fread(g_img, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); exit(2); }
    fclose(f);
    if (g_len < sizeof(Elf64_Ehdr) || memcmp(g_img, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "not an ELF image\n"); exit(2);
    }
}

/* ------------------------------------------------------------------ modes --- */

static int mode_emit_consume(const char *path, uint64_t bias)
{
    load_image(path);
    Elf64_Ehdr eh; memcpy(&eh, g_img, sizeof eh);
    CHECK(eh.e_machine == EM_ALPHA, "image is EM_ALPHA (machine=0x%x)", eh.e_machine);

    /* Independently derive the two addresses from the section headers LINK
     * assigned: g_ptr is the sole quad in $DATA$ (gdata.s: `.data / g_ptr: .quad
     * greeting`), greeting is in $READONLY$ (`.rdata`). */
    uint64_t data = 0, ro = 0;
    int have_data = find_sec("$DATA$", &data, NULL);
    int have_ro   = find_sec("$READONLY$", &ro, NULL);
    CHECK(have_data, "$DATA$ section present (holds g_ptr)");
    CHECK(have_ro,   "$READONLY$ section present (holds greeting)");
    if (failures) return 1;

    uint64_t gptr_slot = data;            /* g_ptr lives at $DATA$+0            */
    uint64_t greeting  = ro;              /* greeting == $READONLY$ base         */

    /* --- EMIT half. --- */
    uint64_t rel[256];
    int nrel = load_rel(rel, 256);
    CHECK(nrel >= 1, ".vms$rel present with count>=1 (got %d)", nrel);
    if (nrel < 0) return 1;
    CHECK(nrel == 3, ".vms$rel records exactly the 3 image-relative REFQUADs "
          "gdata.obj emits (got %d)", nrel);
    CHECK(in_list(gptr_slot, rel, nrel),
          "g_ptr slot ($DATA$+0 = image-relative 0x%llx) is in the .vms$rel list",
          (unsigned long long)gptr_slot);

    uint64_t stored = rdl64(gptr_slot);
    CHECK(stored == greeting,
          "PRE-bias: g_ptr holds greeting's image-relative addr 0x%llx (got 0x%llx)",
          (unsigned long long)greeting, (unsigned long long)stored);

    /* --- CONSUME half: replicate IMGACT apply_vms_rel (imgact.c:938):
     *        for each off in .vms$rel: *(base + off) += base
     * with base == the simulated non-zero load bias. In a real activation the
     * image is mmap'd at `base`, so an image-relative slot value V becomes the
     * runtime address base+V; here we add `bias` to the stored value in place. */
    for (int i = 0; i < nrel; i++) {
        uint64_t off = rel[i];
        uint64_t v = rdl64(off) + bias;
        for (int b = 0; b < 8; b++) g_img[off + b] = (uint8_t)(v >> (8 * b));
    }

    uint64_t g_after = rdl64(gptr_slot);
    CHECK(g_after == bias + greeting,
          "POST-bias(B=0x%llx): g_ptr resolves to B+greeting_off = 0x%llx (got 0x%llx)",
          (unsigned long long)bias, (unsigned long long)(bias + greeting),
          (unsigned long long)g_after);

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nEVAX .vms$rel EMIT+CONSUME (g_ptr survives a non-zero load bias) PASSED\n");
    return 0;
}

static int mode_no_rel(const char *path)
{
    load_image(path);
    uint64_t a, s;
    int present = find_sec(OVMX_REL_SECTION, &a, &s);
    CHECK(!present, "globalvalue-only image has NO .vms$rel (folded absolute "
          "constants are not image-relative -> not recorded)");
    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nEVAX .vms$rel GLOBALVALUE-EXCLUSION PASSED\n");
    return 0;
}

static int mode_import_exclude(const char *path)
{
    load_image(path);

    uint64_t rel[256];
    int nrel = load_rel(rel, 256);
    CHECK(nrel >= 1, ".vms$rel present with a section-relative slot recorded "
          "(got %d)", nrel);

    /* Read every .vms$imp patch_off and assert none is in .vms$rel. */
    uint64_t imp = 0, impsz = 0;
    CHECK(find_sec(OVMX_IMP_SECTION, &imp, &impsz), ".vms$imp present (cross-image imports)");
    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }

    CHECK(rdl32(imp) == OVMX_IMP_MAGIC, ".vms$imp magic == 'IMP1' (got 0x%08x)", rdl32(imp));
    int nimp = (int)rdl32(imp + 4);
    CHECK(nimp >= 1, ".vms$imp has >=1 import record (got %d)", nimp);
    uint64_t ent = imp + sizeof(struct ovmx_imp_header);
    int excluded = 0;
    for (int i = 0; i < nimp; i++) {
        /* ovmx_imp_entry: producer_off(4) sv_index(4) patch_off(8) maj(4) min(4) = 24 */
        uint64_t patch_off = rdl64(ent + (uint64_t)i * 24 + 8);
        int bad = in_list(patch_off, rel, nrel);
        CHECK(!bad, "import patch_off 0x%llx is NOT in .vms$rel (IMGACT fills it; "
              "biasing it would double-count)", (unsigned long long)patch_off);
        if (!bad) excluded++;
    }
    CHECK(excluded == nimp, "all %d cross-image import slots excluded from .vms$rel", nimp);

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nEVAX .vms$rel CROSS-IMPORT-EXCLUSION PASSED\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s emit-consume <image> <biashex>\n"
                        "       %s no-rel <image>\n"
                        "       %s import-exclude <image>\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    if (!strcmp(mode, "emit-consume")) {
        if (argc < 4) { fprintf(stderr, "emit-consume needs <biashex>\n"); return 2; }
        uint64_t bias = strtoull(argv[3], NULL, 0);
        if (bias == 0) { fprintf(stderr, "bias must be non-zero to prove the fixup\n"); return 2; }
        return mode_emit_consume(argv[2], bias);
    }
    if (!strcmp(mode, "no-rel"))         return mode_no_rel(argv[2]);
    if (!strcmp(mode, "import-exclude")) return mode_import_exclude(argv[2]);
    fprintf(stderr, "unknown mode '%s'\n", mode);
    return 2;
}
