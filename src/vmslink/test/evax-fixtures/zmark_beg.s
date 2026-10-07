	.set noreorder
	.set volatile
	.text
	.align 2
	.globl main
	.globl __gcc_main_flags
__gcc_main_flags = 3
	.ent main
main..en:
	.base $27
	.frame $29,0,$26,8
$LVFB0:
	.prologue
$LVM1:
	and $16,1,$16
$LVM2:
	lda $1,zmark_ptr
	ldq $1,0($1)
	addq $1,$16,$1
	lda $22,1($1)
	ldq_u $0,0($1)
	extqh $0,$22,$0
$LVM3:
	sra $0,56,$0
$LVEB0:
$LVM4:
	ret $31,($26),1
$LVFE0:
$LVM5:
	.link
	.align 3
main:
	.pdesc main..en,null
	.end main
	.globl zmark_ptr
	.data
	.align 3
zmark_ptr:
	.quad	zmark_begin

.section	zmark
	.align 3
zmark_begin:
	.text
$Lvetext0:

.section	.vmsdebug
	.align 0
	.word	0x26
	.word	0xbc
	.byte	0x2
	.byte	0
	.long	0x7
	.word	0x1
	.word	0xd
	.byte	0x9
	.ascii "ZMARK_BEG"
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
	.long	0x4
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x4
	.byte	0x11
	.long	$LVM1-	.text
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x4
	.byte	0x11
	.long	$LVM2-$LVM1
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x4
	.byte	0x11
	.long	$LVM3-$LVM2
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x4
	.byte	0x11
	.long	$LVM4-$LVM3
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x4
	.byte	0x11
	.long	$LVM5-$LVM4
	.word	0x3
	.word	0xbd
