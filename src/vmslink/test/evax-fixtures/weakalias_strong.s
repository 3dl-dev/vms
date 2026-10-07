	.set noreorder
	.set volatile
	.text
	.align 2
	.align 4
	.globl __vm_wait
	.ent __vm_wait
__vm_wait..en:
	.base $27
	.frame $29,0,$26,8
$LVFB0:
	.prologue
	lda $22,vm_lock
	.align 4
$L2:
$LVM1:
	ldl $1,0($22)
	addl $31,$1,$1
	bne $1,$L2
$LVEB0:
$LVM2:
$LVM3:
	ret $31,($26),1
$LVFE0:
$LVM4:
	.link
	.align 3
__vm_wait:
	.pdesc __vm_wait..en,null
	.end __vm_wait
	.globl vm_lock
	.data
	.align 2
vm_lock:
	.space 4
	.text
$Lvetext0:

.section	.vmsdebug
	.align 0
	.word	0x2d
	.word	0xbc
	.byte	0x2
	.byte	0
	.long	0x7
	.word	0x1
	.word	0xd
	.byte	0x10
	.ascii "WEAKALIAS_STRONG"
	.byte	0xe
	.ascii "GNU C17 14.2.0"
	.word	0x16
	.word	0xbe
	.byte	0x80
	.long	__vm_wait..en
	.long	__vm_wait
	.byte	0x9
	.ascii "__vm_wait"
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
	.long	0x2
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x2
	.byte	0x11
	.long	$LVM1-	.text
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x2
	.byte	0x11
	.long	$LVM2-$LVM1
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x2
	.byte	0x11
	.long	$LVM3-$LVM2
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x2
	.byte	0x11
	.long	$LVM4-$LVM3
	.word	0x3
	.word	0xbd
