	## link_helper.s — EVAX (Alpha/VMS) object defining HELPER_PROC, the linkage
	## target for link_main.obj. Regenerate:
	##   alpha-dec-vms-as -o link_helper.obj link_helper.s
	.text
	.align 4
	.globl HELPER_PROC
	.ent HELPER_PROC
HELPER_PROC..en:
	.frame $sp, 0, $26, 0
	ldgp $gp, 0($27)
	.prologue
	bis $31, 42, $0
	ret $31, ($26), 1
	.link
	.align 3
HELPER_PROC:
	.pdesc HELPER_PROC..en, stack
	.end HELPER_PROC
