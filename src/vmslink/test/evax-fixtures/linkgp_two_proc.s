	## linkgp_two_proc.s — EVAX (Alpha/VMS) SHAREABLE fixture for the per-image
	## linkage-section-base + per-procedure K test (bead vms-fd5, component C1 of
	## vms-5f5). Exports TWO procedures, FIRST_PROC and SECOND_PROC, so their
	## PDSCs land at two DIFFERENT offsets within the module's single $LINK$
	## psect: FIRST_PROC's .pdesc is emitted first (K==0, at the linkage-section
	## base) and SECOND_PROC's second (K!=0, some non-zero offset). This is the
	## structural precondition the design doc (docs/design-alpha-per-image-gp.md
	## §1.4/§2.3) requires: "K = the offset of THIS procedure's PDSC from its
	## module's linkage-section base," which is only a genuine per-procedure
	## value when a module holds more than one PDSC. Regenerate:
	##   alpha-dec-vms-as -o linkgp_two_proc.obj linkgp_two_proc.s
	.text
	.align 4
	.globl FIRST_PROC
	.ent FIRST_PROC
FIRST_PROC..en:
	.frame $sp, 0, $26, 0
	ldgp $gp, 0($27)
	.prologue
	bis $31, 1, $0
	ret $31, ($26), 1
	.link
	.align 3
FIRST_PROC:
	.pdesc FIRST_PROC..en, stack
	.end FIRST_PROC

	.text
	.align 4
	.globl SECOND_PROC
	.ent SECOND_PROC
SECOND_PROC..en:
	.frame $sp, 0, $26, 0
	ldgp $gp, 0($27)
	.prologue
	bis $31, 2, $0
	ret $31, ($26), 1
	.link
	.align 3
SECOND_PROC:
	.pdesc SECOND_PROC..en, stack
	.end SECOND_PROC
