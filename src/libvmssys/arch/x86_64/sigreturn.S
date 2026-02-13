/*
 * sigreturn.S - rt_sigreturn trampoline for x86_64
 *
 * Without glibc, we must provide our own restorer function for
 * signal handlers.  The kernel expects sa_restorer to point to
 * code that executes the rt_sigreturn syscall.
 *
 * This function is set as the sa_restorer field in struct sigaction
 * when installing signal handlers.  The kernel calls it after the
 * user's signal handler returns.
 */

.text
.globl __vms_rt_sigreturn
.type __vms_rt_sigreturn, @function

__vms_rt_sigreturn:
    movq $15, %rax    /* __NR_rt_sigreturn = 15 */
    syscall
    /* Should never return; if it does, infinite loop */
    jmp __vms_rt_sigreturn

.size __vms_rt_sigreturn, . - __vms_rt_sigreturn

/* Mark stack as non-executable */
.section .note.GNU-stack,"",@progbits
