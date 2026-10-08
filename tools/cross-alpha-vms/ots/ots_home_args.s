# ots_home_args.s — OTS$HOME_ARGS for alpha-dec-vms (bead vms-bfd6; floating
# arguments vms-e839).
#
# The varargs prologue the alpha-dec-vms GCC port emits calls OTS$HOME_ARGS to
# spill ("home") the incoming argument registers into the caller's stack
# argument-home area so va_arg can walk them as a contiguous array. It is a
# SPECIAL-LINKAGE routine and cannot be written in C:
#
#   * called   `lda $0,OTS$HOME_ARGS ; ldq $0,8($0) ; jsr $0,OTS$HOME_ARGS`
#     -> the RETURN address is left in R0 (NOT the standard R26), precisely so
#        the call does not disturb the caller's own saved R26.
#   * on entry R1 = base of the home area; R25 = AI (argument information);
#     R16..R21 / F16..F21 = the incoming argument registers. The compiler's
#     arg_home pattern lists R16-R21, F16-F21, R1 and R25 as used and R0, R24
#     and R25 as clobbered, so R24 is free scratch.
#
# HOME-AREA LAYOUT — derived empirically from cc1 output (clean-room, Rule 8):
# for a function with N fixed args, va_start points at home+8*(1+N), and the
# k-th argument (k = 0..5) is read at home + 8 + 8*k. So:
#
#     home[0]  = AI (R25)         home[8]  = arg0   home[16] = arg1
#     home[24] = arg2             home[32] = arg3   home[40] = arg4
#     home[48] = arg5
#
# WHICH REGISTER IS AN ARGUMENT (vms-e839). The caller passes argument k in
# R(16+k) if it is an integer or address and in F(16+k) if it is floating; the
# AI register says which: bits <7:0> are the argument count and the 3-bit
# field at bit 8+3k is argument k's register type -- 0 for a 64-bit integer,
# 1..5 for the floating types (VAX F/D/G, IEEE S/T). Observed from cc1: a call
# vf("x", 1.5, 7, 2.5f, 3.25) loads AI = 0x5A2805 (count 5; types 0,5,0,5,5:
# the promoted float and both doubles are IEEE T). Homing the R register for a
# floating argument -- what this routine did -- handed va_arg(ap, double) an
# unrelated integer register, so printf("%f", x) printed 0.
#
# The compiler always reserves the full 7-quadword home area when it emits the
# HOME_ARGS call (it cannot know the call-site argument count), so homing all
# six argument slots plus the AI unconditionally is always in-bounds. A
# floating slot is stored with STT: the register image va_arg reads back as a
# double. Returns via R0; touches only memory and R24.

	.set noreorder
	.set volatile
	.text
	.align 2
	.globl OTS$HOME_ARGS
	.ent OTS$HOME_ARGS
OTS$HOME_ARGS..en:
	.base $27
	.frame $30,0,$0,8
	.prologue
	stq $25,0($1)		# home[0]  = AI (argument count + types)
	srl $25,8,$24		# arg0: register type
	and $24,7,$24
	bne $24,$Lf0
	stq $16,8($1)		# integer: R16
	br $31,$Ld0
$Lf0:
	stt $f16,8($1)		# floating: F16
$Ld0:
	srl $25,11,$24		# arg1: register type
	and $24,7,$24
	bne $24,$Lf1
	stq $17,16($1)		# integer: R17
	br $31,$Ld1
$Lf1:
	stt $f17,16($1)		# floating: F17
$Ld1:
	srl $25,14,$24		# arg2: register type
	and $24,7,$24
	bne $24,$Lf2
	stq $18,24($1)		# integer: R18
	br $31,$Ld2
$Lf2:
	stt $f18,24($1)		# floating: F18
$Ld2:
	srl $25,17,$24		# arg3: register type
	and $24,7,$24
	bne $24,$Lf3
	stq $19,32($1)		# integer: R19
	br $31,$Ld3
$Lf3:
	stt $f19,32($1)		# floating: F19
$Ld3:
	srl $25,20,$24		# arg4: register type
	and $24,7,$24
	bne $24,$Lf4
	stq $20,40($1)		# integer: R20
	br $31,$Ld4
$Lf4:
	stt $f20,40($1)		# floating: F20
$Ld4:
	srl $25,23,$24		# arg5: register type
	and $24,7,$24
	bne $24,$Lf5
	stq $21,48($1)		# integer: R21
	br $31,$Ld5
$Lf5:
	stt $f21,48($1)		# floating: F21
$Ld5:
	ret $31,($0),1		# return via R0 (special linkage), not R26
	.link
	.align 3
OTS$HOME_ARGS:
	.pdesc OTS$HOME_ARGS..en,null
	.end OTS$HOME_ARGS
