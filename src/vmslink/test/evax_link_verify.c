/*
 * evax_link_verify.c — integration check for LINK.EXE's EVAX/Alpha path
 * (bead vms-cbe, slices 3+4). Reads the ELF ET_DYN image LINK.EXE produced from
 * the two-object EVAX fixture (link_main.obj + link_helper.obj) and asserts:
 *
 *   - it is an EM_ALPHA ET_DYN image with the psects placed as sections;
 *   - every relocation was applied to the correct S+A value (byte-level),
 *     including the 2-quadword Alpha LINKAGE pair;
 *   - a .vms$xfer section is present with magic 'XSF1', flavor 1, count 1, and
 *     the single entry == the MAIN_PROC transfer (procedure descriptor) address.
 *
 * Expected values are re-derived INDEPENDENTLY from the section header addresses
 * LINK.EXE assigned plus the fixture's known intra-object layout (documented
 * below, tied to link_main.s / link_helper.s), so this is a genuine end-to-end
 * check of the placement + relocation math, not an echo of the linker's output.
 *
 * Pure ELF byte parsing — architecture-independent, no Alpha toolchain needed.
 * argv[1] = the linked image. Exit 0 = all assertions pass.
 */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifndef EM_ALPHA
#define EM_ALPHA 0x9026
#endif

/* Fixture-derived constants (see link_main.s / link_helper.s + objdump -h/-t):
 *   link_main:   $CODE$ 0x30 (align 16), $DATA$ 0x08, $LINK$ 0x30 (align 16)
 *   link_helper: $CODE$ 0x10 (align 16), $LINK$ 0x20 (align 16)
 * merged, main first: helper.$CODE$ is at ALIGN16(0x30)=+0x30 within $CODE$;
 * helper.$LINK$ is at ALIGN16(0x30)=+0x30 within $LINK$.
 *   HELPER_PROC: code entry at helper.$CODE$+0, descriptor at helper.$LINK$+0.
 *   MAIN_PROC:   descriptor at main.$LINK$+0x08. */
#define HELPER_CODE_OFF_IN_CODE  0x30
#define HELPER_LINK_OFF_IN_LINK  0x30
#define MAIN_PDESC_OFF_IN_LINK   0x08

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

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <linked.exe>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "empty image\n"); return 2; }
    g_img = malloc((size_t)sz); g_len = (size_t)sz;
    if (fread(g_img, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 2; }
    fclose(f);

    /* ELF header. */
    if (g_len < sizeof(Elf64_Ehdr) || memcmp(g_img, ELFMAG, SELFMAG) != 0) {
        printf("FAIL: not an ELF image\n"); return 1;
    }
    Elf64_Ehdr eh; memcpy(&eh, g_img, sizeof eh);
    CHECK(eh.e_type == ET_DYN, "image is ET_DYN");
    CHECK(eh.e_machine == EM_ALPHA, "image is EM_ALPHA (machine=0x%x)", eh.e_machine);

    /* Walk the section headers; identity-mapped (sh_offset == sh_addr). */
    const char *shstr = (const char *)(g_img + ((Elf64_Shdr *)(g_img + eh.e_shoff))[eh.e_shstrndx].sh_offset);
    uint64_t code = 0, data = 0, link = 0, xfer = 0, xfer_sz = 0;
    int have_code = 0, have_data = 0, have_link = 0, have_xfer = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh; memcpy(&sh, g_img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        const char *nm = shstr + sh.sh_name;
        if (!strcmp(nm, "$CODE$"))    { code = sh.sh_addr; have_code = 1; CHECK(sh.sh_offset == sh.sh_addr, "$CODE$ identity-mapped"); }
        else if (!strcmp(nm, "$DATA$")) { data = sh.sh_addr; have_data = 1; }
        else if (!strcmp(nm, "$LINK$")) { link = sh.sh_addr; have_link = 1; }
        else if (!strcmp(nm, ".vms$xfer")) { xfer = sh.sh_addr; xfer_sz = sh.sh_size; have_xfer = 1; }
    }
    CHECK(have_code, "$CODE$ section present");
    CHECK(have_data, "$DATA$ section present");
    CHECK(have_link, "$LINK$ section present");
    CHECK(have_xfer, ".vms$xfer section present");
    if (failures) { printf("\n%d assertion(s) FAILED (missing sections)\n", failures); return 1; }

    /* Independently derived expected resolved addresses. */
    uint64_t helper_code  = code + HELPER_CODE_OFF_IN_CODE;   /* HELPER_PROC entry     */
    uint64_t helper_pdesc = link + HELPER_LINK_OFF_IN_LINK;   /* HELPER_PROC descriptor */
    uint64_t main_pdesc   = link + MAIN_PDESC_OFF_IN_LINK;    /* MAIN_PROC descriptor  */
    uint64_t helper_link  = link + HELPER_LINK_OFF_IN_LINK;   /* helper.$LINK$ base    */

    /* --- LINKAGE pair @ main.$CODE$+0x08 = quad[0] code entry, quad[1] descriptor. */
    CHECK(rdl64(code + 0x08) == helper_code,
          "LINKAGE quad[0] @ $CODE$+0x08 = HELPER code entry 0x%llx (got 0x%llx)",
          (unsigned long long)helper_code, (unsigned long long)rdl64(code + 0x08));
    CHECK(rdl64(code + 0x10) == helper_pdesc,
          "LINKAGE quad[1] @ $CODE$+0x10 = HELPER descriptor 0x%llx (got 0x%llx)",
          (unsigned long long)helper_pdesc, (unsigned long long)rdl64(code + 0x10));

    /* --- REFQUAD data pointer @ $DATA$+0 -> HELPER_PROC descriptor. */
    CHECK(rdl64(data + 0x00) == helper_pdesc,
          "REFQUAD $DATA$+0 = HELPER descriptor 0x%llx (got 0x%llx)",
          (unsigned long long)helper_pdesc, (unsigned long long)rdl64(data + 0x00));

    /* --- REFQUAD $LINK$+0 -> HELPER descriptor; $LINK$+0x10 -> main.$CODE$ base. */
    CHECK(rdl64(link + 0x00) == helper_pdesc,
          "REFQUAD $LINK$+0 = HELPER descriptor 0x%llx (got 0x%llx)",
          (unsigned long long)helper_pdesc, (unsigned long long)rdl64(link + 0x00));
    CHECK(rdl64(link + 0x10) == code,
          "REFQUAD $LINK$+0x10 = main.$CODE$ base 0x%llx (got 0x%llx)",
          (unsigned long long)code, (unsigned long long)rdl64(link + 0x10));

    /* --- REFQUAD helper.$LINK$+0x08 -> helper.$CODE$ base. */
    CHECK(rdl64(helper_link + 0x08) == helper_code,
          "REFQUAD helper.$LINK$+0x08 = helper.$CODE$ base 0x%llx (got 0x%llx)",
          (unsigned long long)helper_code, (unsigned long long)rdl64(helper_link + 0x08));

    /* --- .vms$xfer header + entry. */
    CHECK(xfer_sz >= 24, ".vms$xfer holds a 16-byte header + >=1 entry (size 0x%llx)",
          (unsigned long long)xfer_sz);
    CHECK(rdl32(xfer + 0)  == 0x31465358u, ".vms$xfer magic == 'XSF1' (got 0x%08x)", rdl32(xfer + 0));
    CHECK(rdl32(xfer + 4)  == 1u, ".vms$xfer flavor == OVMX_ACT_VMS_STD (1) (got %u)", rdl32(xfer + 4));
    CHECK(rdl32(xfer + 8)  == 1u, ".vms$xfer count == 1 (got %u)", rdl32(xfer + 8));
    CHECK(rdl32(xfer + 12) == 0u, ".vms$xfer reserved == 0 (got %u)", rdl32(xfer + 12));
    CHECK(rdl64(xfer + 16) == main_pdesc,
          ".vms$xfer entry[0] = MAIN_PROC transfer 0x%llx (got 0x%llx)",
          (unsigned long long)main_pdesc, (unsigned long long)rdl64(xfer + 16));
    CHECK(eh.e_entry == main_pdesc, "e_entry = MAIN_PROC transfer 0x%llx (got 0x%llx)",
          (unsigned long long)main_pdesc, (unsigned long long)eh.e_entry);

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX LINK (slices 3+4: placement + reloc apply + .vms$xfer) CHECKS PASSED\n");
    return 0;
}
