/*
 * evax_linkgp_verify.c — proves LINK.EXE's per-image linkage-section base +
 * per-procedure K computation (bead vms-fd5, component C1 of the vms-5f5
 * authentic Alpha per-image GP program; docs/design-alpha-per-image-gp.md
 * §1.4/§2.3).
 *
 * Inputs: argv[1] = the linked EVAX shareable (GPTEST$SHR.EXE, from linking the
 * synthetic linkgp_two_proc.obj fixture with `--symbol-vector
 * "FIRST_PROC=PROCEDURE,SECOND_PROC=PROCEDURE"`); argv[2] = the captured
 * stdout+stderr log of that LINK.EXE invocation, run with OVMX_LINK_DUMP_GP=1
 * so it printed one `%LINK-I-GPENTRY` diagnostic line per recorded procedure
 * plus one `%LINK-I-GPBASE` summary line.
 *
 * INDEPENDENT ground truth (derived from the image, never echoed from the log):
 *   - the $LINK$ section's ELF-header address == the per-image linkage base;
 *   - the .vms$sv table's FIRST_PROC/SECOND_PROC universal values are each
 *     procedure's real PDSC address (VMS's "value" of a procedure symbol —
 *     vms-32e1), read via the SAME shared reader (ovmx_symvec.h) IMGACT uses.
 * From these, expected K = pdsc_addr - link_base for each procedure.
 *
 * The fixture places FIRST_PROC's PDSC first in $LINK$ (expected K == 0) and
 * SECOND_PROC's second, at a nonzero offset (expected K != 0) — the structural
 * precondition for a real per-procedure test: a single-procedure fixture could
 * pass with K hard-coded to 0 for everything; this one cannot, because
 * SECOND_PROC's expected K is only satisfied by an implementation that
 * actually reads its own PDSC placement.
 *
 * The test then parses LINK.EXE's diagnostic log and asserts what it RECORDED
 * (the base and each K) matches the independently-derived ground truth exactly
 * — proving the producer's internal state (not just the emitted image bytes)
 * is the authentic per-procedure K, not a stub.
 *
 * Pure byte/text parsing, architecture-independent, no Alpha toolchain needed
 * at test time (the fixture .obj is checked in, produced offline by
 * alpha-dec-vms-as — see linkgp_two_proc.s). Exit 0 = all assertions pass.
 */
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

#include "ovmx_image.h"
#include "ovmx_symvec.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
    else         { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static uint8_t *slurp(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fprintf(stderr, "%s: empty\n", path); exit(2); }
    uint8_t *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("fread"); exit(2); }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <GPTEST$SHR.EXE> <link.log>\n", argv[0]);
        return 2;
    }

    size_t len;
    uint8_t *img = slurp(argv[1], &len);

    if (len < sizeof(Elf64_Ehdr) || memcmp(img, ELFMAG, SELFMAG) != 0) {
        printf("FAIL: not an ELF image\n"); return 1;
    }
    Elf64_Ehdr eh; memcpy(&eh, img, sizeof eh);

    /* Locate $LINK$ and .vms$sv (identity-mapped: sh_offset == sh_addr). */
    Elf64_Shdr shstrhdr;
    memcpy(&shstrhdr, img + eh.e_shoff + (uint64_t)eh.e_shstrndx * sizeof(Elf64_Shdr),
           sizeof shstrhdr);
    const char *shstr = (const char *)(img + shstrhdr.sh_offset);
    uint64_t link_base = 0, sv_off = 0;
    int have_link = 0, have_sv = 0;
    for (int i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr sh; memcpy(&sh, img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        const char *nm = shstr + sh.sh_name;
        if      (!strcmp(nm, "$LINK$"))  { link_base = sh.sh_addr;  have_link = 1; }
        else if (!strcmp(nm, ".vms$sv")) { sv_off     = sh.sh_offset; have_sv = 1; }
    }
    CHECK(have_link, "$LINK$ section present (the linkage section holding both PDSCs)");
    CHECK(have_sv,   ".vms$sv section present (symbol vector emitted)");
    if (!have_link || !have_sv) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }

    const struct ovmx_sv_header *h = (const struct ovmx_sv_header *)(img + sv_off);
    CHECK(h->magic == OVMX_SV_MAGIC, ".vms$sv magic == OVMX_SV_MAGIC");
    CHECK(h->count == 2, ".vms$sv count == 2 universals (got %u)", h->count);
    const char *names = ovmx_sv_names(h);

    const struct ovmx_sv_entry *e0 = ovmx_sv_at(h, 0);
    const struct ovmx_sv_entry *e1 = ovmx_sv_at(h, 1);
    CHECK(e0 != NULL, "sv#0 resolves through ovmx_sv_at");
    CHECK(e1 != NULL, "sv#1 resolves through ovmx_sv_at");
    if (!e0 || !e1) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }

    /* Match sv entries to names (order is not contractually fixed). */
    const struct ovmx_sv_entry *first = NULL, *second = NULL;
    const struct ovmx_sv_entry *cand[2] = { e0, e1 };
    for (int i = 0; i < 2; i++) {
        if (!strcmp(names + cand[i]->name_off, "FIRST_PROC"))  first  = cand[i];
        if (!strcmp(names + cand[i]->name_off, "SECOND_PROC")) second = cand[i];
    }
    CHECK(first  != NULL, "FIRST_PROC found in .vms$sv");
    CHECK(second != NULL, "SECOND_PROC found in .vms$sv");
    if (!first || !second) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    CHECK(first->kind  == OVMX_SV_PROCEDURE, "FIRST_PROC is a PROCEDURE universal");
    CHECK(second->kind == OVMX_SV_PROCEDURE, "SECOND_PROC is a PROCEDURE universal");

    /* Ground truth: each universal's value IS the procedure's PDSC address
     * (vms-32e1), so K = pdsc_addr - link_base, independent of anything
     * LINK.EXE printed. */
    uint64_t exp_k_first  = first->value  - link_base;
    uint64_t exp_k_second = second->value - link_base;
    printf("ground truth: link_base=0x%" PRIx64 " FIRST_PROC pdsc=0x%" PRIx64
           " (K=0x%" PRIx64 ") SECOND_PROC pdsc=0x%" PRIx64 " (K=0x%" PRIx64 ")\n",
           link_base, first->value, exp_k_first, second->value, exp_k_second);

    CHECK(exp_k_first == 0,
          "structural precondition: FIRST_PROC's PDSC sits AT the linkage-section base (K=0)");
    CHECK(exp_k_second != 0,
          "structural precondition: SECOND_PROC's PDSC sits at a NON-ZERO offset (K!=0) "
          "-- the only way this test can distinguish a real per-procedure K from a "
          "hard-coded K=0 stub");

    /* --- Now parse what LINK.EXE actually RECORDED, from its diagnostic log. --- */
    size_t loglen;
    uint8_t *logbuf = slurp(argv[2], &loglen);
    char *log = malloc(loglen + 1);
    memcpy(log, logbuf, loglen);
    log[loglen] = 0;
    free(logbuf);

    int have_base_line = 0;
    uint64_t rec_base = 0;
    int rec_count = -1;
    {
        const char *p = strstr(log, "%LINK-I-GPBASE,");
        if (p) {
            unsigned long long b; int c;
            if (sscanf(p, "%%LINK-I-GPBASE, EVAX linkage-section ($LINK$) base=0x%llx count=%d",
                       &b, &c) == 2) {
                rec_base = (uint64_t)b; rec_count = c; have_base_line = 1;
            }
        }
    }
    CHECK(have_base_line, "LINK.EXE emitted a %%LINK-I-GPBASE diagnostic line");
    CHECK(have_base_line && rec_base == link_base,
          "RECORDED linkage-section base (0x%" PRIx64 ") == actual $LINK$ base (0x%" PRIx64 ")",
          rec_base, link_base);
    CHECK(have_base_line && rec_count == 2,
          "RECORDED entry count == 2 (got %d)", rec_count);

    /* Per-procedure K, one %LINK-I-GPENTRY line per recorded PDSC. */
    int found_first = 0, found_second = 0;
    uint64_t rec_k_first = 0, rec_k_second = 0;
    const char *p = log;
    while ((p = strstr(p, "%LINK-I-GPENTRY,")) != NULL) {
        char name[64]; unsigned long long pdsc, k;
        if (sscanf(p, "%%LINK-I-GPENTRY, proc=%63s pdsc=0x%llx k=0x%llx",
                   name, &pdsc, &k) == 3) {
            if (!strcmp(name, "FIRST_PROC"))  { found_first  = 1; rec_k_first  = (uint64_t)k; }
            if (!strcmp(name, "SECOND_PROC")) { found_second = 1; rec_k_second = (uint64_t)k; }
        }
        p += 1;
    }
    CHECK(found_first,  "LINK.EXE emitted a %%LINK-I-GPENTRY line for FIRST_PROC");
    CHECK(found_second, "LINK.EXE emitted a %%LINK-I-GPENTRY line for SECOND_PROC");

    /* THE assertion that proves per-procedure K, not a constant: FIRST_PROC's
     * recorded K must be 0 AND SECOND_PROC's recorded K must be the (nonzero)
     * ground-truth offset. A K hard-coded to 0 for every procedure passes the
     * first check but FAILS the second -- exp_k_second is provably != 0. */
    CHECK(found_first && rec_k_first == exp_k_first,
          "RECORDED FIRST_PROC K (0x%" PRIx64 ") == ground truth (0x%" PRIx64 ")",
          rec_k_first, exp_k_first);
    CHECK(found_second && rec_k_second == exp_k_second,
          "RECORDED SECOND_PROC K (0x%" PRIx64 ") == ground truth (0x%" PRIx64 ") "
          "-- genuinely per-procedure, not a hard-coded 0",
          rec_k_second, exp_k_second);

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX LINKAGE-SECTION-BASE + PER-PROCEDURE K CHECKS PASSED (vms-fd5)\n");
    return 0;
}
