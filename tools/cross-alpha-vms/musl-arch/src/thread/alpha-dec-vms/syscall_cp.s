/*
 * syscall_cp.s - musl cancellable-syscall trampoline for the OVMX
 * alpha-dec-vms musl port (vms-838a). Defines __syscall_cp_asm and the three
 * cancellation-point boundary labels __cp_begin / __cp_end / __cp_cancel that
 * musl's pthread_cancel.c cancel_handler uses to decide whether an interrupted
 * PC is inside the cancellable syscall region and, if so, redirect it to the
 * cancel path.
 *
 * musl never shipped an Alpha port, so there is no upstream alpha version to
 * copy; this is an ORIGINAL alpha-dec-vms shim. Rule-8 clean-room: the frame /
 * GP / VMS linkage-pair idiom below is the alpha-dec-vms cc1's OWN output for
 * the equivalent C (compiled from our own reference source, not VSI material);
 * the only hand-additions are the three .globl boundary labels musl's design
 * requires, placed to bracket the syscall. NEVER copied from VSI/HPE source.
 *
 * musl contract (src/thread/pthread_cancel.c, __syscall_cp.c):
 *   long __syscall_cp_asm(volatile void *cancelflag, syscall_arg_t nr,
 *                         arg1, arg2, arg3, arg4, arg5, arg6);
 *   - if *cancelflag is already set, do NOT run the syscall; go to __cp_cancel,
 *     which tail-calls __cancel (which never fakes success — it acts on the
 *     pending cancellation).
 *   - otherwise perform the syscall and return its result.
 *   __cp_begin .. __cp_end bracket the region the async cancel handler may
 *   redirect (VMS calling standard: args a0..a5 in $16..$21, a6/a7 spilled by
 *   the caller; this routine has a full frame because it CALLS out).
 *
 * THE SYSCALL PATH IS REAL, NOT FAKED (INV-6): "performing the syscall" on this
 * port means calling __vms_alpha_syscall(nr, a1..a6) — the port's single,
 * honest syscall backend (src/internal/vms_alpha_syscall.c). At RUNG-1 that
 * backend returns -ENOSYS (no OVMX Alpha executive trap wired yet, GAP3/
 * vms-8954); this shim routes through it exactly as every __syscallN does, so
 * it fails honestly with ENOSYS rather than fabricating a result. When the
 * executive backend lands, this shim performs real syscalls with no change.
 *
 * CHARACTERIZED GAP (honest): full ASYNCHRONOUS cancellation — a SIGCANCEL
 * arriving mid-syscall and the handler rewriting the interrupted PC to
 * __cp_cancel — cannot be exercised until the executive/signal backend
 * (GAP3/vms-8954) delivers SIGCANCEL and presents real trap PCs. What is honest
 * and live at rung-1: the four symbols resolve to real defs, the region is
 * correctly bracketed for that future handler, and the SYNCHRONOUS pre-syscall
 * cancel-flag check + __cancel tail-call work today.
 */
	.set noreorder
	.set volatile
	.text
	.align 4
	.globl __syscall_cp_asm
	.globl __cp_begin
	.globl __cp_end
	.globl __cp_cancel
	.ent __syscall_cp_asm
__syscall_cp_asm..en:
	.base $27
	.frame $29,32,$26,8
	.mask 0x20000000,0
	lda $30,-32($30)
	cpys $f31,$f31,$f31
	stq $29,16($30)
	mov $30,$29
	stq $27,0($30)
	stq $26,8($30)
	lda $30,-16($29)
	.prologue
/* __cp_begin: cancellable region starts here — cancel-flag test + the syscall */
__cp_begin:
	ldl $1,0($16)		/* $1 = *cancelflag                                */
	mov $17,$16		/* shift args down: syscall nr -> a0               */
	mov $18,$17		/* a1 -> a0's neighbour ... (arg1 -> $17)           */
	mov $19,$18		/* arg2 -> $18                                     */
	mov $20,$19		/* arg3 -> $19                                     */
	mov $21,$20		/* arg4 -> $20                                     */
	bne $1,$cp_cancel_l	/* already-cancelled: skip the syscall             */
	ldl $1,40($29)		/* arg6: caller's 2nd stacked arg                  */
	lda $25,7($31)		/* $25 = arg-info: 7 args to __vms_alpha_syscall   */
	ldl $21,32($29)		/* arg5: caller's 1st stacked arg -> a5 ($21)      */
	stq $1,0($30)		/* arg6 spilled for the callee's 7th param         */
	ldq $26,$0..__vms_alpha_syscall..lk
	ldq $27,$0..__vms_alpha_syscall..lk+8
	jsr $26,__vms_alpha_syscall
	ldq $27,0($29)		/* restore GP after the call                       */
/* __cp_end: end of the cancellable region; $0 holds the syscall result */
__cp_end:
	mov $29,$30
	ldq $26,8($30)
	ldq $29,16($30)
	lda $30,32($30)
	ret $31,($26),1
	.align 4
/* __cp_cancel: reached by the pre-syscall flag test OR by the async handler
 * rewriting a mid-region PC here. Tail-call __cancel (does not fake success). */
$cp_cancel_l:
__cp_cancel:
	mov $31,$25		/* $25 = arg-info: 0 args to __cancel               */
	ldq $26,$0..__cancel..lk
	ldq $27,$0..__cancel..lk+8
	jsr $26,__cancel
	ldq $27,0($29)
	mov $29,$30
	ldq $26,8($30)
	ldq $29,16($30)
	lda $30,32($30)
	ret $31,($26),1
	.link
	.align 3
__syscall_cp_asm:
	.pdesc __syscall_cp_asm..en,stack
$0..__vms_alpha_syscall..lk:
	.linkage __vms_alpha_syscall
$0..__cancel..lk:
	.linkage __cancel
	.end __syscall_cp_asm
