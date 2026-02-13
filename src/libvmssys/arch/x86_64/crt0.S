/*
 * crt0.S - Custom _start entry point for x86_64 (no glibc)
 *
 * When the kernel starts a new process, the stack looks like:
 *
 *   (%rsp)      argc
 *   8(%rsp)     argv[0]
 *   ...         argv[argc-1]
 *               NULL
 *               envp[0]
 *   ...         envp[n-1]
 *               NULL
 *               auxv[0] (key, value pairs)
 *   ...         AT_NULL
 *
 * We extract these and call:
 *   __vms_runtime_init(argc, argv, envp)
 *   main(argc, argv, envp)
 *   exit_group(retval)
 *
 * Stack must be 16-byte aligned before call instruction.
 */

.text
.globl _start
.type _start, @function

_start:
    /* Clear frame pointer for clean backtraces */
    xorq %rbp, %rbp

    /* argc is at (%rsp), move to %rdi */
    movq (%rsp), %rdi        /* argc */

    /* argv = rsp + 8 */
    leaq 8(%rsp), %rsi       /* argv */

    /* envp = argv + (argc + 1) * 8 */
    leaq 8(%rsi,%rdi,8), %rdx /* envp */

    /* Align stack to 16 bytes (it's 8-byte aligned at entry) */
    andq $-16, %rsp

    /* Call runtime initialization */
    /* __vms_runtime_init(argc, argv, envp) */
    pushq %rdx               /* save envp */
    pushq %rsi               /* save argv */
    pushq %rdi               /* save argc */

    call __vms_runtime_init

    /* Restore args for main */
    popq %rdi                /* argc */
    popq %rsi                /* argv */
    popq %rdx                /* envp */

    /* Align stack again before call */
    andq $-16, %rsp
    subq $8, %rsp

    /* Call main(argc, argv, envp) */
    call main

    /* Exit with main's return value */
    movq %rax, %rdi
    call vms_sys_exit_group

    /* Should never reach here */
    ud2

.size _start, . - _start

/* Mark stack as non-executable */
.section .note.GNU-stack,"",@progbits
