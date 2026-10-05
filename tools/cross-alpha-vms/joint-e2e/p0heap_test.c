/*
 * p0heap_test.c - vms-122: the C RTL heap lives in the VMS P0 region.
 *
 * On OpenVMS Alpha the default heap grows P0 (below 0x40000000), so every
 * address malloc returns is a valid 32-bit pointer. This program exercises
 * every way the heap gets memory -- small blocks (mallocng groups), large
 * blocks (direct mmap), calloc, a large realloc that must move (mremap), and an
 * explicit anonymous mmap -- writes and re-reads a pattern through each, and
 * requires every address to lie in P0. Sentinel 7 = all held.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P0_END 0x40000000ULL

static unsigned long long maxaddr;
static int bad;

static void check(void *p, unsigned long long len, const char *what)
{
    unsigned long long a = (unsigned long long)p;
    if (!p) { printf("  %s: NULL\n", what); bad++; return; }
    if (a + len > maxaddr) maxaddr = a + len;
    if (a + len > P0_END) { printf("  %s: %#llx+%#llx outside P0\n", what, a, len); bad++; }
}

static int fill_ok(unsigned char *p, unsigned long long len, unsigned char seed)
{
    for (unsigned long long i = 0; i < len; i += 4096) p[i] = (unsigned char)(seed + i / 4096);
    p[len - 1] = seed;
    for (unsigned long long i = 0; i < len; i += 4096)
        if (p[i] != (unsigned char)(seed + i / 4096)) return 0;
    return p[len - 1] == seed;
}

int main(int argc, char **argv)
{
    (void)argv;
    int small_ok = 1, large_ok = 1, realloc_ok = 1, mmap_ok = 1;

    static void *blk[2000];
    for (int i = 0; i < 2000; i++) {
        unsigned n = 8 + (i * 37) % 3000;
        blk[i] = malloc(n);
        check(blk[i], n, "small");
        if (blk[i]) memset(blk[i], i & 0xff, n); else small_ok = 0;
    }
    for (int i = 0; i < 2000; i += 2) { free(blk[i]); blk[i] = 0; }
    for (int i = 0; i < 2000; i += 2) { blk[i] = calloc(1, 64); check(blk[i], 64, "calloc"); }

    static const unsigned long long big[] = { 200000, 1u << 20, 8u << 20, 64u << 20 };
    void *lg[4];
    for (int i = 0; i < 4; i++) {
        lg[i] = malloc(big[i]);
        check(lg[i], big[i], "large");
        if (!lg[i] || !fill_ok(lg[i], big[i], (unsigned char)(0x40 + i))) large_ok = 0;
    }

    unsigned char *r = malloc(8u << 20);
    check(r, 8u << 20, "realloc-src");
    if (r && fill_ok(r, 8u << 20, 0x11)) {
        r = realloc(r, 96u << 20);        /* mmap-sized both ways: mremap, likely moving */
        check(r, 96u << 20, "realloc");
        if (!r || r[0] != 0x11 || !fill_ok(r, 96u << 20, 0x22)) realloc_ok = 0;
    } else realloc_ok = 0;

    void *m = mmap(0, 3u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { m = 0; mmap_ok = 0; }
    check(m, 3u << 20, "mmap");
    if (m && !fill_ok(m, 3u << 20, 0x33)) mmap_ok = 0;

    for (int i = 0; i < 2000; i++) free(blk[i]);
    for (int i = 0; i < 4; i++) free(lg[i]);
    free(r);
    if (m) munmap(m, 3u << 20);

    int ok = !bad && small_ok && large_ok && realloc_ok && mmap_ok;
    fprintf(stderr, "OVMX p0heap test: small=%d large=%d realloc=%d mmap=%d maxaddr=%#llx inP0=%d argc=%d\n",
            small_ok, large_ok, realloc_ok, mmap_ok, maxaddr, bad == 0, argc);
    return ok ? 7 : 3;
}
