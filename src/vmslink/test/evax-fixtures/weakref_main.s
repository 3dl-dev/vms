	.set noreorder
	.set volatile
	.text
	.align 2
	.align 4
	.globl MAIN_PROC
	.ent MAIN_PROC
MAIN_PROC..en:
	.base $27
	.frame $29,32,$26,8
	.mask 0x20000004,0
$LVFB0:
	lda $30,-32($30)
	cpys $f31,$f31,$f31
	stq $29,24($30)
	mov $30,$29
	stq $27,0($30)
	stq $26,8($30)
	stq $2,16($30)
	.prologue
$LVM1:
	bis $31,$31,$31
	mov $31,$2
$LVM2:
	lda $1,WEAK_CALL
	zapnot $1,15,$1
	beq $1,$L2
$LVM3:
	lda $16,1($31)
	lda $25,1($31)
	ldq $26,$0..WEAK_CALL..lk
	ldq $27,$0..WEAK_CALL..lk+8
	jsr $26,WEAK_CALL
	ldq $27,0($29)
	mov $0,$2
$L2:
$LVM4:
	lda $1,WEAK_DATA
	zapnot $1,15,$1
	beq $1,$L3
$LVM5:
	lda $1,WEAK_DATA
	ldl $1,0($1)
	addl $1,$2,$2
$L3:
$LVM6:
	mov $31,$25
	ldq $26,$0..HELPER_PROC..lk
	ldq $27,$0..HELPER_PROC..lk+8
	jsr $26,HELPER_PROC
	ldq $27,0($29)
$LVEB0:
$LVM7:
$LVM8:
	mov $29,$30
$LVM9:
	cpys $f31,$f31,$f31
	addl $0,$2,$0
$LVM10:
	ldq $26,8($30)
	ldq $2,16($30)
	ldq $29,24($30)
	lda $30,32($30)
	ret $31,($26),1
$LVFE0:
$LVM11:
	.link
	.align 3
MAIN_PROC:
	.pdesc MAIN_PROC..en,stack
$0..WEAK_CALL..lk:
	.linkage WEAK_CALL
$0..HELPER_PROC..lk:
	.linkage HELPER_PROC
	.end MAIN_PROC
	.weak	HELPER_PROC
	.weak	WEAK_DATA
	.weak	WEAK_CALL
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
	.ascii "WEAKREF_MAIN"
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
	.long	0x10
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x10
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
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM6-$LVM5
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x15
	.byte	0x11
	.long	$LVM7-$LVM6
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM8-$LVM7
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x15
	.byte	0x11
	.long	$LVM9-$LVM8
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM10-$LVM9
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x16
	.byte	0x11
	.long	$LVM11-$LVM10
	.word	0x3
	.word	0xbd
