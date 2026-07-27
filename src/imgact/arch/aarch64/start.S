/*
 * IMGACT.EXE — aarch64 assembly entry, TLS descriptor resolver, TP setter.
 *
 * Part of OVMX, bead vms-913.2. See docs/design-image-activation.md.
 *
 * The kernel enters IMGACT.EXE (the ELF PT_INTERP) with:
 *   sp -> { argc, argv[0..argc-1], NULL, envp..., NULL, auxv..., AT_NULL }
 * for the *target* executable, and all GP registers zeroed.
 *
 * We hand the raw stack pointer to imgact_bootstrap(), which self-relocates
 * IMGACT (it is static-PIE), links the target and its shareable images, sets
 * up TLS, and returns the target's entry point. We then restore the pristine
 * initial stack (the bl/ret pair leaves sp unchanged) and branch to the
 * target entry with GP registers cleared, exactly as the kernel would.
 */
	.text

	.global _start
	.hidden _start
	.type _start,%function
_start:
	mov	x0, sp			/* x0 = pointer to initial process stack   */
	bl	imgact_bootstrap	/* returns target entry point in x0        */
	mov	x16, x0			/* stash entry                             */
	/* Clear the GP registers the kernel would zero at process start.   */
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x29, #0
	mov	x30, #0
	br	x16			/* jump to target executable _start        */
	.size _start,.-_start

/*
 * ptrdiff_t __tlsdesc_static(struct tlsdesc *td)  [x0 = &descriptor]
 *
 * Static TLSDESC resolver. The loader stores the TP-relative offset of the
 * variable in descriptor word 1; we simply return it. Per the TLSDESC ABI
 * this routine may clobber only x0 — the access sequence then computes
 * TPIDR_EL0 + return_value to obtain the variable's address.
 */
	.global __tlsdesc_static
	.hidden __tlsdesc_static
	.type __tlsdesc_static,%function
__tlsdesc_static:
	ldr	x0, [x0, #8]		/* return descriptor->arg (TP offset)      */
	ret
	.size __tlsdesc_static,.-__tlsdesc_static

/*
 * void imgact_set_tp(void *tp)  [x0 = thread pointer]
 * Program the userspace thread pointer for the target image's TLS.
 */
	.global imgact_set_tp
	.hidden imgact_set_tp
	.type imgact_set_tp,%function
imgact_set_tp:
	msr	tpidr_el0, x0
	ret
	.size imgact_set_tp,.-imgact_set_tp

/*
 * void *imgact_get_tp(void)  [returns TPIDR_EL0 in x0]
 * Read the userspace thread pointer. Used when a C-RTL (musl DECC$SHR) already
 * owns TP and IMGACT must resolve a coexisting TLS producer's static TLSDESC
 * entries relative to that already-programmed TP (bead vms-616).
 */
	.global imgact_get_tp
	.hidden imgact_get_tp
	.type imgact_get_tp,%function
imgact_get_tp:
	mrs	x0, tpidr_el0
	ret
	.size imgact_get_tp,.-imgact_get_tp

	.section .note.GNU-stack,"",%progbits
