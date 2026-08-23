/*
 * setjmp.s - Alpha setjmp.  OVMX alpha-dec-vms musl port (vms-960).
 * CORRECTNESS-CRITICAL - real register save.
 *
 * Saves the Alpha callee-saved state the standard (OSF/1 & OpenVMS Alpha)
 * calling conventions require a procedure to preserve:
 *   integer  s0-s6 ($9-$15), ra ($26), gp ($29), sp ($30)
 *   float    $f2-$f9  (stored as T-floating / IEEE double)
 * Layout matches bits/setjmp.h (__jmp_buf[18]). jmp_buf ptr arrives in $16 (a0).
 */
	.set noat
	.set noreorder
	.text
	.align 4
	.globl setjmp
	.globl _setjmp
	.globl __setjmp
	.ent __setjmp
__setjmp:
setjmp:
_setjmp:
	.frame $30,0,$26,0
	.prologue 0
	stq $9,    0($16)
	stq $10,   8($16)
	stq $11,  16($16)
	stq $12,  24($16)
	stq $13,  32($16)
	stq $14,  40($16)
	stq $15,  48($16)
	stq $26,  56($16)
	stq $29,  64($16)
	stq $30,  72($16)
	stt $f2,  80($16)
	stt $f3,  88($16)
	stt $f4,  96($16)
	stt $f5, 104($16)
	stt $f6, 112($16)
	stt $f7, 120($16)
	stt $f8, 128($16)
	stt $f9, 136($16)
	mov 0, $0
	ret $31,($26),1
	.end __setjmp
