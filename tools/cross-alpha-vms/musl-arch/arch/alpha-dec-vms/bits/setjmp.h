/*
 * setjmp.h - Alpha jmp_buf.  OVMX alpha-dec-vms musl port (vms-960).
 *
 * 18 quadwords, matching src/setjmp/alpha-dec-vms/{setjmp,longjmp}.s:
 *   [0..6]   integer callee-saved s0-s6 ($9-$15)
 *   [7]      ra ($26)
 *   [8]      gp ($29)
 *   [9]      sp ($30)
 *   [10..17] float callee-saved $f2-$f9
 *
 * Elements MUST be 64-bit: setjmp.s stores quadwords (stq/stt). `long` is only
 * 32-bit on alpha-dec-vms (LLP64), so use `long long` here or the buffer is half
 * the size the asm writes.
 */
typedef unsigned long long __jmp_buf[18];
