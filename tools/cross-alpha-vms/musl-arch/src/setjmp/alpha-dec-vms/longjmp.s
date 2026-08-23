/*
 * longjmp.s - Alpha longjmp.  OVMX alpha-dec-vms musl port (vms-960).
 * CORRECTNESS-CRITICAL - real register restore.
 *
 * Restores the state saved by setjmp.s and returns into setjmp's caller. Per
 * C semantics longjmp(env, 0) makes setjmp appear to return 1; any other value
 * is returned as-is. jmp_buf ptr in $16 (a0), value in $17 (a1).
 * VMS/EVAX procedure model - see setjmp.s.
 *
 * DECORATED NAME (vms-838a): the alpha-dec-vms cc1 decorates a call to the
 * recognized C-RTL name `longjmp` as `decc$longjmp` at the CALL site (verified
 * from cc1 -S: `jsr $26,decc$longjmp` + `.linkage decc$longjmp`). This file is
 * hand-written EVAX asm, so it is NOT run through cc1's name decoration and its
 * `.globl longjmp` stays the raw name — the mismatch is why `decc$longjmp` was
 * left deferred in the DECC$SHR whole-archive link. `decc$longjmp` is made the
 * PRIMARY .ent procedure (so it carries EGSY__V_NORM and mk_decc_shr enumerates
 * it as a PROCEDURE universal, exactly like a cc1-emitted decc$* proc — a plain
 * alias label would enumerate as DATA); the raw `longjmp`/`_longjmp` remain as
 * descriptor aliases for a completeness/direct call.
 * (setjmp needs no decorated alias: cc1 leaves `setjmp` UNDECORATED — verified
 * the same way — so the raw `setjmp` def below already matches its call sites.)
 */
	.set noreorder
	.set volatile
	.text
	.align 4
	.globl longjmp
	.globl _longjmp
	.globl decc$longjmp
	.ent decc$longjmp
decc$longjmp..en:
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
decc$longjmp:
_longjmp:
longjmp:
	.pdesc decc$longjmp..en,null
	.end decc$longjmp
