/*
 * syscall.S - Raw Linux syscall trampoline for x86_64
 *
 * Linux x86_64 syscall convention:
 *   syscall number: %rax
 *   arguments:      %rdi, %rsi, %rdx, %r10, %r8, %r9
 *   return value:   %rax (negative = -errno)
 *   clobbered:      %rcx, %r11
 *
 * Note: arg4 is %r10 (not %rcx as in the C ABI), because the
 * syscall instruction itself clobbers %rcx (saves RIP there).
 */

.text
.globl __vms_syscall0
.globl __vms_syscall1
.globl __vms_syscall2
.globl __vms_syscall3
.globl __vms_syscall4
.globl __vms_syscall5
.globl __vms_syscall6

.type __vms_syscall0, @function
.type __vms_syscall1, @function
.type __vms_syscall2, @function
.type __vms_syscall3, @function
.type __vms_syscall4, @function
.type __vms_syscall5, @function
.type __vms_syscall6, @function

/* syscall0(nr) */
__vms_syscall0:
    movq %rdi, %rax
    syscall
    ret
.size __vms_syscall0, . - __vms_syscall0

/* syscall1(nr, a1) */
__vms_syscall1:
    movq %rdi, %rax
    movq %rsi, %rdi
    syscall
    ret
.size __vms_syscall1, . - __vms_syscall1

/* syscall2(nr, a1, a2) */
__vms_syscall2:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    syscall
    ret
.size __vms_syscall2, . - __vms_syscall2

/* syscall3(nr, a1, a2, a3) */
__vms_syscall3:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    syscall
    ret
.size __vms_syscall3, . - __vms_syscall3

/* syscall4(nr, a1, a2, a3, a4) */
__vms_syscall4:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8,  %r10
    syscall
    ret
.size __vms_syscall4, . - __vms_syscall4

/* syscall5(nr, a1, a2, a3, a4, a5) */
__vms_syscall5:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8,  %r10
    movq %r9,  %r8
    syscall
    ret
.size __vms_syscall5, . - __vms_syscall5

/* syscall6(nr, a1, a2, a3, a4, a5, a6) - a6 on stack */
__vms_syscall6:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8,  %r10
    movq %r9,  %r8
    movq 8(%rsp), %r9
    syscall
    ret
.size __vms_syscall6, . - __vms_syscall6

/* Mark stack as non-executable */
.section .note.GNU-stack,"",@progbits
