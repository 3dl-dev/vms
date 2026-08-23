/*
 * setjmp.h - Alpha jmp_buf.  OVMX alpha-dec-vms musl port (vms-960).
 *
 * 18 quadwords, matching src/setjmp/alpha-dec-vms/{setjmp,longjmp}.s:
 *   [0..6]   integer callee-saved s0-s6 ($9-$15)
 *   [7]      ra ($26)
 *   [8]      gp ($29)
 *   [9]      sp ($30)
 *   [10..17] float callee-saved $f2-$f9
 */
typedef unsigned long __jmp_buf[18];
