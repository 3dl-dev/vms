/*
 * evax_read_test.c — unit test for the EVAX object reader (bead vms-cbe).
 *
 * Parses a real EVAX object (test/evax-fixtures/sample.obj, produced by
 * binutils-2.43 alpha-dec-vms-as from sample.s) and asserts the sections +
 * symbols the reader extracts match what `alpha-dec-vms-objdump -h -t` reports:
 *   sections: $CODE$ (0x30), $DATA$ (0), $BSS$ (0), $LINK$ (0x30)
 *   symbols : EXAMPLE_PROC (defined procedure, in psect $LINK$ = index 3)
 *             EXT_ROUTINE  (undefined external reference)
 * Takes the fixture path as argv[1]. Exit 0 = all assertions pass.
 */
#include "../evax_read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static const struct evax_section *find_sec(const struct evax_object *o, const char *n)
{
    for (int i = 0; i < o->nsec; i++)
        if (strcmp(o->sec[i].name, n) == 0) return &o->sec[i];
    return NULL;
}
static const struct evax_symbol *find_sym(const struct evax_object *o, const char *n)
{
    for (int i = 0; i < o->nsym; i++)
        if (strcmp(o->sym[i].name, n) == 0) return &o->sym[i];
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <sample.obj>\n", argv[0]); return 2; }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    static uint8_t buf[1 << 20];
    size_t len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (len == 0) { fprintf(stderr, "empty fixture\n"); return 2; }

    CHECK(evax_is_object(buf, len), "evax_is_object should recognize the fixture");

    struct evax_object o;
    if (evax_read(buf, len, &o) != 0) {
        printf("FAIL: evax_read: %s\n", evax_last_error());
        return 1;
    }

    printf("module='%s'  nsec=%d  nsym=%d\n", o.module, o.nsec, o.nsym);
    for (int i = 0; i < o.nsec; i++)
        printf("  psect[%d] %-8s flags=0x%04x alloc=0x%llx align=2^%u\n",
               i, o.sec[i].name, o.sec[i].flags,
               (unsigned long long)o.sec[i].alloc, o.sec[i].align);
    for (int i = 0; i < o.nsym; i++)
        printf("  sym[%d]   %-14s %s%s psindx=%u value=0x%llx\n",
               i, o.sym[i].name, o.sym[i].defined ? "DEF" : "UND",
               o.sym[i].is_proc ? " PROC" : "",
               o.sym[i].psindx, (unsigned long long)o.sym[i].value);

    CHECK(strcmp(o.module, "SAMPLE") == 0, "module name should be SAMPLE (got '%s')", o.module);

    CHECK(o.nsec == 4, "expected 4 psects, got %d", o.nsec);
    const struct evax_section *code = find_sec(&o, "$CODE$");
    const struct evax_section *data = find_sec(&o, "$DATA$");
    const struct evax_section *bss  = find_sec(&o, "$BSS$");
    const struct evax_section *link = find_sec(&o, "$LINK$");
    CHECK(code && code->alloc == 0x30, "$CODE$ present, alloc 0x30");
    CHECK(data && data->alloc == 0,    "$DATA$ present, alloc 0");
    CHECK(bss  && bss->alloc  == 0,    "$BSS$ present, alloc 0");
    CHECK(link && link->alloc == 0x30, "$LINK$ present, alloc 0x30");

    CHECK(o.nsym == 2, "expected 2 symbols, got %d", o.nsym);
    const struct evax_symbol *ep = find_sym(&o, "EXAMPLE_PROC");
    const struct evax_symbol *er = find_sym(&o, "EXT_ROUTINE");
    CHECK(ep && ep->defined,  "EXAMPLE_PROC present and defined");
    CHECK(ep && ep->is_proc,  "EXAMPLE_PROC is a normal procedure (EGSY__V_NORM)");
    CHECK(ep && ep->psindx == 3, "EXAMPLE_PROC defined in psect index 3 ($LINK$)");
    CHECK(er && !er->defined, "EXT_ROUTINE present and UNDEFINED (external ref)");

    /* --- relocations (slice 2): must match alpha-dec-vms-objdump -r ---
     *   $CODE$ (psect 0): LINKAGE @0x8  -> EXT_ROUTINE
     *   $LINK$ (psect 3): REFQUAD @0x0  -> EXT_ROUTINE
     *                     REFQUAD @0x10 -> section $CODE$ (index 0) */
    static const char *rtn[] = { "REFLONG","REFQUAD","CODEADDR","LINKAGE","NOP","BSR","LDA","BOH" };
    for (int i = 0; i < o.nreloc; i++) {
        const struct evax_reloc *r = &o.reloc[i];
        printf("  reloc[%d] %-8s psect=%d @0x%llx -> %s addend=0x%llx\n",
               i, rtn[r->type], r->psect, (unsigned long long)r->address,
               r->to_section >= 0 ? o.sec[r->to_section].name : r->sym,
               (unsigned long long)r->addend);
    }
    CHECK(o.nreloc == 3, "expected 3 relocations, got %d", o.nreloc);

    int code_idx = -1, link_idx = -1;
    for (int i = 0; i < o.nsec; i++) {
        if (!strcmp(o.sec[i].name, "$CODE$")) code_idx = i;
        if (!strcmp(o.sec[i].name, "$LINK$")) link_idx = i;
    }
    int saw_linkage = 0, saw_extq = 0, saw_secq = 0;
    for (int i = 0; i < o.nreloc; i++) {
        const struct evax_reloc *r = &o.reloc[i];
        if (r->type == EVAX_R_LINKAGE && r->psect == code_idx && r->address == 0x8 &&
            r->to_section < 0 && !strcmp(r->sym, "EXT_ROUTINE"))
            saw_linkage = 1;
        if (r->type == EVAX_R_REFQUAD && r->psect == link_idx && r->address == 0x0 &&
            r->to_section < 0 && !strcmp(r->sym, "EXT_ROUTINE"))
            saw_extq = 1;
        if (r->type == EVAX_R_REFQUAD && r->psect == link_idx && r->address == 0x10 &&
            r->to_section == code_idx)
            saw_secq = 1;
    }
    CHECK(saw_linkage, "LINKAGE @ $CODE$+0x8 -> EXT_ROUTINE");
    CHECK(saw_extq,    "REFQUAD @ $LINK$+0x0 -> EXT_ROUTINE");
    CHECK(saw_secq,    "REFQUAD @ $LINK$+0x10 -> section $CODE$");

    /* --- slice 3: materialized psect content + procedure code entry ---
     * $CODE$'s first STO_IMM stored the function prologue; its first
     * instruction is `lda $sp,-16($sp)` = 0x23defff0 (bytes f0 ff de 23 LE).
     * EXAMPLE_PROC's code entry (EXAMPLE_PROC..en) is at $CODE$ (index 0) +0. */
    CHECK(code && code->content != NULL, "$CODE$ content materialized (STO_IMM)");
    if (code && code->content) {
        static const uint8_t want[4] = { 0xf0, 0xff, 0xde, 0x23 };
        CHECK(memcmp(code->content, want, 4) == 0,
              "$CODE$[0..4] = lda $sp,-16($sp) (0x23defff0)");
    }
    CHECK(ep && ep->code_psindx == 0, "EXAMPLE_PROC code entry in psect 0 ($CODE$)");
    CHECK(ep && ep->code_value  == 0, "EXAMPLE_PROC code entry at $CODE$+0");

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX READER (slice 1 psects/symbols + slice 2 relocs) CHECKS PASSED\n");
    return 0;
}
