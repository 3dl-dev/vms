	.set noreorder
	.set volatile
	.text
	.align 2
	.globl __main
	.ent __main
__main..en:
	.base $27
	.frame $29,128,$26,8
	.mask 0x20000000,0
$LVFB0:
	lda $30,-128($30)
	stq $27,0($30)
	stq $26,8($30)
	stq $29,16($30)
	mov $30,$29
	lda $30,-32($29)
	.prologue
	stq $16,80($29)
	stq $17,88($29)
	stq $18,96($29)
	stq $19,104($29)
	mov $20,$1
	mov $21,$22
	stl $1,112($29)
	bis $31,$22,$1
	stl $1,116($29)
$LVM1:
	lda $1,__gcc_main_flags
	stl $1,60($29)
$LVM2:
	ldl $23,116($29)
	ldl $22,112($29)
	lda $1,76($29)
	stq $1,16($30)
	lda $1,72($29)
	stq $1,8($30)
	lda $1,68($29)
	stq $1,0($30)
	mov $23,$21
	mov $22,$20
	ldq $19,104($29)
	ldq $18,96($29)
	ldq $17,88($29)
	ldq $16,80($29)
	lda $25,9($31)
	ldq $26,$0..decc$main..lk
	ldq $27,$0..decc$main..lk+8
	jsr $26,decc$main
	ldq $27,0($29)
$LVM3:
	ldl $1,60($29)
	and $1,1,$1
	addl $31,$1,$1
$LVM4:
	beq $1,$L2
$LVM5:
	ldl $1,68($29)
	addl $1,1,$1
	addl $31,$1,$1
$LVM6:
	s8addl $1,0,$1
	addl $31,$1,$1
$LVM7:
	mov $1,$16
	lda $25,1($31)
	ldq $26,$0..decc$malloc..lk
	ldq $27,$0..decc$malloc..lk+8
	jsr $26,decc$malloc
	ldq $27,0($29)
	mov $0,$1
$LVM8:
	stq $1,40($29)
$LVM9:
	stl $31,56($29)
$LVM10:
	br $31,$L3
$L4:
$LVM11:
	ldl $1,56($29)
	s4addq $1,0,$1
	ldl $22,72($29)
	addq $1,$22,$1
	ldl $1,0($1)
$LVM12:
	mov $1,$23
$LVM13:
	ldl $1,56($29)
	s8addq $1,0,$1
	ldq $22,40($29)
	addq $22,$1,$1
$LVM14:
	mov $23,$22
$LVM15:
	stq $22,0($1)
$LVM16:
	ldl $1,56($29)
	addl $1,1,$1
	stl $1,56($29)
$L3:
$LVM17:
	ldl $1,68($29)
	ldl $22,56($29)
	cmplt $22,$1,$1
	bne $1,$L4
$LVM18:
	ldl $1,68($29)
	s8addq $1,0,$1
	ldq $22,40($29)
	addq $22,$1,$1
$LVM19:
	stq $31,0($1)
$LVM20:
	stl $31,56($29)
$LVM21:
	br $31,$L5
$L6:
$LVM22:
	ldl $1,56($29)
	addl $1,1,$1
	stl $1,56($29)
$L5:
$LVM23:
	ldl $1,56($29)
	s4addq $1,0,$1
	ldl $22,76($29)
	addq $1,$22,$1
	ldl $1,0($1)
$LVM24:
	bne $1,$L6
$LVM25:
	ldl $1,56($29)
	addl $1,1,$1
	addl $31,$1,$1
$LVM26:
	s8addl $1,0,$1
	addl $31,$1,$1
$LVM27:
	mov $1,$16
	lda $25,1($31)
	ldq $26,$0..decc$malloc..lk
	ldq $27,$0..decc$malloc..lk+8
	jsr $26,decc$malloc
	ldq $27,0($29)
	mov $0,$1
$LVM28:
	stq $1,48($29)
$LVM29:
	stl $31,56($29)
$LVM30:
	br $31,$L7
$L8:
$LVM31:
	ldl $1,56($29)
	s4addq $1,0,$1
	ldl $22,76($29)
	addq $1,$22,$1
	ldl $1,0($1)
$LVM32:
	mov $1,$23
$LVM33:
	ldl $1,56($29)
	s8addq $1,0,$1
	ldq $22,48($29)
	addq $22,$1,$1
$LVM34:
	mov $23,$22
$LVM35:
	stq $22,0($1)
$LVM36:
	ldl $1,56($29)
	addl $1,1,$1
	stl $1,56($29)
$L7:
$LVM37:
	ldl $1,56($29)
	s4addq $1,0,$1
	ldl $22,76($29)
	addq $1,$22,$1
	ldl $1,0($1)
$LVM38:
	bne $1,$L8
$LVM39:
	ldl $1,56($29)
	s8addq $1,0,$1
	ldq $22,48($29)
	addq $22,$1,$1
$LVM40:
	stq $31,0($1)
	br $31,$L9
$L2:
$LVM41:
	ldl $1,72($29)
$LVM42:
	stq $1,40($29)
$LVM43:
	ldl $1,76($29)
$LVM44:
	stq $1,48($29)
$L9:
$LVM45:
	ldl $1,68($29)
	ldq $18,48($29)
	ldq $17,40($29)
	mov $1,$16
	lda $25,3($31)
	ldq $26,$0..main..lk
	ldq $27,$0..main..lk+8
	jsr $26,main
	ldq $27,0($29)
	mov $0,$1
	stl $1,32($29)
$LVM46:
	ldl $1,60($29)
	and $1,2,$1
	addl $31,$1,$1
$LVM47:
	beq $1,$L10
$LVM48:
	ldl $1,32($29)
	and $1,255,$1
	stl $1,32($29)
$LVM49:
	ldl $1,32($29)
	beq $1,$L11
$LVM50:
	ldl $1,32($29)
	stl $1,64($29)
$LVM51:
	ldl $1,32($29)
	subl $1,1,$1
	addl $31,$1,$1
$LVM52:
	s8addl $1,0,$1
	addl $31,$1,$22
$LVM53:
	lda $1,C$_EXIT1
	addl $31,$1,$1
	addl $22,$1,$1
	addl $31,$1,$1
$LVM54:
	stl $1,32($29)
$LVM55:
	ldl $1,64($29)
	cmpeq $1,1,$1
	beq $1,$L10
$LVM56:
	ldl $1,32($29)
	addl $1,1,$1
	stl $1,32($29)
$LVM57:
	ldl $1,32($29)
	ldah $22,4096($31)
	bis $1,$22,$1
	stl $1,32($29)
	br $31,$L10
$L11:
$LVM58:
	lda $1,1($31)
	stl $1,32($29)
$L10:
$LVM59:
	ldl $1,32($29)
$LVM60:
	mov $1,$0
$LVEB0:
$LVM61:
	mov $29,$30
	ldq $26,8($30)
	ldq $29,16($30)
	lda $30,128($30)
	ret $31,($26),1
$LVFE0:
$LVM62:
	.link
	.align 3
__main:
	.pdesc __main..en,stack
$0..decc$malloc..lk:
	.linkage decc$malloc
$0..decc$main..lk:
	.linkage decc$main
$0..main..lk:
	.linkage main
	.end __main
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
	.ascii "VMS-UCRT0"
	.byte	0xe
	.ascii "GNU C17 14.2.0"
	.word	0x13
	.word	0xbe
	.byte	0x80
	.long	__main..en
	.long	__main
	.byte	0x6
	.ascii "__main"
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
	.long	0x4d
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x4d
	.byte	0x11
	.long	$LVM1-	.text
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM2-$LVM1
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM3-$LVM2
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x53
	.byte	0x11
	.long	$LVM4-$LVM3
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x4
	.byte	0x11
	.long	$LVM5-$LVM4
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x58
	.byte	0x11
	.long	$LVM6-$LVM5
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x58
	.byte	0x11
	.long	$LVM7-$LVM6
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x58
	.byte	0x11
	.long	$LVM8-$LVM7
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM9-$LVM8
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5a
	.byte	0x11
	.long	$LVM10-$LVM9
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM11-$LVM10
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5b
	.byte	0x11
	.long	$LVM12-$LVM11
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5b
	.byte	0x11
	.long	$LVM13-$LVM12
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5b
	.byte	0x11
	.long	$LVM14-$LVM13
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5b
	.byte	0x11
	.long	$LVM15-$LVM14
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5a
	.byte	0x11
	.long	$LVM16-$LVM15
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5a
	.byte	0x11
	.long	$LVM17-$LVM16
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM18-$LVM17
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5d
	.byte	0x11
	.long	$LVM19-$LVM18
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM20-$LVM19
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5f
	.byte	0x11
	.long	$LVM21-$LVM20
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5f
	.byte	0x11
	.long	$LVM22-$LVM21
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5f
	.byte	0x11
	.long	$LVM23-$LVM22
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x5f
	.byte	0x11
	.long	$LVM24-$LVM23
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM25-$LVM24
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x61
	.byte	0x11
	.long	$LVM26-$LVM25
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x61
	.byte	0x11
	.long	$LVM27-$LVM26
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x61
	.byte	0x11
	.long	$LVM28-$LVM27
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM29-$LVM28
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x63
	.byte	0x11
	.long	$LVM30-$LVM29
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM31-$LVM30
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x64
	.byte	0x11
	.long	$LVM32-$LVM31
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x64
	.byte	0x11
	.long	$LVM33-$LVM32
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x64
	.byte	0x11
	.long	$LVM34-$LVM33
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x64
	.byte	0x11
	.long	$LVM35-$LVM34
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x63
	.byte	0x11
	.long	$LVM36-$LVM35
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x63
	.byte	0x11
	.long	$LVM37-$LVM36
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x63
	.byte	0x11
	.long	$LVM38-$LVM37
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM39-$LVM38
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x66
	.byte	0x11
	.long	$LVM40-$LVM39
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x3
	.byte	0x11
	.long	$LVM41-$LVM40
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6a
	.byte	0x11
	.long	$LVM42-$LVM41
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM43-$LVM42
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x6b
	.byte	0x11
	.long	$LVM44-$LVM43
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM45-$LVM44
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM46-$LVM45
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x70
	.byte	0x11
	.long	$LVM47-$LVM46
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM48-$LVM47
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM49-$LVM48
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM50-$LVM49
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM51-$LVM50
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x79
	.byte	0x11
	.long	$LVM52-$LVM51
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x79
	.byte	0x11
	.long	$LVM53-$LVM52
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x79
	.byte	0x11
	.long	$LVM54-$LVM53
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x8
	.byte	0x11
	.long	$LVM55-$LVM54
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x1
	.byte	0x11
	.long	$LVM56-$LVM55
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM57-$LVM56
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x3
	.byte	0x11
	.long	$LVM58-$LVM57
	.word	0xd
	.word	0xb9
	.byte	0x12
	.long	0x2
	.byte	0x11
	.long	$LVM59-$LVM58
	.word	0x8
	.word	0xb9
	.byte	0x11
	.long	$LVM60-$LVM59
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x8d
	.byte	0x11
	.long	$LVM61-$LVM60
	.word	0xd
	.word	0xb9
	.byte	0x14
	.long	0x8d
	.byte	0x11
	.long	$LVM62-$LVM61
	.word	0x3
	.word	0xbd
