/*
 * evax_shareable_verify.c — integration check for LINK.EXE's EVAX/Alpha
 * SHAREABLE emit (bead vms-c65). Reads the ELF ET_DYN image LINK.EXE produced by
 *   LINK.EXE --shareable --symbol-vector "FOO=PROCEDURE,BAR=DATA"
 *            --gsmatch LEQUAL,1,0 -o FOO$SHR.EXE shr_lib.obj
 * and asserts a genuine, IMGACT-consumable Alpha symbol-vector producer:
 *
 *   - it is an EM_ALPHA ET_DYN image with $CODE$/$DATA$/$LINK$ + .vms$sv;
 *   - it carries NO .vms$xfer (a shareable has no transfer address);
 *   - the .vms$sv parses with the SAME reader IMGACT / the ELF shareable use
 *     (ovmx_symvec.h): magic OVMX_SV_MAGIC, count 2, GSMATCH LEQUAL/1/0;
 *   - sv#0 FOO is OVMX_SV_PROCEDURE, value == $LINK$ base (FOO's PROCEDURE
 *     DESCRIPTOR — VMS's real "value" of an Alpha procedure symbol; vms-32e1),
 *     and the PDSC's code-entry quad (*(PDSC+8)) resolves back to $CODE$ base;
 *   - sv#1 BAR is OVMX_SV_DATA,      value == $DATA$ base (BAR's data addr);
 *   - each entry's name blob string is exactly "FOO" / "BAR";
 *   - GSMATCH accepts a consumer linked at (1,0) and (1, older), and rejects a
 *     newer-minor or wrong-major consumer — via the shared ovmx_gsmatch_ok;
 *   - .vms$rel is present (BAR points to FOO: an image-relative data pointer to
 *     be load-biased at activation) with a valid ovmx_rel_header.
 *
 * Section addresses (FOO's $CODE$ base, BAR's $DATA$ base) are read from the ELF
 * section header table — the sv values are checked against those independently
 * derived addresses, not echoed from LINK.EXE's stdout.
 *
 * Byte-compatibility is the point: an Alpha shareable and an x86_64 shareable
 * differ only in e_machine + .text arch; this same reader validates both. Pure
 * ELF byte parsing — architecture-independent, no Alpha toolchain needed.
 * argv[1] = the linked shareable.  Exit 0 = all assertions pass.
 */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ovmx_image.h"
#include "ovmx_symvec.h"   /* the SAME reader IMGACT + the ELF shareable path use */

#ifndef EM_ALPHA
#define EM_ALPHA 0x9026
#endif

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
    else         { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <FOO$SHR.EXE>\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "empty image\n"); return 2; }
    uint8_t *img = malloc((size_t)sz); size_t len = (size_t)sz;
    if (fread(img, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); return 2; }
    fclose(f);

    if (len < sizeof(Elf64_Ehdr) || memcmp(img, ELFMAG, SELFMAG) != 0) {
        printf("FAIL: not an ELF image\n"); return 1;
    }
    Elf64_Ehdr eh; memcpy(&eh, img, sizeof eh);
    CHECK(eh.e_type == ET_DYN, "shareable is ET_DYN");
    CHECK(eh.e_machine == EM_ALPHA, "shareable is EM_ALPHA (machine=0x%x)", eh.e_machine);
    CHECK(eh.e_entry == 0, "shareable has no entry point (e_entry == 0)");

    /* A shareable must NOT carry PT_INTERP (it is read as a producer, never
     * kernel-activated) — matching the ELF emit_shareable header. */
    int has_interp = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph; memcpy(&ph, img + eh.e_phoff + (uint64_t)i * sizeof ph, sizeof ph);
        if (ph.p_type == PT_INTERP) has_interp = 1;
    }
    CHECK(!has_interp, "shareable carries NO PT_INTERP (read as a producer)");

    /* Locate sections (identity-mapped: sh_offset == sh_addr). */
    Elf64_Shdr shstrhdr;
    memcpy(&shstrhdr, img + eh.e_shoff + (uint64_t)eh.e_shstrndx * sizeof(Elf64_Shdr), sizeof shstrhdr);
    const char *shstr = (const char *)(img + shstrhdr.sh_offset);
    uint64_t code = 0, data = 0, link = 0, sv_off = 0, rel_off = 0;
    int have_code = 0, have_data = 0, have_link = 0, have_sv = 0, have_xfer = 0, have_rel = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh; memcpy(&sh, img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        const char *nm = shstr + sh.sh_name;
        if      (!strcmp(nm, "$CODE$"))    { code = sh.sh_addr; have_code = 1; }
        else if (!strcmp(nm, "$DATA$"))    { data = sh.sh_addr; have_data = 1; }
        else if (!strcmp(nm, "$LINK$"))    { link = sh.sh_addr; have_link = 1; }
        else if (!strcmp(nm, ".vms$sv"))   { sv_off = sh.sh_offset; have_sv = 1; }
        else if (!strcmp(nm, ".vms$xfer")) { have_xfer = 1; }
        else if (!strcmp(nm, ".vms$rel"))  { rel_off = sh.sh_offset; have_rel = 1; }
    }
    CHECK(have_code, "$CODE$ section present");
    CHECK(have_data, "$DATA$ section present");
    CHECK(have_link, "$LINK$ section present (holds FOO's procedure descriptor)");
    CHECK(have_sv,   ".vms$sv section present (symbol vector emitted)");
    CHECK(!have_xfer, "NO .vms$xfer (a shareable has no transfer address)");
    if (!have_sv) { printf("\n%d assertion(s) FAILED (no .vms$sv)\n", failures); return 1; }

    /* --- Parse .vms$sv with the SHARED reader (format-parity proof). --- */
    const struct ovmx_sv_header *h = (const struct ovmx_sv_header *)(img + sv_off);
    CHECK(h->magic == OVMX_SV_MAGIC, ".vms$sv magic == OVMX_SV_MAGIC (got 0x%08x)", h->magic);
    CHECK(h->count == 2, ".vms$sv count == 2 universals (got %u)", h->count);
    CHECK(h->gsmatch_kind == OVMX_GSMATCH_LEQUAL,
          "GSMATCH kind == LEQUAL (got %u)", h->gsmatch_kind);
    CHECK(h->gsmatch_major == 1 && h->gsmatch_minor == 0,
          "GSMATCH major.minor == 1.0 (got %u.%u)", h->gsmatch_major, h->gsmatch_minor);

    const char *names = ovmx_sv_names(h);

    /* sv#0 FOO — a PROCEDURE whose universal value is its PROCEDURE DESCRIPTOR
     * (PDSC, in $LINK$), not the raw code entry: VMS's real "value" of an Alpha
     * procedure symbol. IMGACT loads R27 = PV = this PDSC and derives the code
     * entry = *(PV+8) at activation (vms-32e1 — the code-entry form left IMGACT's
     * linkage quad[1]=PV NULL and SEGV'd decc$main's prologue). The image is
     * identity-mapped (sh_offset == sh_addr), so the PDSC is readable at
     * img + value, and its code-entry quad *(PDSC+8) ties back to $CODE$ base. */
    const struct ovmx_sv_entry *e0 = ovmx_sv_at(h, 0);
    CHECK(e0 != NULL, "sv#0 resolves through ovmx_sv_at (shared reader)");
    if (e0) {
        CHECK(e0->kind == OVMX_SV_PROCEDURE, "sv#0 kind == PROCEDURE (got %u)", e0->kind);
        CHECK(strcmp(names + e0->name_off, "FOO") == 0,
              "sv#0 name == \"FOO\" (got \"%s\")", names + e0->name_off);
        CHECK(e0->value == link,
              "sv#0 FOO value == $LINK$ base 0x%llx (got 0x%llx) — bound to the PROCEDURE DESCRIPTOR",
              (unsigned long long)link, (unsigned long long)e0->value);
        /* Positive PDSC->code-entry tie: the descriptor's PDSC$Q_ENTRY quad
         * (offset +8) holds FOO's code entry, which is $CODE$ base. */
        if (e0->value + 16 <= len) {
            uint64_t pdsc_entry;
            memcpy(&pdsc_entry, img + e0->value + 8, sizeof pdsc_entry);
            CHECK(pdsc_entry == code,
                  "sv#0 FOO PDSC code-entry quad *(PV+8) == $CODE$ base 0x%llx (got 0x%llx)",
                  (unsigned long long)code, (unsigned long long)pdsc_entry);
        } else {
            CHECK(0, "sv#0 FOO PDSC (0x%llx) lies within the image (len 0x%llx)",
                  (unsigned long long)e0->value, (unsigned long long)len);
        }
    }

    /* sv#1 BAR — a DATA symbol bound to BAR's data address (== $DATA$ base). */
    const struct ovmx_sv_entry *e1 = ovmx_sv_at(h, 1);
    CHECK(e1 != NULL, "sv#1 resolves through ovmx_sv_at (shared reader)");
    if (e1) {
        CHECK(e1->kind == OVMX_SV_DATA, "sv#1 kind == DATA (got %u)", e1->kind);
        CHECK(strcmp(names + e1->name_off, "BAR") == 0,
              "sv#1 name == \"BAR\" (got \"%s\")", names + e1->name_off);
        CHECK(e1->value == data,
              "sv#1 BAR value == $DATA$ base 0x%llx (got 0x%llx) — bound to the data addr",
              (unsigned long long)data, (unsigned long long)e1->value);
    }

    /* --- GSMATCH policy via the shared checker (what a consumer link enforces). --- */
    CHECK(ovmx_gsmatch_ok(h, 1, 0), "GSMATCH accepts a consumer linked at 1.0 (exact)");
    CHECK(!ovmx_gsmatch_ok(h, 1, 5), "GSMATCH rejects a consumer needing a newer minor (1.5)");
    CHECK(!ovmx_gsmatch_ok(h, 2, 0), "GSMATCH rejects a wrong-major consumer (2.0)");

    /* --- .vms$rel present (BAR -> FOO is an image-relative data pointer). --- */
    CHECK(have_rel, ".vms$rel present (image-relative data pointer BAR -> FOO)");
    if (have_rel) {
        const struct ovmx_rel_header *rh = (const struct ovmx_rel_header *)(img + rel_off);
        CHECK(rh->magic == OVMX_REL_MAGIC, ".vms$rel magic == OVMX_REL_MAGIC (got 0x%08x)", rh->magic);
        CHECK(rh->count >= 1, ".vms$rel records >= 1 load-bias fixup (got %u)", rh->count);
    }

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX SHAREABLE (vms-c65) FORMAT-PARITY CHECKS PASSED\n");
    return 0;
}
