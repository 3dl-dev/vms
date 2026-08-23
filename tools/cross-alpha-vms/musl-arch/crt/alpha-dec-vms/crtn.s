/*
 * crtn.s - Alpha .init/.fini epilogue.  OVMX alpha-dec-vms musl port (vms-960).
 * Paired with crti.s. Restores ra and returns.
 */
	.set noreorder
	.section .init
	ldq $26,0($30)
	addq $30,16,$30
	ret $31,($26),1

	.section .fini
	ldq $26,0($30)
	addq $30,16,$30
	ret $31,($26),1
