/*
 * sigreturn.S - rt_sigreturn trampoline for AArch64
 */

.text
.globl __vms_rt_sigreturn
.type __vms_rt_sigreturn, @function

__vms_rt_sigreturn:
    mov x8, #139          /* __NR_rt_sigreturn on aarch64 */
    svc #0
    brk #1                /* should never return */

.size __vms_rt_sigreturn, . - __vms_rt_sigreturn

.section .note.GNU-stack,"",@progbits
