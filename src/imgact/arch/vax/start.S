/*
 * IMGACT.EXE — VAX (elf32-vax) assembly entry + freestanding syscall primitive.
 *
 * rd vms-73b2, epic vms-404. Design: docs/design/vax-symbol-vector-imgact.md
 * §1b (mirror x86_64/aarch64's SysV tail-jump, NOT Alpha's PDSC trampoline).
 *
 * Clean-room (Rule 8): the VAX procedure-calling and chmk syscall conventions
 * are the public VAX architecture / NetVAX BSD conventions, confirmed against
 * the cross toolchain's own libc stubs (objdump of write.o / __syscall.o).
 *
 * The kernel enters IMGACT.EXE (the ELF PT_INTERP) with the target's initial
 * process stack in SP:
 *     sp -> { argc, argv[0..argc-1], NULL, envp..., NULL, auxv..., AT_NULL }
 * and general registers zeroed. VAX addresses PC-relative (no GP/ldgp analog),
 * so no addressability setup is needed — this is simpler than Alpha.
 */
	.text

/*
 * void _start(void)  —  interpreter entry (not called via CALLS; no entry mask)
 *
 * Capture the pristine initial SP, hand it to imgact_bootstrap(sp) (which
 * self-relocates IMGACT, binds the target + its symbol-vector shareables, and
 * returns the target entry point), restore the initial SP, and tail-JMP to the
 * target entry — the SysV flavor, exactly as x86_64's `jmp *%r11` and aarch64's
 * `br x16`. VAX has no procedure descriptor: r0 (the return value) IS the code
 * entry address, so a plain `jmp (%r0)` transfers control.
 */
	.globl _start
	.hidden _start
	.type _start,@function
_start:
	movl	%sp, %r6		/* r6 = pointer to initial process stack   */
	pushl	%r6			/* arg1 = sp                               */
	calls	$1, imgact_bootstrap	/* r0 = target entry point                 */
	movl	%r6, %sp		/* restore the pristine initial stack      */
	jmp	(%r0)			/* tail-transfer to the target executable  */
	.size _start,.-_start

/*
 * long syscall6(long n, long a, long b, long c, long d, long e, long f)
 *
 * Entered via the standard VAX CALLS sequence, so AP points at the argument
 * list { count=7, n, a, b, c, d, e, f }. The NetBSD/vax kernel reads a system
 * call's arguments from AP+4 onward (the count comes from the sysent table),
 * with the syscall number as the chmk operand. So: load n, then advance AP by
 * one longword to { a, b, c, d, e, f } (writing the vacated slot with the arg
 * count for a well-formed CALLS list) and chmk with the number in r0. On error
 * the carry flag is set and r0 holds errno; convert that to the Linux-style
 * -errno the shared imgact.c wrappers test with `< 0`.
 */
	.globl syscall6
	.hidden syscall6
	.type syscall6,@function
syscall6:
	.word 0x0000			/* entry mask: preserve no registers       */
	movl	4(%ap), %r0		/* r0 = syscall number (first C argument)  */
	movl	$6, 4(%ap)		/* that slot becomes the arg-count word    */
	addl2	$4, %ap			/* AP -> { 6, a, b, c, d, e, f }           */
	chmk	%r0			/* trap: kernel reads args via AP, nr in r0*/
	bcs	1f			/* carry set == error (errno in r0)        */
	ret				/* success: r0 = result                    */
1:	mnegl	%r0, %r0		/* return -errno (matches Linux backends)  */
	ret
	.size syscall6,.-syscall6

/*
 * TLS asm seam — FAIL-STOP stubs (design R4).
 *
 * The shared symbol-vector TLS code (imgact.c symvec_tls_place /
 * absorb_tls_over_crtl) references __tlsdesc_static and imgact_get_tp/set_tp
 * unconditionally, so these symbols must exist to link. The VAX shareable
 * graph is kept _Thread_local-free, so a TLS module is never present, these are
 * never installed as descriptors and never called. Rather than fake or
 * partially implement VAX TLS (which the design forbids — that would be a
 * LARP), each stub FAILS STOP if ever reached: `halt` is privileged, so in user
 * mode it raises a reserved/privileged-instruction fault and the process dies —
 * an honest "VAX TLS is unimplemented", never silent wrong behavior. If a
 * future VAX RTL genuinely needs TLS, that is the operator escalation named in
 * the design (R4), not a change to slip in here.
 */
	.globl __tlsdesc_static
	.hidden __tlsdesc_static
	.type __tlsdesc_static,@function
__tlsdesc_static:
	.word 0x0000
	halt
	.size __tlsdesc_static,.-__tlsdesc_static

	.globl imgact_set_tp
	.hidden imgact_set_tp
	.type imgact_set_tp,@function
imgact_set_tp:
	.word 0x0000
	halt
	.size imgact_set_tp,.-imgact_set_tp

	.globl imgact_get_tp
	.hidden imgact_get_tp
	.type imgact_get_tp,@function
imgact_get_tp:
	.word 0x0000
	halt
	.size imgact_get_tp,.-imgact_get_tp

	.section .note.GNU-stack,"",@progbits
