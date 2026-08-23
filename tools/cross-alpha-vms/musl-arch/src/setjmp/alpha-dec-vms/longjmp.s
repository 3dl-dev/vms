/*
 * longjmp.s - Alpha longjmp.  OVMX alpha-dec-vms musl port (vms-960).
 * CORRECTNESS-CRITICAL - real register restore.
 *
 * Restores the state saved by setjmp.s and returns into setjmp's caller. Per
 * C semantics longjmp(env, 0) makes setjmp appear to return 1; any other value
 * is returned as-is. jmp_buf ptr in $16 (a0), value in $17 (a1).
 * VMS/EVAX procedure model - see setjmp.s.
 */
	.set noreorder
	.set volatile
	.text
	.align 4
	.globl longjmp
	.globl _longjmp
	.ent _longjmp
_longjmp..en:
	.base $27
	.frame $30,0,$26,0
	.prologue
	ldq $9,    0($16)
	ldq $10,   8($16)
	ldq $11,  16($16)
	ldq $12,  24($16)
	ldq $13,  32($16)
	ldq $14,  40($16)
	ldq $15,  48($16)
	ldq $26,  56($16)
	ldq $29,  64($16)
	ldq $30,  72($16)
	ldt $f2,  80($16)
	ldt $f3,  88($16)
	ldt $f4,  96($16)
	ldt $f5, 104($16)
	ldt $f6, 112($16)
	ldt $f7, 120($16)
	ldt $f8, 128($16)
	ldt $f9, 136($16)
	mov $17, $0
	bne $17, 1f
	mov 1, $0
1:
	ret $31,($26),1
	.link
	.align 3
_longjmp:
longjmp:
	.pdesc _longjmp..en,null
	.end _longjmp
