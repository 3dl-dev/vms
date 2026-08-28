/* ots_block.c — OVMX LIBOTS$ block primitives for alpha-dec-vms (bead vms-0e4d,
 * gap-10). Split out of ots_runtime.c so these two routines can be compiled with
 * an EXTRA register held under the OTS$ preservation contract (R0/v0) that the
 * arithmetic routines in ots_runtime.c must NOT hold — see below and
 * build-libots.sh.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): identical provenance to ots_runtime.c — the
 * OTS$MOVE/OTS$ZERO interface (arg order, register/standard-call convention) is
 * public OpenVMS RTL/LIBOTS$ documentation, additionally confirmed against the
 * alpha-dec-vms cc1's own emitted call sites (OTS$MOVE as (dst,len,src),
 * OTS$ZERO as (dst,len)); the implementations are ours (plain byte loops). NO
 * VSI/HPE source or binary was read.
 *
 * WHY R0 MATTERS HERE (the gap-10 register, vms-0e4d).
 *
 *   OTS$MOVE and OTS$ZERO are VOID — they produce NO result in R0. Unlike the
 *   integer DIV/REM routines (whose result IS R0), they fall under the OTS$
 *   contract's general clause: the language-support routines preserve EVERY
 *   register except the *result* register and reserved scratch. With no result
 *   register, R0 too must come back intact.
 *
 *   gap-9 (vms-334) marked the caller-saved TEMP set (R1-R8, R16-R25) call-SAVED
 *   for the whole OTS$ runtime, but deliberately EXCLUDED R0 as "the DIV/REM
 *   result". That is correct for DIV/REM — but the alpha-dec-vms port also parks
 *   a LIVE value in R0 across an OTS$ZERO/OTS$MOVE implicit call. The observed
 *   fault: musl __init_libc holds `envp` in R0 across the OTS$ZERO that zeroes
 *   its `size_t aux[AUX_CNT]={0}` (a 304-byte block-zero the port lowers to
 *   OTS$ZERO(&aux,304)); gap-9's OTS$ZERO used R0 as loop scratch and returned
 *   1, so `envp` came back as 1 and `auxv = envp+i+1` computed a bogus pointer
 *   (0x41) -> SIGSEGV in the auxv scan (DECC$SHR __init_libc). Same fault CLASS
 *   as gap-9 (a live temp parked across an implicit OTS$ call), one register
 *   further (R0, which gap-9 left out).
 *
 *   FIX: build THIS file with R0 additionally call-SAVED (build-libots.sh adds
 *   -fcall-saved-0 for ots_block.c only). GCC then prologue-saves / epilogue-
 *   restores R0 if it uses it, so a caller's live R0 survives. -fcall-saved-0 is
 *   applied ONLY here — GCC docs warn it is "disaster" on a function's
 *   return-value register, which is exactly why the R0-returning DIV/REM
 *   routines stay in ots_runtime.c WITHOUT it.
 *
 * SELF-CONTAINED. Explicit byte loops; the build compiles with -fno-builtin
 * -fno-tree-loop-distribute-patterns so GCC does not re-synthesise a
 * memcpy/memset/OTS$MOVE call out of the loops (which would self-reference).
 */

typedef unsigned long long u64;

/* ---- block primitives ----
 * OTS$MOVE(dst, len, src): move `len` bytes from src to dst. Confirmed arg order
 * from cc1 codegen (R16=dst, R17=len, R18=src). Overlap-correct (memmove
 * semantics): copies backward when the regions overlap forward, so it is never
 * wrong even where a caller relies on move (not copy) semantics.
 * OTS$ZERO(dst, len): zero `len` bytes at dst (R16=dst, R17=len). */
void OTS$MOVE(void *dst, u64 len, const void *src)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || len == 0) return;
    if (d < s) { for (u64 i = 0; i < len; i++) d[i] = s[i]; }
    else       { for (u64 i = len; i-- > 0; )  d[i] = s[i]; }
}

void OTS$ZERO(void *dst, u64 len)
{
    unsigned char *d = (unsigned char *)dst;
    for (u64 i = 0; i < len; i++) d[i] = 0;
}
