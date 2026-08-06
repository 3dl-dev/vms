/*
 * rodata_jumptable.c — the regression specimen for vms-a66.
 *
 * vms-a66: LINK.EXE collected relocations only against sections it bucketed
 * B_TEXT or B_DATA. Every relocation against a READ-ONLY allocatable section
 * (B_RODATA) was dropped in silence. gcc puts each `switch` jump table into a
 * read-only section (`.rodata`) encoded as `.long arm_label - table_base`; the
 * arms live in .text and the table lives in .rodata, so the assembler cannot
 * fold the difference and leaves one real R_X86_64_PC32 per arm. Dropping them
 * left the table ALL ZERO, and the dispatch sequence
 *   movslq (%rcx,%rsi,4),%rdx ; add %rcx,%rdx ; jmp *%rdx
 * computed table_base + 0 — i.e. it executed the table's own bytes.
 *
 * This specimen puts a jump table on BOTH sides of the image boundary the bug
 * straddled:
 *
 *   1. dispatch()/weigh() — the CONSUMER's own jump tables, in this
 *      executable's .rodata, patched by LINK.EXE when it emits the image.
 *   2. the printf/snprintf calls — the PRODUCER's jump tables, inside musl's
 *      printf_core / pop_arg / fmt_fp in DECC$SHR.EXE. That is the pair that
 *      actually killed DCL.EXE: pop_arg's dispatch jumped into the "(null)"
 *      string constant sitting in DECC$SHR's .rodata.
 *
 * IMPORTANT (codegen shape, not scale): the arms must do genuinely DIFFERENT
 * work. A switch whose arms only `return <constant>` is compiled by gcc into a
 * constant LOOKUP table — data, no relocations — and proves nothing here. The
 * harness asserts .rela.rodata is actually present so this cannot rot silently.
 *
 * Every computed value is printed, so a table that is SHUFFLED rather than
 * zeroed is caught too. The harness compares this program's output, activated
 * VMS-native through IMGACT.EXE, against the SAME source built by the system
 * toolchain (gcc + ld) — the expected values are the native toolchain's answer,
 * never a hand-written constant.
 */
#include <stdio.h>

/* Opaque helper: keeps two arms from folding into arithmetic. */
__attribute__((noinline)) static int bump(int x) { return x * 2 + 1; }

/* Dense 0..15 switch with distinct operations per arm -> a real jump table. */
__attribute__((noinline)) static int dispatch(int op, int a, int b)
{
    switch (op) {
    case 0:  return a + b;
    case 1:  return a - b;
    case 2:  return a * b;
    case 3:  return a ^ b;
    case 4:  return a | b;
    case 5:  return a & b;
    case 6:  return (a << 1) + b;
    case 7:  return (a >> 1) - b;
    case 8:  return bump(a) + b;
    case 9:  return a + bump(b);
    case 10: return a * 3 + b * 5;
    case 11: return a - b * 7;
    case 12: return (a + b) * (a - b);
    case 13: return a ? b : -b;
    case 14: return b ? a : -a;
    case 15: return a % (b | 1);
    default: return -1000;
    }
}

/* A second, char-keyed switch so the specimen does not rest on one shape. */
__attribute__((noinline)) static int weigh(char k, int n)
{
    switch (k) {
    case 'a': return n + 1;
    case 'b': return n * 2;
    case 'c': return n - 3;
    case 'd': return n << 2;
    case 'e': return n ^ 0x55;
    case 'f': return n | 0x0f;
    case 'g': return n & 0x3c;
    case 'h': return bump(n);
    case 'i': return n * n;
    case 'j': return -n;
    default:  return 0;
    }
}

int main(int argc, char **argv)
{
    char buf[192];
    int base = argc - 1;              /* 0 under both harness runs; opaque */
    int total = 0;

    (void)argv;
    for (int i = 0; i < 16; i++) {
        int v = dispatch(base + i, 12, 5);
        printf("D%02d=%d\n", i, v);
        total += v;
    }
    printf("DISPATCH-TOTAL=%d\n", total);

    int w = 0;
    for (const char *p = "abcdefghij"; *p; p++) {
        int v = weigh((char)(*p + base), 9);
        printf("W%c=%d\n", *p, v);
        w += v;
    }
    printf("WEIGH-TOTAL=%d\n", w);

    /* Drives musl's printf_core (conversion dispatch), pop_arg (argument-type
     * dispatch) and fmt_fp — all jump tables inside DECC$SHR's .rodata. */
    snprintf(buf, sizeof buf, "%d|%s|%c|%x|%o|%lu|%.3s|%5d|%-4s|%.2f|%%",
             42, "str", 'Z', 0xbeefu, 0755u, 1234567890UL, "abcdef", 7, "xy",
             3.14159);
    printf("FMT=%s\n", buf);
    printf("PASS\n");
    return 0;
}
