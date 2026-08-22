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

    if (failures) { printf("\n%d assertion(s) FAILED\n", failures); return 1; }
    printf("\nALL EVAX READER (slice 1) CHECKS PASSED\n");
    return 0;
}
