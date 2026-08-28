	.set noreorder
	.set volatile
	.text
	.rdata
greeting:
	.ascii "hi from global data\0"
	.globl g_ptr
	.data
	.align 3
g_ptr:
	.quad	greeting
	.text
	.align 2
	.globl main
	.globl __gcc_main_flags
__gcc_main_flags = 3
	.ent main
main..en:
	.base $27
	.frame $29,64,$26,8
	.mask 0x20000000,0
$LVFB0:
	lda $30,-64($30)
	stq $27,0($30)
	stq $26,8($30)
	stq $29,16($30)
	mov $30,$29
	.prologue
	mov $16,$1
	stq $17,40($29)
	stq $18,48($29)
	stl $1,32($29)
$LVM1:
	lda $1,g_ptr
	ldq $22,0($1)
$LVM2:
	ldl $1,32($29)
	and $1,1,$1
$LVM3:
	addq $22,$1,$1
	lda $22,1($1)
	ldq_u $1,0($1)
	extqh $1,$22,$1
	sra $1,56,$1
$LVM4:
	mov $1,$0
$LVEB0:
$LVM5:
	mov $29,$30
	ldq $26,8($30)
	ldq $29,16($30)
	lda $30,64($30)
	ret $31,($26),1
$LVFE0:
$LVM6:
	.link
	.align 3
main:
	.pdesc main..en,stack
	.end main
	.text
$Lvetext0:

.section	.vmsdebug
	.align 0
	.word	0x22
	.word	0xbc
	.byte	0x2
	.byte	0
	.long	0x7
	.word	0x1
	.word	0xd
	.byte	0x5
	.ascii "GDATA"
	.byte	0xe
	.ascii "GNU C17 14.2.0"
	.word	0x1a
	.word	0x17
	.byte	0x1
	.long	main
	.byte	0x11
	.ascii "TRANSFER$BREAK$GO"
	.word	0x11
	.word	0xbe
	.byte	0x80
	.long	main..en
	.long	main
	.byte	0x4
	.ascii "main"
	.word	0x8
	.word	0xbf
	.byte	0
	.long	$LVFE0-$LVFB0
	.word	0x8
	.word	0xb9
	.byte	0x10
	.long		.text
	.word	0x8
	.word	0xb9
	.byte	0x14
	.long	0x3
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x3
	.byte	0x11
	.long	$LVM1-	.text
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x3
	.byte	0x11
	.long	$LVM2-$LVM1
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x3
	.byte	0x11
	.long	$LVM3-$LVM2
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x3
	.byte	0x11
	.long	$LVM4-$LVM3
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x3
	.byte	0x11
	.long	$LVM5-$LVM4
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x3
	.byte	0x11
	.long	$LVM6-$LVM5
	.word	0x3
	.word	0xbd
