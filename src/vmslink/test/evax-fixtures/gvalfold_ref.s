	.globl MAIN
	.text
MAIN:
	ret $31,($26),1
	.data
	.globl CEXITSLOT
CEXITSLOT:
	.quad C$_EXIT1
