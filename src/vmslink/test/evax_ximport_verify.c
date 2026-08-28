/*
 * evax_ximport_verify.c — integration check for LINK.EXE's EVAX/Alpha CROSS-IMAGE
 * import binding (bead vms-c179). Reads the ELF ET_DYN image LINK.EXE produced by
 * linking link_main.obj ALONE (its HELPER_PROC target absent from the object set)
 * against a --use'd producer shareable that EXPORTS HELPER_PROC, and asserts the
 * genuine cross-image mechanism was emitted:
 *
 *   - it is an EM_ALPHA ET_DYN image with $CODE$/$DATA$/$LINK$ + .vms$imp;
 *   - every reference to the imported HELPER_PROC left its fill slot 0 (IMGACT
 *     fills it at activation): the LINKAGE pair quad[0] @ $CODE$+0x08 and quad[1]
 *     @ $CODE$+0x10, the REFQUAD data pointer @ $DATA$+0, and the REFQUAD @
 *     $LINK$+0 are all 0;
 *   - the ONE local REFQUAD (@ $LINK$+0x10 -> $CODE$ base) is still resolved
 *     directly (cross-image binding did not disturb the local reloc path);
 *   - the .vms$imp table names exactly the three import sites, each binding
 *     (producer soname "HELPER$SHR.EXE", symbol-vector index 0), with the linked
 *     GSMATCH (major/minor) recorded per record;
 *   - a .vms$xfer transfer vector is still present (transfer = MAIN_PROC).
 *
 * Expected patch_offs are re-derived INDEPENDENTLY from the section addresses
 * LINK.EXE assigned plus link_main.obj's known reloc offsets (LINKAGE @ $CODE$
 * +0x08, REFQUAD @ $DATA$+0, REFQUAD @ $LINK$+0) — not an echo of LINK.EXE.
 *
 * Pure ELF byte parsing — architecture-independent, no Alpha toolchain needed.
 * argv[1] = the linked image.  Exit 0 = all assertions pass.
 */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ovmx_image.h"   /* struct ovmx_imp_header / ovmx_imp_entry, magic */

#ifndef EM_ALPHA
#define EM_ALPHA 0x9026
#endif

/* link_main.obj reloc offsets against the external HELPER_PROC (from the reader
 * dump: LINKAGE psect=$CODE$@0x08, REFQUAD psect=$DATA$@0x00, REFQUAD
 * psect=$LINK$@0x00; the fourth reloc @ $LINK$+0x10 -> $CODE$ is LOCAL). */
#define LINKAGE_OFF_IN_CODE  0x08
#define DATA_IMPORT_OFF      0x00
#define LINK_IMPORT_OFF      0x00
#define LINK_LOCAL_OFF       0x10   /* -> $CODE$ base (local, must stay resolved) */

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
    const char *producer = "HELPER$SHR.EXE";
    if (argc < 2) { fprintf(stderr, "usage: %s <linked.exe>\n", argv[0]); return 2; }
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

    /* Locate sections (identity-mapped: sh_offset == sh_addr). */
    const char *shstr = (const char *)(g_img + ((Elf64_Shdr *)(g_img + eh.e_shoff))[eh.e_shstrndx].sh_offset);
    uint64_t code = 0, data = 0, link = 0, xfer = 0, imp = 0, imp_sz = 0;
    int have_code = 0, have_data = 0, have_link = 0, have_xfer = 0, have_imp = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh; memcpy(&sh, g_img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        const char *nm = shstr + sh.sh_name;
        if (!strcmp(nm, "$CODE$"))         { code = sh.sh_addr; have_code = 1; }
        else if (!strcmp(nm, "$DATA$"))    { data = sh.sh_addr; have_data = 1; }
        else if (!strcmp(nm, "$LINK$"))    { link = sh.sh_addr; have_link = 1; }
        else if (!strcmp(nm, ".vms$xfer")) { xfer = sh.sh_addr; have_xfer = 1; }
        else if (!strcmp(nm, ".vms$imp"))  { imp = sh.sh_addr; imp_sz = sh.sh_size; have_imp = 1; }
    }
    CHECK(have_code, "$CODE$ section present");
    CHECK(have_data, "$DATA$ section present");
    CHECK(have_link, "$LINK$ section present");
    CHECK(have_xfer, ".vms$xfer section present");
    CHECK(have_imp,  ".vms$imp section present (cross-image imports emitted)");
    if (failures) { printf("\n%d assertion(s) FAILED (missing sections)\n", failures); return 1; }

    /* Independently-derived import patch sites. */
    uint64_t linkage_q0 = code + LINKAGE_OFF_IN_CODE;   /* R27-loaded quad[0]     */
    uint64_t linkage_q1 = code + LINKAGE_OFF_IN_CODE + 8;
    uint64_t data_site  = data + DATA_IMPORT_OFF;
    uint64_t link_site  = link + LINK_IMPORT_OFF;

    /* --- Every imported-symbol slot is left 0 for IMGACT to fill. --- */
    CHECK(rdl64(linkage_q0) == 0,
          "LINKAGE quad[0] @ $CODE$+0x08 left 0 (IMGACT fills = imported entry)");
    CHECK(rdl64(linkage_q1) == 0,
          "LINKAGE quad[1] @ $CODE$+0x10 left 0 (no local PDSC for an import)");
    CHECK(rdl64(data_site) == 0,
          "REFQUAD data pointer @ $DATA$+0 left 0 (IMGACT fills = imported addr)");
    CHECK(rdl64(link_site) == 0,
          "REFQUAD @ $LINK$+0 left 0 (IMGACT fills = imported addr)");

    /* --- The one LOCAL REFQUAD is still resolved directly (not an import). --- */
    CHECK(rdl64(link + LINK_LOCAL_OFF) == code,
          "local REFQUAD @ $LINK$+0x10 = $CODE$ base 0x%llx (got 0x%llx) — local reloc path intact",
          (unsigned long long)code, (unsigned long long)rdl64(link + LINK_LOCAL_OFF));

    /* --- .vms$imp table: header + 3 records naming (producer, sv#0, patch_off). --- */
    struct ovmx_imp_header ih; memcpy(&ih, g_img + imp, sizeof ih);
    CHECK(ih.magic == OVMX_IMP_MAGIC, ".vms$imp magic == 'IMP1' (got 0x%08x)", ih.magic);
    CHECK(ih.count == 3, ".vms$imp count == 3 import records (got %u)", ih.count);
    CHECK(imp_sz >= sizeof(struct ovmx_imp_header) + 3u * sizeof(struct ovmx_imp_entry),
          ".vms$imp holds header + 3 entries (size 0x%llx)", (unsigned long long)imp_sz);

    const char *names = (const char *)(g_img + imp + ih.names_off);

    /* Match each record to an expected site; every record must bind sv#0 of the
     * one producer. We collect the patch_offs and require the set to equal the
     * three independently-derived sites. */
    uint64_t want[3] = { linkage_q0, data_site, link_site };
    int seen[3] = { 0, 0, 0 };
    for (uint32_t k = 0; k < ih.count && k < 3; k++) {
        struct ovmx_imp_entry ie;
        memcpy(&ie, g_img + imp + sizeof(struct ovmx_imp_header) + (uint64_t)k * sizeof ie, sizeof ie);
        const char *pn = names + ie.producer_off;
        CHECK(strcmp(pn, producer) == 0,
              "record[%u] producer soname == \"%s\" (got \"%s\")", k, producer, pn);
        /* The producer symbol-vector index lives in the low 31 bits; bit31
         * (OVMX_IMP_LINKAGE) carries the EVAX/Alpha import FORM, not the index,
         * so it MUST be masked off before the index compare — exactly the
         * contract IMGACT reads by (imgact.c: sidx = sv_index & ~OVMX_IMP_LINKAGE).
         * vms-32e1. */
        CHECK((ie.sv_index & ~OVMX_IMP_LINKAGE) == 0,
              "record[%u] symbol-vector index == 0 (HELPER_PROC, got %u)",
              k, ie.sv_index & ~OVMX_IMP_LINKAGE);
        int matched = -1;
        for (int j = 0; j < 3; j++) if (!seen[j] && ie.patch_off == want[j]) { matched = j; break; }
        CHECK(matched >= 0,
              "record[%u] patch_off 0x%llx is one of the 3 import sites {0x%llx,0x%llx,0x%llx}",
              k, (unsigned long long)ie.patch_off,
              (unsigned long long)want[0], (unsigned long long)want[1], (unsigned long long)want[2]);
        if (matched >= 0) {
            seen[matched] = 1;
            /* POSITIVE form-bit check: the LINKAGE-pair import (site 0 =
             * linkage_q0 @ $CODE$+0x08, a 2-quadword procedure linkage) MUST
             * carry OVMX_IMP_LINKAGE; the two REFQUAD DATA imports ($DATA$ and
             * $LINK$, sites 1 and 2) MUST NOT. This is the on-disk encoding
             * IMGACT keys the 2-quad PV fill off — so the test now pins BOTH
             * the index (masked, above) AND the form (here). vms-32e1. */
            int want_linkage = (matched == 0);
            int have_linkage = (ie.sv_index & OVMX_IMP_LINKAGE) != 0;
            CHECK(have_linkage == want_linkage,
                  "record[%u] (%s import) OVMX_IMP_LINKAGE bit31 %s (sv_index=0x%08x)",
                  k, want_linkage ? "LINKAGE" : "DATA",
                  want_linkage ? "set" : "clear", ie.sv_index);
        }
    }
    CHECK(seen[0], "LINKAGE quad[0] site 0x%llx bound in .vms$imp", (unsigned long long)want[0]);
    CHECK(seen[1], "$DATA$ import site 0x%llx bound in .vms$imp",   (unsigned long long)want[1]);
    CHECK(seen[2], "$LINK$ import site 0x%llx bound in .vms$imp",   (unsigned long long)want[2]);

    /* --- .vms$xfer still present + names the local MAIN_PROC transfer. --- */
    CHECK(rdl32(xfer + 0) == 0x31465358u, ".vms$xfer magic == 'XSF1' (got 0x%08x)", rdl32(xfer + 0));
    CHECK(eh.e_entry == rdl64(xfer + 16), "e_entry == .vms$xfer entry[0] (MAIN_PROC transfer)");

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX CROSS-IMAGE IMPORT (vms-c179) CHECKS PASSED\n");
    return 0;
}
