	.text
	.align 4
	.globl EXAMPLE_PROC
	.ent EXAMPLE_PROC
EXAMPLE_PROC..en:
	.frame $sp, 16, $26, 0
	ldgp $gp, 0($27)
	.prologue
	lda $sp, -16($sp)
	stq $26, 0($sp)
	.linkage EXT_ROUTINE
	ldq $27, EXT_ROUTINE($gp)
	jsr $26, ($27), 0
	ldq $26, 0($sp)
	lda $sp, 16($sp)
	ret $31, ($26), 1
	.link
	.align 3
EXAMPLE_PROC:
	.pdesc EXAMPLE_PROC..en, stack
	.end EXAMPLE_PROC
