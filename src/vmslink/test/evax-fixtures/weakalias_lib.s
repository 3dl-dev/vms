	.set noreorder
	.set volatile
	.text
	.align 2
	.align 4
	.ent dummy
dummy..en:
	.base $27
	.frame $29,0,$26,8
$LVFB0:
	.prologue
$LVEB0:
$LVM1:
$LVM2:
	ret $31,($26),1
$LVFE0:
$LVM3:
	.link
	.align 3
dummy:
	.pdesc dummy..en,null
	.end dummy
	.weak	__vm_wait
	__vm_wait..en = dummy..en
	__vm_wait = dummy
	.text
	.align 2
	.align 4
	.globl __impl
	.ent __impl
__impl..en:
	.base $27
	.frame $29,48,$26,8
	.mask 0x2000000c,0
$LVFB1:
	lda $30,-48($30)
	cpys $f31,$f31,$f31
	stq $29,32($30)
	mov $30,$29
	stq $27,0($30)
	stq $26,8($30)
	stq $2,16($30)
	stq $3,24($30)
	.prologue
$LVM4:
	mov $31,$25
$LVM5:
	mov $16,$2
	mov $17,$3
$LVM6:
	ldq $26,$1..__vm_wait..lk
	ldq $27,$1..__vm_wait..lk+8
	jsr $26,__vm_wait
	ldq $27,0($29)
$LVM7:
	addl $2,$2,$0
$LVEB1:
$LVM8:
$LVM9:
	mov $29,$30
$LVM10:
	addl $0,$2,$0
$LVM11:
	addl $0,$3,$0
$LVM12:
	ldq $26,8($30)
	ldq $2,16($30)
	ldq $3,24($30)
	ldq $29,32($30)
	lda $30,48($30)
	ret $31,($26),1
$LVFE1:
$LVM13:
	.link
	.align 3
__impl:
	.pdesc __impl..en,stack
$1..__vm_wait..lk:
	.quad __vm_wait..en
	.quad __vm_wait
	.end __impl
	.weak	decc$_mmap64
	decc$_mmap64..en = __impl..en
	decc$_mmap64 = __impl
	.text
$Lvetext0:

.section	.vmsdebug
	.align 0
	.word	0x2a
	.word	0xbc
	.byte	0x2
	.byte	0
	.long	0x7
	.word	0x1
	.word	0xd
	.byte	0xd
	.ascii "WEAKALIAS_LIB"
	.byte	0xe
	.ascii "GNU C17 14.2.0"
	.word	0x12
	.word	0xbe
	.byte	0x80
	.long	dummy..en
	.long	dummy
	.byte	0x5
	.ascii "dummy"
	.word	0x8
	.word	0xbf
	.byte	0
	.long	$LVFE0-$LVFB0
	.word	0x13
	.word	0xbe
	.byte	0x80
	.long	__impl..en
	.long	__impl
	.byte	0x6
	.ascii "__impl"
	.word	0x8
	.word	0xbf
	.byte	0
	.long	$LVFE1-$LVFB1
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
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM4-$LVM3
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM5-$LVM4
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM6-$LVM5
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM7-$LVM6
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM8-$LVM7
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM9-$LVM8
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM10-$LVM9
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM11-$LVM10
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM12-$LVM11
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6
	.byte	0x11
	.long	$LVM13-$LVM12
	.word	0x3
	.word	0xbd
