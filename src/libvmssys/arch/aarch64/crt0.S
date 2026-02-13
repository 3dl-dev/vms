/*
 * crt0.S - Custom _start entry point for AArch64 (no glibc)
 *
 * Kernel places on stack at entry:
 *   sp+0:   argc
 *   sp+8:   argv[0] ... argv[argc-1], NULL
 *   then:   envp[0] ... envp[n], NULL
 *   then:   auxv pairs ... AT_NULL
 *
 * AArch64 calling convention: x0-x7 = args, x30 = LR
 * Stack must be 16-byte aligned before function calls.
 */

.text
.globl _start
.type _start, @function

_start:
    /* Clear frame pointer */
    mov x29, #0
    mov x30, #0

    /* Load argc, argv, envp */
    ldr x0, [sp]           /* argc */
    add x1, sp, #8         /* argv = sp + 8 */
    /* envp = argv + (argc + 1) * 8 */
    add x2, x0, #1
    lsl x2, x2, #3
    add x2, x1, x2         /* envp */

    /* Align stack to 16 bytes */
    mov x3, sp
    and x3, x3, #-16
    mov sp, x3

    /* Save args for main */
    stp x0, x1, [sp, #-32]!
    str x2, [sp, #16]

    /* Call __vms_runtime_init(argc, argv, envp) */
    bl __vms_runtime_init

    /* Restore args */
    ldp x0, x1, [sp]
    ldr x2, [sp, #16]
    add sp, sp, #32

    /* Call main(argc, argv, envp) */
    bl main

    /* Exit with return value */
    mov x8, #94            /* __NR_exit_group on aarch64 */
    svc #0

    /* Unreachable */
    brk #1

.size _start, . - _start

.section .note.GNU-stack,"",@progbits
