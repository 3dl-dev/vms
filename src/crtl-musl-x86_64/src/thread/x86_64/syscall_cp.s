/*
 * syscall_cp.s - musl's cancellable system call for OVMX's x86_64 C RTL
 * (rd vms-569), replacing musl 1.2.4's src/thread/x86_64/syscall_cp.s.
 *
 * musl issues its cancellation-point system calls -- open, read, write, close,
 * pause, nanosleep, wait4, ... -- through __syscall_cp_asm once a program links
 * pthread_cancel. musl's own version executes the `syscall` instruction here,
 * past the OVMX system-call funnel (src/internal/ovmx_syscall.c), so those calls
 * never reached the C RTL layer over RMS and VMS channels. A static image seldom
 * links pthread_cancel; DECC$SHR whole-archives libc.a and always does, so in a
 * DECC$SHR program open("/proc/...") opened, read() of a pipe's mailbox went to
 * the kernel and pause() waited in Linux instead of $HIBER.
 *
 * Here the call goes to __ovmx_syscall(nr, a1..a6) like every other musl
 * system call -- the same choice the alpha-dec-vms port makes
 * (tools/cross-alpha-vms/musl-arch/src/thread/alpha-dec-vms/syscall_cp.s).
 * The pending-cancel check before the call is kept; a cancellation request
 * arriving while the call blocks is acted on when it returns EINTR
 * (pthread_cancel.c __syscall_cp_c), not by the signal handler moving the
 * interrupted PC: the funnel is ordinary C between __cp_begin and __cp_end.
 *
 * In:  rdi = &self->cancel, rsi = nr, rdx..r9 = a1..a4, 8(%rsp) = a5,
 *      16(%rsp) = a6.
 * Out: tail call __ovmx_syscall with rdi = nr, rsi..r9 = a1..a5, 8(%rsp) = a6
 *      (the callee owns its incoming argument slots).
 */
.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type   __syscall_cp_asm,@function
__syscall_cp_asm:

__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	mov %rsi,%rdi
	mov %rdx,%rsi
	mov %rcx,%rdx
	mov %r8,%rcx
	mov %r9,%r8
	mov 8(%rsp),%r9
	mov 16(%rsp),%rax
	mov %rax,8(%rsp)
	jmp __ovmx_syscall
__cp_end:
	ret
__cp_cancel:
	jmp __cancel
