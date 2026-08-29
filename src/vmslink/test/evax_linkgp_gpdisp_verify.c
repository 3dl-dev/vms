/*
 * evax_linkgp_gpdisp_verify.c — proves the OVMX-labeled EVAX_R_OVMX_GPDISP
 * relocation ROUND-TRIPS: gas emits it (into the checked-in fixture
 * linkgp_gpdisp.obj), OVMX LINK.EXE reads the OVMX-private ETIR__C_OVMX_GPDISP
 * command and APPLIES it, and the two patched ldah/lda immediates decode to
 * exactly -K, signed-split (bead vms-4ed, component C2 of the vms-5f5 authentic
 * Alpha per-image GP program; docs/design-alpha-per-image-gp.md §2.1/§2.2).
 *
 * [OVMX] NOT a VMS-authentic relocation: EVAX publishes no GP-displacement
 * encoding (OSF/Alpha uses ldgp/GPDISP), so this whole mechanism — the ETIR
 * command, the wire form, the ldah/lda re-base — is an OVMX design choice.
 *
 * Inputs: argv[1] = the linked EVAX shareable (GPDISP$SHR.EXE, from linking
 * linkgp_gpdisp.obj with `--symbol-vector "FIRST_PROC=PROCEDURE,
 * SECOND_PROC=PROCEDURE"`); argv[2] = the captured stdout+stderr log of that
 * LINK.EXE invocation (which printed one `%LINK-I-GPDISP` line per applied
 * relocation, giving the procedure and the image-relative ldah/lda addresses).
 *
 * INDEPENDENT ground truth (derived from the image, NEVER trusted from the log's
 * value): exactly as the C1 verifier does, K for a procedure = its .vms$sv PDSC
 * value minus the $LINK$ section base. From K we independently compute the
 * expected signed-split immediates:
 *     exp_hi = ((-K + 0x8000) >> 16) & 0xffff   (the ldah immediate)
 *     exp_lo = (-K) & 0xffff                     (the lda  immediate)
 * The log is used ONLY to LOCATE each patched pair (the ldah/lda image
 * addresses); the asserted VALUES are the on-image bytes vs. this independent
 * computation. So a stubbed apply (leaving 0/0), a wrong K, or the wrong split
 * all FAIL — SECOND_PROC's K is provably nonzero, so a 0/0 stub cannot pass.
 *
 * FIRST_PROC's PDSC sits at the linkage-section base (K==0 -> immediates 0/0, a
 * no-op-equivalent pair, the single-procedure cascade of §1.4); SECOND_PROC's is
 * at a nonzero offset (K!=0 -> a real -K patch). Both are asserted.
 *
 * Pure byte/text parsing, architecture-independent, no Alpha toolchain needed at
 * test time (the .obj fixture is checked in, produced offline by the PATCHED
 * alpha-dec-vms-as — see linkgp_gpdisp.s). Exit 0 = all assertions pass.
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

/* -K, signed-split standard ldah/lda pair (the SAME formula the apply uses; here
 * derived from ground-truth K so the assert is genuinely independent). */
static void split_negK(uint64_t k, uint16_t *hi, uint16_t *lo)
{
    uint64_t negK = (uint64_t)(-(int64_t)k);
    *hi = (uint16_t)(((negK + 0x8000) >> 16) & 0xffff);
    *lo = (uint16_t)(negK & 0xffff);
}

/* Map an image-relative virtual address to a file offset via the containing
 * section (robust whether or not sections are identity-mapped). */
static int va_to_off(const uint8_t *img, const Elf64_Ehdr *eh, uint64_t va, uint64_t *off)
{
    for (int i = 0; i < eh->e_shnum; i++) {
        Elf64_Shdr sh;
        memcpy(&sh, img + eh->e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        if (sh.sh_type == SHT_NOBITS || sh.sh_size == 0) continue;
        if (va >= sh.sh_addr && va < sh.sh_addr + sh.sh_size) {
            *off = sh.sh_offset + (va - sh.sh_addr);
            return 1;
        }
    }
    return 0;
}

static uint16_t insn_imm(const uint8_t *img, uint64_t off)
{
    /* Alpha memory-format instruction, little-endian; bits [15:0] = displacement. */
    uint32_t iw = (uint32_t)img[off] | ((uint32_t)img[off + 1] << 8)
                | ((uint32_t)img[off + 2] << 16) | ((uint32_t)img[off + 3] << 24);
    return (uint16_t)(iw & 0xffff);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <GPDISP$SHR.EXE> <link.log>\n", argv[0]);
        return 2;
    }

    size_t len;
    uint8_t *img = slurp(argv[1], &len);
    if (len < sizeof(Elf64_Ehdr) || memcmp(img, ELFMAG, SELFMAG) != 0) {
        printf("FAIL: not an ELF image\n"); return 1;
    }
    Elf64_Ehdr eh; memcpy(&eh, img, sizeof eh);

    /* $LINK$ base + .vms$sv (same independent ground-truth path as C1). */
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
    if (!e0 || !e1) { printf("FAIL: sv entries do not resolve\n"); return 1; }
    const struct ovmx_sv_entry *first = NULL, *second = NULL;
    const struct ovmx_sv_entry *cand[2] = { e0, e1 };
    for (int i = 0; i < 2; i++) {
        if (!strcmp(names + cand[i]->name_off, "FIRST_PROC"))  first  = cand[i];
        if (!strcmp(names + cand[i]->name_off, "SECOND_PROC")) second = cand[i];
    }
    CHECK(first  != NULL, "FIRST_PROC found in .vms$sv");
    CHECK(second != NULL, "SECOND_PROC found in .vms$sv");
    if (!first || !second) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }

    uint64_t exp_k_first  = first->value  - link_base;
    uint64_t exp_k_second = second->value - link_base;
    CHECK(exp_k_first == 0,
          "structural precondition: FIRST_PROC's PDSC sits AT the linkage-section base (K=0)");
    CHECK(exp_k_second != 0,
          "structural precondition: SECOND_PROC's PDSC sits at a NON-ZERO offset (K!=0) "
          "-- the only way this test distinguishes a real -K patch from a 0/0 stub");

    uint16_t exp_hi_first, exp_lo_first, exp_hi_second, exp_lo_second;
    split_negK(exp_k_first,  &exp_hi_first,  &exp_lo_first);
    split_negK(exp_k_second, &exp_hi_second, &exp_lo_second);
    printf("ground truth: link_base=0x%" PRIx64
           " | FIRST_PROC K=0x%" PRIx64 " -> ldah=0x%04x lda=0x%04x"
           " | SECOND_PROC K=0x%" PRIx64 " -> ldah=0x%04x lda=0x%04x\n",
           link_base, exp_k_first, exp_hi_first, exp_lo_first,
           exp_k_second, exp_hi_second, exp_lo_second);

    /* --- Parse the %LINK-I-GPDISP lines: LOCATION (ldah_va/lda_va) + proc only.
     * The applied imm values printed there are cross-checked, but the binding
     * assertion is the on-image bytes vs. the independent split above. --- */
    size_t loglen;
    uint8_t *logbuf = slurp(argv[2], &loglen);
    char *log = malloc(loglen + 1);
    memcpy(log, logbuf, loglen); log[loglen] = 0; free(logbuf);

    int seen_first = 0, seen_second = 0;
    const char *p = log;
    while ((p = strstr(p, "%LINK-I-GPDISP,")) != NULL) {
        char name[64];
        unsigned long long k, ldah_va, lda_va;
        unsigned lhi, llo;
        int n = sscanf(p, "%%LINK-I-GPDISP, EVAX_R_OVMX_GPDISP proc=%63s K=0x%llx "
                       "ldah_imm=0x%x lda_imm=0x%x ldah_va=0x%llx lda_va=0x%llx",
                       name, &k, &lhi, &llo, &ldah_va, &lda_va);
        p += 1;
        if (n != 6) continue;

        int is_first  = !strcmp(name, "FIRST_PROC");
        int is_second = !strcmp(name, "SECOND_PROC");
        if (!is_first && !is_second) continue;

        uint16_t exp_hi = is_first ? exp_hi_first : exp_hi_second;
        uint16_t exp_lo = is_first ? exp_lo_first : exp_lo_second;
        uint64_t exp_k  = is_first ? exp_k_first  : exp_k_second;

        /* Cross-check: what LINK printed it applied == independent split. */
        CHECK((uint16_t)lhi == exp_hi && (uint16_t)llo == exp_lo,
              "%s: LINK-reported applied imms (ldah=0x%04x lda=0x%04x) == independent "
              "-K split (0x%04x/0x%04x)", name, lhi, llo, exp_hi, exp_lo);
        CHECK((uint64_t)k == exp_k,
              "%s: LINK-reported K (0x%llx) == independent K (0x%" PRIx64 ")",
              name, k, exp_k);

        /* THE binding assertion: the ACTUAL on-image instruction immediates. */
        uint64_t ldah_off = 0, lda_off = 0;
        int okd = va_to_off(img, &eh, (uint64_t)ldah_va, &ldah_off);
        int okl = va_to_off(img, &eh, (uint64_t)lda_va, &lda_off);
        CHECK(okd && okl, "%s: ldah_va/lda_va map into a placed section", name);
        if (okd && okl) {
            uint16_t got_hi = insn_imm(img, ldah_off);
            uint16_t got_lo = insn_imm(img, lda_off);
            CHECK(got_hi == exp_hi,
                  "%s: ON-IMAGE ldah immediate (0x%04x) == HIGH(-K) (0x%04x)",
                  name, got_hi, exp_hi);
            CHECK(got_lo == exp_lo,
                  "%s: ON-IMAGE lda immediate (0x%04x) == LOW(-K) (0x%04x)",
                  name, got_lo, exp_lo);
            if (is_second) {
                /* Belt-and-suspenders: SECOND_PROC's patch is provably non-trivial
                 * (a 0/0 stub would fail this AND the equality above). */
                CHECK((got_hi | got_lo) != 0,
                      "SECOND_PROC: patched immediates are NON-zero (a stubbed 0/0 "
                      "apply is rejected)");
            }
            if (is_first) {
                CHECK(got_hi == 0 && got_lo == 0,
                      "FIRST_PROC: K==0 patched the pair to 0/0 (no-op-equivalent, "
                      "the single-procedure cascade of §1.4)");
            }
        }
        if (is_first)  seen_first  = 1;
        if (is_second) seen_second = 1;
    }
    CHECK(seen_first,
          "LINK.EXE applied (and reported) EVAX_R_OVMX_GPDISP for FIRST_PROC "
          "-- proves the recognizer read the gas-emitted command");
    CHECK(seen_second,
          "LINK.EXE applied (and reported) EVAX_R_OVMX_GPDISP for SECOND_PROC");

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX_R_OVMX_GPDISP ROUND-TRIP CHECKS PASSED (vms-4ed): "
           "gas emit -> OVMX read -> OVMX apply -K, both K=0 and K!=0\n");
    return 0;
}
