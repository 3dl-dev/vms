/* Correctness proof for the OTS$ integer-divide family (bead vms-bfd6).
 *
 * The routines in ots_runtime.c use ONLY shifts/subtracts/compares — no target-
 * specific anything — so the exact same C compiles and runs natively. Here we
 * link them on the host and check every result against the host C `/` and `%`
 * across an edge-case matrix (negatives, by-1, by-large, zero numerator, mixed
 * signs, INT/LLONG limits), plus the division identity q*d + r == n.
 *
 * Divide-by-zero is intentionally NOT exercised (host `/` by zero is UB / traps;
 * matches the bead's own test list which asks only for zero-NUMERATOR). INT_MIN
 * / -1 overflows and TRAPS under host `/` (SIGFPE), so for that single cell we
 * do not compare to native — we assert our routine does not trap and returns
 * the two's-complement wrap, exactly the latitude C's UB and the hardware give.
 */
#include <stdio.h>
#include <limits.h>

extern int                OTS$DIV_I (int, int);
extern int                OTS$REM_I (int, int);
extern unsigned           OTS$DIV_UI(unsigned, unsigned);
extern unsigned           OTS$REM_UI(unsigned, unsigned);
extern long long          OTS$DIV_L (long long, long long);
extern long long          OTS$REM_L (long long, long long);
extern unsigned long long OTS$DIV_UL(unsigned long long, unsigned long long);
extern unsigned long long OTS$REM_UL(unsigned long long, unsigned long long);

static int fails = 0, checks = 0;

#define CK(cond, fmt, ...) do { checks++; if (!(cond)) { \
    fails++; printf("FAIL: " fmt "\n", __VA_ARGS__); } } while (0)

int main(void)
{
    static const int si[] = { 0, 1, -1, 2, -2, 3, -3, 7, -7, 100, -100,
                              12345, -12345, 65536, -65536,
                              INT_MAX, INT_MIN, INT_MAX-1, INT_MIN+1 };
    static const unsigned ui[] = { 0u, 1u, 2u, 3u, 7u, 100u, 12345u, 65536u,
                                   0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu };
    static const long long sl[] = { 0, 1, -1, 2, -2, 7, -7, 1000000, -1000000,
                                    4294967296LL, -4294967296LL,
                                    1234567890123LL, -1234567890123LL,
                                    LLONG_MAX, LLONG_MIN, LLONG_MAX-1, LLONG_MIN+1 };
    static const unsigned long long ul[] = { 0ull, 1ull, 2ull, 7ull, 1000000ull,
                                             4294967296ull, 1234567890123ull,
                                             0x7fffffffffffffffull, 0x8000000000000000ull,
                                             0xffffffffffffffffull, 0xfffffffffffffffeull };
    int n;

    /* signed 32 */
    n = (int)(sizeof si / sizeof si[0]);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        int a = si[i], b = si[j];
        if (b == 0) continue;
        int q = OTS$DIV_I(a, b), r = OTS$REM_I(a, b);
        if (a == INT_MIN && b == -1) {          /* overflow cell: no native compare */
            CK(q == INT_MIN, "DIV_I overflow %d/%d -> %d (want wrap INT_MIN)", a, b, q);
            continue;
        }
        CK(q == a / b, "DIV_I %d/%d = %d want %d", a, b, q, a / b);
        CK(r == a % b, "REM_I %d%%%d = %d want %d", a, b, r, a % b);
        CK((long long)q * b + r == a, "identity_I %d/%d q=%d r=%d", a, b, q, r);
    }
    /* unsigned 32 */
    n = (int)(sizeof ui / sizeof ui[0]);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        unsigned a = ui[i], b = ui[j];
        if (b == 0) continue;
        unsigned q = OTS$DIV_UI(a, b), r = OTS$REM_UI(a, b);
        CK(q == a / b, "DIV_UI %u/%u = %u want %u", a, b, q, a / b);
        CK(r == a % b, "REM_UI %u%%%u = %u want %u", a, b, r, a % b);
        CK((unsigned long long)q * b + r == a, "identity_UI %u/%u", a, b, a, b);
    }
    /* signed 64 */
    n = (int)(sizeof sl / sizeof sl[0]);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        long long a = sl[i], b = sl[j];
        if (b == 0) continue;
        long long q = OTS$DIV_L(a, b), r = OTS$REM_L(a, b);
        if (a == LLONG_MIN && b == -1) {
            CK(q == LLONG_MIN, "DIV_L overflow -> %lld (want wrap LLONG_MIN)", q);
            continue;
        }
        CK(q == a / b, "DIV_L %lld/%lld = %lld want %lld", a, b, q, a / b);
        CK(r == a % b, "REM_L %lld%%%lld = %lld want %lld", a, b, r, a % b);
        CK(q * b + r == a, "identity_L %lld/%lld q=%lld r=%lld", a, b, q, r);
    }
    /* unsigned 64 */
    n = (int)(sizeof ul / sizeof ul[0]);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        unsigned long long a = ul[i], b = ul[j];
        if (b == 0) continue;
        unsigned long long q = OTS$DIV_UL(a, b), r = OTS$REM_UL(a, b);
        CK(q == a / b, "DIV_UL %llu/%llu = %llu want %llu", a, b, q, a / b);
        CK(r == a % b, "REM_UL %llu%%%llu = %llu want %llu", a, b, r, a % b);
        CK(q * b + r == a, "identity_UL %llu/%llu", a, b, a, b);
    }

    printf("OTS$ divide correctness: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
