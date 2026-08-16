/*
 * sigreturn.S - rt_sigreturn trampoline for Alpha (alpha-linux-gnu)
 *
 * Without glibc we supply our own restorer.  The kernel invokes this
 * after a signal handler returns; it must issue the rt_sigreturn
 * syscall, which never returns.
 *
 * __NR_rt_sigreturn == 351 on Alpha (OSF-derived syscall table; verified
 * against the alpha-linux-gnu <asm/unistd.h> in the cross toolchain).
 */
	.set noat
	.text
	.globl __vms_rt_sigreturn
	.ent   __vms_rt_sigreturn
__vms_rt_sigreturn:
	.frame $30, 0, $26, 0
	.prologue 0
	lda	$0, 351($31)		/* __NR_rt_sigreturn */
	callsys
	br	$31, __vms_rt_sigreturn	/* never returns */
	.end   __vms_rt_sigreturn

	.section .note.GNU-stack,"",@progbits
