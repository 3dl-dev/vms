/*
 * syscall.S - Raw Linux syscall trampoline for AArch64
 *
 * AArch64 syscall convention:
 *   syscall number: x8
 *   arguments:      x0, x1, x2, x3, x4, x5
 *   return value:   x0 (negative = -errno)
 *   instruction:    svc #0
 *
 * C calling convention: x0-x7 are argument registers.
 * Our trampoline receives (nr, a1, a2, ...) and must shuffle
 * nr into x8 while shifting args down.
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
    mov x8, x0
    svc #0
    ret
.size __vms_syscall0, . - __vms_syscall0

/* syscall1(nr, a1) */
__vms_syscall1:
    mov x8, x0
    mov x0, x1
    svc #0
    ret
.size __vms_syscall1, . - __vms_syscall1

/* syscall2(nr, a1, a2) */
__vms_syscall2:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    svc #0
    ret
.size __vms_syscall2, . - __vms_syscall2

/* syscall3(nr, a1, a2, a3) */
__vms_syscall3:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    svc #0
    ret
.size __vms_syscall3, . - __vms_syscall3

/* syscall4(nr, a1, a2, a3, a4) */
__vms_syscall4:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    svc #0
    ret
.size __vms_syscall4, . - __vms_syscall4

/* syscall5(nr, a1, a2, a3, a4, a5) */
__vms_syscall5:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    svc #0
    ret
.size __vms_syscall5, . - __vms_syscall5

/* syscall6(nr, a1, a2, a3, a4, a5, a6) */
__vms_syscall6:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret
.size __vms_syscall6, . - __vms_syscall6

/* Mark stack as non-executable */
.section .note.GNU-stack,"",@progbits
