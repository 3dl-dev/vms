/* ots_runtime.c — OVMX LIBOTS$ runtime for alpha-dec-vms (bead vms-bfd6).
 *
 * The OpenVMS Alpha GCC port lowers the operations Alpha hardware cannot do to
 * LIBOTS$ (the VMS language-independent support runtime): integer divide and
 * remainder (Alpha has NO integer-divide instruction) and a couple of block
 * data primitives. This file DEFINES those routines so the whole-archived musl
 * DECC$SHR (which references them through the compiler's codegen) links with no
 * deferred OTS$ externals.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8). The OTS$ *interface* — the routine names and
 * their register/standard-call convention — is public (OpenVMS RTL / LIBOTS$
 * manuals) and was additionally confirmed against the alpha-dec-vms cc1's own
 * emitted call sites (dividend in R16, divisor in R17, result in R0; OTS$MOVE
 * as (dst,len,src); OTS$ZERO as (dst,len)). The *implementations* below are
 * ours: textbook shift-subtract long division and plain byte-block move/zero.
 * NO VSI/HPE source or binary was read.
 *
 * SELF-CONTAINED / NO SELF-RECURSION. The division routines must not use C's
 * `/` or `%` on the target — the compiler would lower those right back to
 * OTS$DIV_x / OTS$REM_x and the routine would call itself. Everything here is
 * expressed with shifts, subtracts, and comparisons only. Likewise the block
 * primitives are explicit byte loops; the build compiles this file with
 * -fno-builtin -fno-tree-loop-distribute-patterns so GCC does not re-synthesise
 * a memcpy/memset/OTS$MOVE call out of the loops.
 *
 * WIDTHS on alpha-dec-vms (LLP64): int/long = 32-bit (longword, the "_I"/"_UI"
 * family), long long = 64-bit (quadword, the "_L"/"_UL" family).
 */

typedef unsigned long long u64;
typedef long long          s64;
typedef unsigned int       u32;
typedef int                s32;

/* Core unsigned 64-bit divide: returns quotient, writes remainder via *rem.
 * Restoring (shift-subtract) long division, MSB first — O(64), branch-per-bit.
 *
 * Divisor 0: real LIBOTS$ raises SS$_INTDIV via the VMS condition system, which
 * needs the executive (LIB$SIGNAL) not present in this link-oracle rung. Rather
 * than fake a value silently, this returns quotient 0 / remainder = numerator
 * for a zero divisor; faithful SS$_INTDIV signalling is a labelled follow-up
 * (see build-libots.sh header). No well-formed caller divides by zero, and the
 * correctness test below never asks it to (it tests zero-NUMERATOR, 0/b). */
static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    if (d == 0) { if (rem) *rem = n; return 0; }
    if (n < d)  { if (rem) *rem = n; return 0; }   /* fast path incl. n==0 */
    u64 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (u64)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}

/* Signed 64-bit divide/remainder built on the unsigned core. Magnitudes are
 * taken with -(u64)x, which is well defined for INT64_MIN (yields 2^63). The
 * quotient sign is the XOR of the operand signs; the remainder takes the sign
 * of the dividend (C99 truncation-toward-zero semantics, which is what the port
 * expects). INT64_MIN / -1 overflows in the same way the hardware path would;
 * it is left as the two's-complement wrap (no trap), matching C's UB latitude. */
static s64 sdivmod64(s64 a, s64 b, s64 *rem)
{
    u64 ua = (a < 0) ? -(u64)a : (u64)a;
    u64 ub = (b < 0) ? -(u64)b : (u64)b;
    u64 ur;
    u64 uq = udivmod64(ua, ub, &ur);
    s64 q = ((a < 0) ^ (b < 0)) ? -(s64)uq : (s64)uq;
    if (rem) *rem = (a < 0) ? -(s64)ur : (s64)ur;
    return q;
}

/* ---- 64-bit (quadword) family: long long ---- */
s64 OTS$DIV_L (s64 a, s64 b) { s64 r; return sdivmod64(a, b, &r); }
s64 OTS$REM_L (s64 a, s64 b) { s64 r; sdivmod64(a, b, &r); return r; }
u64 OTS$DIV_UL(u64 a, u64 b) { u64 r; return udivmod64(a, b, &r); }
u64 OTS$REM_UL(u64 a, u64 b) { u64 r; udivmod64(a, b, &r); return r; }

/* ---- 32-bit (longword) family: int. Widen to 64, divide, truncate back. The
 * results are identical to a native 32-bit divide because the operands fit. ---- */
s32 OTS$DIV_I (s32 a, s32 b) { s64 r; return (s32)sdivmod64((s64)a, (s64)b, &r); }
s32 OTS$REM_I (s32 a, s32 b) { s64 r; sdivmod64((s64)a, (s64)b, &r); return (s32)r; }
u32 OTS$DIV_UI(u32 a, u32 b) { u64 r; return (u32)udivmod64((u64)a, (u64)b, &r); }
u32 OTS$REM_UI(u32 a, u32 b) { u64 r; udivmod64((u64)a, (u64)b, &r); return (u32)r; }

/* ---- block primitives (OTS$MOVE / OTS$ZERO) live in ots_block.c ----
 * They are VOID (no R0 result), so — unlike the DIV/REM routines above whose
 * result IS R0 — they must ALSO preserve R0 under the OTS$ contract: the port
 * compiler parks a live value (musl __init_libc's `envp`) in R0 across an
 * implicit OTS$ZERO call. ots_block.c is compiled with R0 additionally
 * call-saved (-fcall-saved-0), which GCC docs warn is "disaster" on a
 * return-value register — so it must NOT be applied to this file's R0-returning
 * DIV/REM routines. That register-conflict is exactly why the block routines
 * are a separate translation unit (bead vms-0e4d, gap-10). */
