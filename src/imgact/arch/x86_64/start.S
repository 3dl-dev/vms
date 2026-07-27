/*
 * IMGACT.EXE — x86_64 assembly entry, TLS descriptor resolver, TP setter.
 *
 * Part of OVMX, bead vms-913.11. See docs/design-image-activation.md.
 *
 * The kernel enters IMGACT.EXE (the ELF PT_INTERP) with:
 *   rsp -> { argc, argv[0..argc-1], NULL, envp..., NULL, auxv..., AT_NULL }
 * for the *target* executable, and the general registers zeroed.
 *
 * We hand the raw stack pointer to imgact_bootstrap(), which self-relocates
 * IMGACT (it is static-PIE), links the target and its shareable images, sets
 * up TLS, and returns the target's entry point. The call/ret pair leaves rsp
 * unchanged, so on return rsp still points at the pristine initial stack; we
 * clear the general registers and jump to the target entry exactly as the
 * kernel would (rdx = 0: no rtld_fini function to register with atexit).
 */
	.text

	.global _start
	.hidden _start
	.type _start,@function
_start:
	mov	%rsp, %rdi		/* rdi = pointer to initial process stack  */
	call	imgact_bootstrap	/* returns target entry point in rax       */
	mov	%rax, %r11		/* stash entry (r11 is caller-saved)       */
	xor	%eax, %eax
	xor	%ebx, %ebx
	xor	%ecx, %ecx
	xor	%edx, %edx		/* rdx = 0: no rtld_fini                   */
	xor	%esi, %esi
	xor	%edi, %edi
	xor	%ebp, %ebp
	jmp	*%r11			/* jump to target executable _start        */
	.size _start,.-_start

/*
 * long __tlsdesc_static(struct tlsdesc *td)   [%rax = &descriptor]
 *
 * Static TLSDESC resolver. The loader stores the TP-relative offset of the
 * variable in descriptor word 1; we return it in %rax. Per the x86_64 TLSDESC
 * ABI this routine receives the descriptor address in %rax, returns the offset
 * in %rax, and MUST preserve every other register (the caller then computes
 * %fs:0 + return_value to obtain the variable's address).
 */
	.global __tlsdesc_static
	.hidden __tlsdesc_static
	.type __tlsdesc_static,@function
__tlsdesc_static:
	mov	8(%rax), %rax		/* return descriptor->arg (TP offset)      */
	ret
	.size __tlsdesc_static,.-__tlsdesc_static

/*
 * void imgact_set_tp(void *tp)   [%rdi = thread pointer]
 * Program the userspace thread pointer via arch_prctl(ARCH_SET_FS, tp).
 * arch_prctl(int code, unsigned long addr): code in %rdi, addr in %rsi.
 */
	.global imgact_set_tp
	.hidden imgact_set_tp
	.type imgact_set_tp,@function
imgact_set_tp:
	mov	%rdi, %rsi		/* addr = tp                               */
	mov	$0x1002, %edi		/* ARCH_SET_FS                             */
	mov	$158, %eax		/* SYS_arch_prctl                          */
	syscall
	ret
	.size imgact_set_tp,.-imgact_set_tp

/*
 * void *imgact_get_tp(void)   [returns the thread pointer (FS base) in %rax]
 * Read the userspace thread pointer. Used when a C-RTL (musl DECC$SHR) already
 * owns TP and IMGACT must resolve a coexisting TLS producer's static TLSDESC
 * entries relative to that already-programmed TP (bead vms-616). On x86_64 the
 * TP is the FS base; musl stores the TCB self-pointer at %fs:0, so read it there
 * (no rdfsbase dependency).
 */
	.global imgact_get_tp
	.hidden imgact_get_tp
	.type imgact_get_tp,@function
imgact_get_tp:
	mov	%fs:0, %rax
	ret
	.size imgact_get_tp,.-imgact_get_tp

	.section .note.GNU-stack,"",@progbits
