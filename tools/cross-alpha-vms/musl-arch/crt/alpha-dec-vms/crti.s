/*
 * crti.s - Alpha .init/.fini prologue.  OVMX alpha-dec-vms musl port (vms-960).
 * Paired with crtn.s. Establishes gp and saves ra; only used when a program is
 * linked against this musl (not exercised at rung 1 - the GCC port uses its own
 * crt0). Provided so the static-link crt set is complete.
 */
	.set noreorder
	.section .init
	.globl _init
	.align 3
_init:
	ldgp $29,0($27)
	subq $30,16,$30
	stq $26,0($30)

	.section .fini
	.globl _fini
	.align 3
_fini:
	ldgp $29,0($27)
	subq $30,16,$30
	stq $26,0($30)
