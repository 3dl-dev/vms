/*
 * softdiv.S - __divqu / __remqu (unsigned 64-bit integer divide/remainder)
 * for Alpha, freestanding (no glibc, see softmath.c's file header for why
 * these can't just be __builtin_* or plain C `/`  `%`  on Alpha).
 *
 * These are NOT ordinary C-ABI functions -- Alpha's compiler-generated
 * calls to its integer-division helpers use a special, non-standard
 * calling convention (confirmed by disassembling GCC's own output for
 * `a / b` on alpha-linux-gnu, and cross-checked against glibc's divqu.o):
 *
 *   inputs:         $24 (t10), $25 (t11)     -- NOT $16/$17 (a0/a1)
 *   return address: $23 (t9)                 -- NOT $26 (ra)
 *   result:         $27 (pv/t12)             -- NOT $0 (v0)
 *   preserved:      $1-$8 (t0-t7)            -- NOT caller-saved as usual
 *
 * That last point is the one that isn't obvious from a single call site
 * and cost real debugging time: GCC's div+mod fusion (`n % k` and `n / k`
 * against the same operands in one function, e.g. vms_snprintf.c's
 * uint_to_str) calls __divqu ONCE, keeps the ORIGINAL dividend alive
 * across the call in an ordinarily-caller-saved temp register (observed:
 * $1/t0), and derives the remainder itself afterwards via `mulq`+`subq` --
 * i.e. GCC's alpha backend treats __divqu/__remqu as preserving the usual
 * integer scratch registers, unlike a normal function call. glibc's own
 * divqu.o confirms this empirically: it explicitly saves/restores t0-t4
 * (and $f0-$f3) around its body. This file preserves $1-$8 (a superset)
 * to stay safely inside that contract regardless of which exact subset
 * GCC's machine description relies on.
 *
 * Plain binary long division (restoring division, MSB-first) -- exact for
 * all inputs, no floating point, no PALcode divide-emulation trap
 * dependency.
 */
	.set noat
	.text

/* --- __divqu(n, d) -> quotient --------------------------------------- */
	.globl __divqu
	.ent   __divqu
__divqu:
	.frame $30, 64, $23, 0
	.prologue 0
	lda	$30, -64($30)
	stq	$1, 0($30)
	stq	$2, 8($30)
	stq	$3, 16($30)
	stq	$4, 24($30)
	stq	$5, 32($30)
	stq	$6, 40($30)
	stq	$7, 48($30)
	stq	$8, 56($30)
	beq	$25, 9f			/* d == 0: bail out, quotient = 0 */
	mov	$31, $1			/* q = 0 */
	mov	$31, $2			/* r = 0 */
	lda	$3, 63			/* i = 63 */
1:	srl	$24, $3, $4		/* $4 = (n >> i) & 1 */
	and	$4, 1, $4
	sll	$2, 1, $2		/* r = (r << 1) | $4 */
	or	$2, $4, $2
	cmpule	$25, $2, $5		/* $5 = (d <= r) */
	beq	$5, 2f
	subq	$2, $25, $2		/* r -= d */
	lda	$6, 1
	sll	$6, $3, $6		/* $6 = 1 << i */
	or	$1, $6, $1		/* q |= $6 */
2:	subq	$3, 1, $3		/* i-- */
	bge	$3, 1b
	mov	$1, $27			/* result (quotient) -> pv */
	br	$31, 8f
9:	mov	$31, $27
8:	ldq	$1, 0($30)
	ldq	$2, 8($30)
	ldq	$3, 16($30)
	ldq	$4, 24($30)
	ldq	$5, 32($30)
	ldq	$6, 40($30)
	ldq	$7, 48($30)
	ldq	$8, 56($30)
	lda	$30, 64($30)
	ret	$31, ($23), 1
	.end   __divqu

/* --- __remqu(n, d) -> remainder --------------------------------------- */
	.globl __remqu
	.ent   __remqu
__remqu:
	.frame $30, 64, $23, 0
	.prologue 0
	lda	$30, -64($30)
	stq	$1, 0($30)
	stq	$2, 8($30)
	stq	$3, 16($30)
	stq	$4, 24($30)
	stq	$5, 32($30)
	stq	$6, 40($30)
	stq	$7, 48($30)
	stq	$8, 56($30)
	beq	$25, 9f			/* d == 0: bail out, remainder = n */
	mov	$31, $2			/* r = 0 */
	lda	$3, 63			/* i = 63 */
1:	srl	$24, $3, $4		/* $4 = (n >> i) & 1 */
	and	$4, 1, $4
	sll	$2, 1, $2		/* r = (r << 1) | $4 */
	or	$2, $4, $2
	cmpule	$25, $2, $5		/* $5 = (d <= r) */
	beq	$5, 2f
	subq	$2, $25, $2		/* r -= d */
2:	subq	$3, 1, $3		/* i-- */
	bge	$3, 1b
	mov	$2, $27			/* result (remainder) -> pv */
	br	$31, 8f
9:	mov	$24, $27
8:	ldq	$1, 0($30)
	ldq	$2, 8($30)
	ldq	$3, 16($30)
	ldq	$4, 24($30)
	ldq	$5, 32($30)
	ldq	$6, 40($30)
	ldq	$7, 48($30)
	ldq	$8, 56($30)
	lda	$30, 64($30)
	ret	$31, ($23), 1
	.end   __remqu

	.section .note.GNU-stack,"",@progbits
