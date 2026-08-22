	## absval_def.s — EVAX (Alpha/VMS) object DEFINING an ABSOLUTE global
	## symbol (a "globalvalue"): __gcc_main_flags names no storage — its VALUE
	## IS the constant 3. This is exactly what the OpenVMS GCC port emits for a
	## `main()` object (gcc/config/vms/vms.cc vms_start_function:
	##     .globl __gcc_main_flags ; __gcc_main_flags = <flags>), which the port
	## crt0 reads as `(unsigned __int64)&__gcc_main_flags` (the ADDRESS is the
	## value). alpha-dec-vms-as encodes it as an EGSD SYM with EGSY__V_DEF set
	## and EGSY__V_REL CLEAR (flags=0x0002), value=3, psindx -> the synthetic
	## $ABS$ psect (bfd/vms-alpha.c _bfd_vms_write_egsd: REL is set only when the
	## symbol is NOT in an absolute section). REFUSER + MAIN are normal
	## psect-relative globals in the same object (the no-regression controls).
	## Regenerate: alpha-dec-vms-as -o absval_def.obj absval_def.s   (bead vms-1bc)
	.globl __gcc_main_flags
	__gcc_main_flags = 3
	.data
	.globl REFUSER
REFUSER:
	.quad 0xdead
	.text
	.align 4
	.globl MAIN
MAIN:
	ret $31,($26),1
