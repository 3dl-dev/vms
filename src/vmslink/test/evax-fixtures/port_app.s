	.set noreorder
	.set volatile
	.text
	.rdata
$LC0:
	.ascii "hello\0"
$LC1:
	.ascii "%s world argc=%d len=%lu\12\0"
	.text
	.align 2
	.globl main
	.globl __gcc_main_flags
__gcc_main_flags = 3
	.ent main
main..en:
	.base $27
	.frame $29,80,$26,8
	.mask 0x20000000,0
$LVFB0:
	lda $30,-80($30)
	stq $27,0($30)
	stq $26,8($30)
	stq $29,16($30)
	mov $30,$29
	.prologue
	mov $16,$1
	stq $17,56($29)
	stq $18,64($29)
	stl $1,48($29)
$LVM1:
	lda $16,32($31)
	lda $25,1($31)
	ldq $26,$0..decc$_malloc64..lk
	ldq $27,$0..decc$_malloc64..lk+8
	jsr $26,decc$_malloc64
	ldq $27,0($29)
	mov $0,$1
	stq $1,32($29)
$LVM2:
	ldq $1,32($29)
	lda $22,$LC0
	ldq_u $25,0($22)
	ldq_u $24,3($22)
	mov $22,$23
	extll $25,$23,$25
	extlh $24,$23,$23
	bis $31,$25,$24
	bis $24,$23,$23
	bis $31,$23,$0
	ldq_u $23,4($22)
	ldq_u $24,5($22)
	lda $22,4($22)
	extwl $23,$22,$23
	extwh $24,$22,$22
	bis $23,$22,$22
	bis $31,$22,$25
	ldq_u $23,3($1)
	ldq_u $22,0($1)
	mov $1,$24
	inslh $0,$24,$21
	insll $0,$24,$0
	msklh $23,$24,$23
	mskll $22,$24,$22
	bis $23,$21,$23
	bis $22,$0,$22
	stq_u $23,3($1)
	stq_u $22,0($1)
	ldq_u $23,5($1)
	ldq_u $22,4($1)
	lda $24,4($1)
	inswh $25,$24,$0
	inswl $25,$24,$25
	mskwh $23,$24,$23
	mskwl $22,$24,$22
	bis $23,$0,$23
	bis $22,$25,$22
	stq_u $23,5($1)
	stq_u $22,4($1)
$LVM3:
	ldq $16,32($29)
	lda $25,1($31)
	ldq $26,$0..decc$strlen..lk
	ldq $27,$0..decc$strlen..lk+8
	jsr $26,decc$strlen
	ldq $27,0($29)
	mov $0,$1
	mov $1,$22
	ldl $1,48($29)
	mov $22,$19
	mov $1,$18
	ldq $17,32($29)
	lda $16,$LC1
	lda $25,4($31)
	ldq $26,$0..decc$tprintf..lk
	ldq $27,$0..decc$tprintf..lk+8
	jsr $26,decc$tprintf
	ldq $27,0($29)
$LVM4:
	mov $31,$1
$LVM5:
	mov $1,$0
$LVEB0:
$LVM6:
	mov $29,$30
	ldq $26,8($30)
	ldq $29,16($30)
	lda $30,80($30)
	ret $31,($26),1
$LVFE0:
$LVM7:
	.link
	.align 3
main:
	.pdesc main..en,stack
$0..decc$tprintf..lk:
	.linkage decc$tprintf
$0..decc$_malloc64..lk:
	.linkage decc$_malloc64
$0..decc$strlen..lk:
	.linkage decc$strlen
	.end main
	.text
$Lvetext0:

.section	.vmsdebug
	.align 0
	.word	0x20
	.word	0xbc
	.byte	0x2
	.byte	0
	.long	0x7
	.word	0x1
	.word	0xd
	.byte	0x3
	.ascii "APP"
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
	.long	0x6
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM1-	.text
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM2-$LVM1
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM3-$LVM2
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM4-$LVM3
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM5-$LVM4
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0xa
	.byte	0x11
	.long	$LVM6-$LVM5
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0xa
	.byte	0x11
	.long	$LVM7-$LVM6
	.word	0x3
	.word	0xbd
