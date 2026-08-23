# ots_home_args.s — OTS$HOME_ARGS for alpha-dec-vms (bead vms-bfd6).
#
# The varargs prologue the alpha-dec-vms GCC port emits calls OTS$HOME_ARGS to
# spill ("home") the incoming argument registers into the caller's stack
# argument-home area so va_arg can walk them as a contiguous array. It is a
# SPECIAL-LINKAGE routine and cannot be written in C:
#
#   * called   `lda $0,OTS$HOME_ARGS ; ldq $0,8($0) ; jsr $0,OTS$HOME_ARGS`
#     -> the RETURN address is left in R0 (NOT the standard R26), precisely so
#        the call does not disturb the caller's own saved R26.
#   * on entry R1 = base of the home area; R25 = AI (argument-info, low bits =
#     argument count); R16..R21 = the incoming argument registers.
#   * it must PRESERVE every register (it runs mid-prologue).
#
# HOME-AREA LAYOUT — derived empirically from cc1 output (clean-room, Rule 8):
# for a function with N fixed args, va_start points at home+8*(1+... ), and the
# first variadic register Rk is read at home + 8 + 8*(k-16). So:
#
#     home[0]  = AI (R25)         home[8]  = R16 (arg0)   home[16] = R17 (arg1)
#     home[24] = R18 (arg2)       home[32] = R19 (arg3)   home[40] = R20 (arg4)
#     home[48] = R21 (arg5)
#
# The compiler always reserves the full 7-quadword home area when it emits the
# HOME_ARGS call (it cannot know the call-site argument count), so homing all
# six argument registers plus the AI unconditionally is always in-bounds and is
# never a wrong answer. This routine only writes memory and reads fixed input
# registers — it clobbers nothing and returns via R0.

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
	stq $25,0($1)		# home[0]  = AI (argument count)
	stq $16,8($1)		# home[8]  = arg0
	stq $17,16($1)		# home[16] = arg1
	stq $18,24($1)		# home[24] = arg2
	stq $19,32($1)		# home[32] = arg3
	stq $20,40($1)		# home[40] = arg4
	stq $21,48($1)		# home[48] = arg5
	ret $31,($0),1		# return via R0 (special linkage), not R26
	.link
	.align 3
OTS$HOME_ARGS:
	.pdesc OTS$HOME_ARGS..en,null
	.end OTS$HOME_ARGS
