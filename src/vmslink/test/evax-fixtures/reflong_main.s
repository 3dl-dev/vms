	.set noreorder
	.set volatile
	.text
	.align 2
	.globl MAIN_PROC
	.ent MAIN_PROC
MAIN_PROC..en:
	.base $27
	.frame $29,32,$26,8
	.mask 0x20000000,0
$LVFB0:
	lda $30,-32($30)
	stq $27,0($30)
	stq $26,8($30)
	stq $29,16($30)
	mov $30,$29
	.prologue
$LVM1:
	lda $1,helper_ptr
	ldl $1,0($1)
	mov $31,$25
	ldq $26,8($1)
	mov $1,$27
	jsr $26,0
	ldq $27,0($29)
$LVEB0:
$LVM2:
$LVM3:
	mov $29,$30
	ldq $26,8($30)
	ldq $29,16($30)
	lda $30,32($30)
	ret $31,($26),1
$LVFE0:
$LVM4:
	.link
	.align 3
MAIN_PROC:
	.pdesc MAIN_PROC..en,stack
	.end MAIN_PROC
	.globl helper_ptr
	.data
	.align 2
helper_ptr:
	.long	HELPER_PROC
	.text
$Lvetext0:

.section	.vmsdebug
	.align 0
	.word	0x29
	.word	0xbc
	.byte	0x2
	.byte	0
	.long	0x7
	.word	0x1
	.word	0xd
	.byte	0xc
	.ascii "REFLONG_MAIN"
	.byte	0xe
	.ascii "GNU C17 14.2.0"
	.word	0x16
	.word	0xbe
	.byte	0x80
	.long	MAIN_PROC..en
	.long	MAIN_PROC
	.byte	0x9
	.ascii "MAIN_PROC"
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
	.word	0x3
	.word	0xbd
