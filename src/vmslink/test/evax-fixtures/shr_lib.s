	## shr_lib.s — EVAX (Alpha/VMS) SHAREABLE producer fixture (bead vms-c65).
	## Exports a PROCEDURE FOO and a DATA quadword BAR. BAR is a pointer to FOO
	## (a .quad FOO), so it carries a REFQUAD data reloc to a placed symbol —
	## exercising the shareable's .vms$rel load-bias fixup table on the producer
	## side. FOO returns 7. Assembled to a real EVAX object by the alpha-dec-vms
	## binutils; the .obj is checked in (CI has no Alpha toolchain — LINK.EXE's
	## EVAX path is pure byte manipulation). Regenerate:
	##   alpha-dec-vms-as -o shr_lib.obj shr_lib.s
	.text
	.align 4
	.globl FOO
	.ent FOO
FOO..en:
	.frame $sp, 0, $26, 0
	ldgp $gp, 0($27)
	.prologue
	bis $31, 7, $0
	ret $31, ($26), 1
	.link
	.align 3
FOO:
	.pdesc FOO..en, stack
	.end FOO

	.data
	.align 3
	.globl BAR
BAR:
	.quad FOO
