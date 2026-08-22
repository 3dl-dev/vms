	## link_main.s — EVAX (Alpha/VMS) object: MAIN_PROC calls an EXTERNAL
	## HELPER_PROC via an Alpha .linkage pair, producing a genuine STC_LP_PSB
	## (LINKAGE) relocation that LINK.EXE resolves against link_helper.obj. DPTR
	## is a data quadword pointing at the external HELPER_PROC (REFQUAD data reloc
	## against a symbol). Regenerate: alpha-dec-vms-as -o link_main.obj link_main.s
	.text
	.align 4
	.globl MAIN_PROC
	.ent MAIN_PROC
MAIN_PROC..en:
	.frame $sp, 16, $26, 0
	ldgp $gp, 0($27)
	.prologue
	lda $sp, -16($sp)
	stq $26, 0($sp)
	.linkage HELPER_PROC
	ldq $27, HELPER_PROC($gp)
	jsr $26, ($27), 0
	ldq $26, 0($sp)
	lda $sp, 16($sp)
	ret $31, ($26), 1
	.link
	.align 3
MAIN_PROC:
	.pdesc MAIN_PROC..en, stack
	.end MAIN_PROC

	.data
	.align 3
	.globl DPTR
DPTR:
	.quad HELPER_PROC
