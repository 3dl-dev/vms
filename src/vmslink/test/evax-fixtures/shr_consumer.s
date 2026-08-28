	## shr_consumer.s — EVAX (Alpha/VMS) consumer fixture (bead vms-c65). CONSUMER_PROC
	## calls an EXTERNAL FOO through an Alpha .linkage pair (a genuine STC_LP_PSB
	## LINKAGE reloc). Linked ALONE — without shr_lib.obj — against the Alpha
	## shareable FOO$SHR.EXE via `--use`, LINK.EXE must bind FOO as a cross-image
	## import by symbol-vector index (the vms-c179 machinery), proving an Alpha
	## shareable produced by emit_evax_shareable is CONSUMABLE. Regenerate:
	##   alpha-dec-vms-as -o shr_consumer.obj shr_consumer.s
	.text
	.align 4
	.globl CONSUMER_PROC
	.ent CONSUMER_PROC
CONSUMER_PROC..en:
	.frame $sp, 16, $26, 0
	ldgp $gp, 0($27)
	.prologue
	lda $sp, -16($sp)
	stq $26, 0($sp)
	.linkage FOO
	ldq $27, FOO($gp)
	jsr $26, ($27), 0
	ldq $26, 0($sp)
	lda $sp, 16($sp)
	ret $31, ($26), 1
	.link
	.align 3
CONSUMER_PROC:
	.pdesc CONSUMER_PROC..en, stack
	.end CONSUMER_PROC
