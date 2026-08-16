/*
 * IMGACT.EXE — Alpha assembly entry, OVMX-native TLS resolver, TP get/set.
 *
 * Part of OVMX, bead vms-e11 (Alpha[A2]). See docs/design-image-activation.md.
 *
 * The kernel enters IMGACT.EXE (the ELF PT_INTERP) with:
 *   $30 (sp) -> { argc, argv[0..argc-1], NULL, envp..., NULL, auxv..., AT_NULL }
 * for the *target* executable, and $27 (pv) is NOT guaranteed to hold this
 * routine's own address (unlike a normal jsr/bsr call) — so, like
 * src/libvmssys/arch/alpha/crt0.S, we cannot rely on `ldgp $29,0($27)` and
 * must establish gp via the position-independent br/ldgp idiom before
 * touching any global data (imgact_bootstrap self-relocates IMGACT first
 * thing, which itself needs a working gp to reach the hidden _DYNAMIC
 * symbol).
 *
 * We hand the raw stack pointer (in $16, Alpha's first argument register) to
 * imgact_bootstrap(), which self-relocates IMGACT (it is static-PIE), links
 * the target and its shareable images, sets up TLS, and returns the target's
 * entry point in $0. The jsr/ret pair leaves $30 unchanged, so on return sp
 * still points at the pristine initial stack; we clear the registers the
 * kernel would have zeroed at process start and jump to the target entry.
 */
	.set noat
	.text

	.globl _start
	.hidden _start
	.ent   _start
_start:
	.frame $30, 0, $26, 0
	.prologue 0

	/* Establish gp ($29) without relying on $27/pv at kernel entry. */
	br	$29, 1f
1:	ldgp	$29, 0($29)

	mov	$30, $16		/* a0 = pointer to initial process stack   */
	jsr	$26, imgact_bootstrap	/* returns target entry point in $0        */
	ldgp	$29, 0($26)		/* reload gp after the call                */
	mov	$0, $27			/* stash entry (pv, for the coming jmp)    */

	/* Clear the GP registers the kernel would zero at process start. */
	mov	$31, $0
	mov	$31, $1
	mov	$31, $2
	mov	$31, $3
	mov	$31, $4
	mov	$31, $5
	mov	$31, $6
	mov	$31, $7
	mov	$31, $8
	mov	$31, $9
	mov	$31, $10
	mov	$31, $11
	mov	$31, $12
	mov	$31, $13
	mov	$31, $14
	mov	$31, $15
	mov	$31, $16
	mov	$31, $17
	mov	$31, $18
	mov	$31, $19
	mov	$31, $20
	mov	$31, $21
	mov	$31, $22
	mov	$31, $23
	mov	$31, $24
	mov	$31, $25
	mov	$31, $26
	mov	$31, $29
	jmp	$31, ($27)		/* jump to target executable _start        */
	.end   _start

/*
 * void *__tlsdesc_static(void *descriptor)   [$16 = &descriptor, returns $0]
 *
 * OVMX-native symbol-vector TLS resolver (imgact.c's setup_symvec_tls /
 * symvec_tls_place / absorb_tls_over_crtl): the loader stores the
 * TP-relative offset of the variable in descriptor word 1 (offset 8); we
 * return it. This is OVMX's OWN two-word-descriptor convention for images
 * produced by OVMX's own LINK.EXE (not yet ported to Alpha) — it reuses the
 * "descriptor" shape the x86_64/aarch64 backends use for the same purpose,
 * but is NOT the hardware/ELF TLSDESC ABI, which classic Alpha has no
 * compiler or PALcode support for (confirmed: `alpha-linux-gnu-gcc
 * -mtls-dialect=desc` is rejected as an unrecognized option). Standard Alpha
 * C calling convention: first arg in $16 (a0), return value in $0 (v0).
 */
	.globl __tlsdesc_static
	.hidden __tlsdesc_static
	.ent   __tlsdesc_static
__tlsdesc_static:
	.frame $30, 0, $26, 0
	.prologue 0
	ldq	$0, 8($16)		/* return descriptor->arg (TP offset)      */
	ret
	.end   __tlsdesc_static

/*
 * void imgact_set_tp(void *tp)   [$16 = thread pointer]
 * Program the userspace thread pointer via PALcode wrunique (call_pal 0x9f),
 * which takes the new value in $16 (a0) per the Linux/Alpha ABI — proven
 * empirically under qemu-alpha (round-tripped through imgact_get_tp below).
 */
	.globl imgact_set_tp
	.hidden imgact_set_tp
	.ent   imgact_set_tp
imgact_set_tp:
	.frame $30, 0, $26, 0
	.prologue 0
	call_pal 0x9f			/* wrunique                                */
	ret
	.end   imgact_set_tp

/*
 * void *imgact_get_tp(void)   [returns the thread pointer in $0]
 * Read the userspace thread pointer via PALcode rduniq (call_pal 0x9e),
 * which returns the current value in $0 (v0) per the Linux/Alpha ABI.
 */
	.globl imgact_get_tp
	.hidden imgact_get_tp
	.ent   imgact_get_tp
imgact_get_tp:
	.frame $30, 0, $26, 0
	.prologue 0
	call_pal 0x9e			/* rduniq                                  */
	ret
	.end   imgact_get_tp

	.section .note.GNU-stack,"",@progbits
