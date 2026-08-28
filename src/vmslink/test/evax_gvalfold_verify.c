/*
 * evax_gvalfold_verify.c — integration check for LINK.EXE's EVAX/Alpha
 * GLOBALVALUE FOLD in the cross-image path (bead vms-069).
 *
 * A VMS globalvalue is an ABSOLUTE LINK-TIME CONSTANT — its address IS the
 * value — so VMS resolves it at LINK, folding the constant straight into the
 * reference site, never binding it through an activation import cell. This
 * verifier reads the ELF ET_DYN image LINK.EXE produced by linking the EVAX
 * fixture ref.obj (a `.quad C$_EXIT1` REFQUAD @ $DATA$+0, C$_EXIT1 undefined
 * in the object set) against a --use'd producer that exports
 * `C$_EXIT1=GLOBALVALUE:0x0035A009`, and asserts:
 *
 *   - it is an EM_ALPHA ET_DYN image with $CODE$/$DATA$ + a .vms$xfer;
 *   - the REFQUAD site @ $DATA$+0 holds the FOLDED absolute constant
 *     0x0035A009 (NOT left 0 for IMGACT, NOT biased by a psect base);
 *   - there is NO .vms$imp section at all — a folded globalvalue creates no
 *     cross-image import record (contrast run_evax_ximport, where a real
 *     PROCEDURE export DOES emit .vms$imp).
 *
 * The expected fold value is passed on argv (independently of LINK.EXE) and the
 * site offset is re-derived from the section address LINK.EXE assigned plus
 * ref.obj's known reloc offset ($DATA$+0). Pure ELF byte parsing —
 * architecture-independent, no Alpha toolchain needed to READ the result.
 * argv[1] = linked image, argv[2] = expected fold constant (0x..). Exit 0 = pass.
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

#define DATA_FOLD_OFF 0x00   /* ref.obj: REFQUAD against C$_EXIT1 @ $DATA$+0 */

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

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <linked.exe> <expected-fold-hex>\n", argv[0]); return 2; }
    uint64_t want = strtoull(argv[2], NULL, 0);

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "empty image\n"); return 2; }
    g_img = malloc((size_t)sz); g_len = (size_t)sz;
    if (fread(g_img, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 2; }
    fclose(f);

    if (g_len < sizeof(Elf64_Ehdr) || memcmp(g_img, ELFMAG, SELFMAG) != 0) {
        printf("FAIL: not an ELF image\n"); return 1;
    }
    Elf64_Ehdr eh; memcpy(&eh, g_img, sizeof eh);
    CHECK(eh.e_type == ET_DYN, "image is ET_DYN");
    CHECK(eh.e_machine == EM_ALPHA, "image is EM_ALPHA (machine=0x%x)", eh.e_machine);

    const char *shstr = (const char *)(g_img + ((Elf64_Shdr *)(g_img + eh.e_shoff))[eh.e_shstrndx].sh_offset);
    uint64_t code = 0, data = 0, xfer = 0;
    int have_code = 0, have_data = 0, have_xfer = 0, have_imp = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh; memcpy(&sh, g_img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        const char *nm = shstr + sh.sh_name;
        if (!strcmp(nm, "$CODE$"))              { code = sh.sh_addr; have_code = 1; }
        else if (!strcmp(nm, "$DATA$"))         { data = sh.sh_addr; have_data = 1; }
        else if (!strcmp(nm, OVMX_XFER_SECTION)){ xfer = sh.sh_addr; have_xfer = 1; }
        else if (!strcmp(nm, OVMX_IMP_SECTION)) { have_imp = 1; }
    }
    (void)code; (void)xfer;
    CHECK(have_code, "$CODE$ section present");
    CHECK(have_data, "$DATA$ section present");
    CHECK(have_xfer, ".vms$xfer section present");
    if (failures) { printf("\n%d assertion(s) FAILED (missing sections)\n", failures); return 1; }

    /* The globalvalue was FOLDED to its absolute constant at the reference site
     * — the constant sits in the slot, NOT 0 (an IMGACT-filled import) and NOT
     * a base-relocated address. */
    uint64_t site = data + DATA_FOLD_OFF;
    uint64_t got  = rdl64(site);
    CHECK(got == want,
          "REFQUAD @ $DATA$+0 (0x%llx) holds FOLDED constant 0x%llx (got 0x%llx)",
          (unsigned long long)site, (unsigned long long)want, (unsigned long long)got);
    CHECK(got != 0,
          "fold site is NOT left 0 (a folded globalvalue is a link-time constant, not an IMGACT-filled import)");

    /* A folded globalvalue emits NO cross-image import: there must be no
     * .vms$imp section at all (contrast the PROCEDURE import in run_evax_ximport). */
    CHECK(!have_imp,
          "NO .vms$imp section (globalvalue folded at link, never an activation import)");

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX GLOBALVALUE-FOLD (vms-069) CHECKS PASSED\n");
    return 0;
}
