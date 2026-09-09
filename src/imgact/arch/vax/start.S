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
 * void *imgact_vax_mmap(void *addr, unsigned long len, int prot, int flags,
 *                       int fd, long off)   —   NetBSD/vax mmap(2) (rd vms-33b)
 *
 * The NetBSD/vax mmap syscall takes EIGHT argument words, not six: off_t is
 * 64-bit on 32-bit VAX (confirmed: sizeof(off_t)==8) and, following the classic
 * BSD mmap ABI, a padding longword precedes the 64-bit offset. The kernel
 * argument list the trap reads is therefore:
 *     { addr, len, prot, flags, fd, pad, off_lo, off_hi }
 * This layout was ground-truthed against the sysroot libc's own mmap stub
 * (vax--netbsdelf objdump of libc.a(mmap.o) <_mmap>): it pushes addr; the
 * {len,prot} pair; the {flags,fd} pair; a pushl $0 pad; then the 64-bit offset
 * quadword, and issues CALLS $8 — i.e. exactly the eight words above.
 *
 * The shared imgact.c sys_mmap() cannot express this through syscall6() (six
 * slots, and it would leave the kernel reading off_lo/off_hi from uninitialized
 * stack — a real bug for the one MAP_SHARED file mapping IMGACT makes), so the
 * VAX backend routes mmap here. IMGACT only ever maps at non-negative offsets
 * well under 2^31 (image-file byte offsets), so the 32-bit C `off` zero-extends
 * into off_lo with off_hi = 0. chmk/carry/-errno convention matches syscall6.
 *
 * All 4(%ap)..0x18(%ap) reads happen before %ap is repointed at the freshly
 * built list; RET restores the caller's SP/AP/FP from the CALLS frame, so
 * clobbering them here is safe (this is an ordinary arg-pushing procedure).
 */
	.globl imgact_vax_mmap
	.hidden imgact_vax_mmap
	.type imgact_vax_mmap,@function
imgact_vax_mmap:
	.word 0x0000			/* entry mask: preserve no registers       */
	clrl	-(%sp)			/* off_hi = 0 (offset is non-negative)     */
	pushl	0x18(%ap)		/* off_lo = off                            */
	clrl	-(%sp)			/* pad = 0                                 */
	pushl	0x14(%ap)		/* fd                                      */
	pushl	0x10(%ap)		/* flags                                   */
	pushl	0x0c(%ap)		/* prot                                    */
	pushl	0x08(%ap)		/* len                                     */
	pushl	0x04(%ap)		/* addr                                    */
	pushl	$8			/* kernel argument count word              */
	movl	%sp, %ap		/* %ap -> { 8, addr,len,prot,flags,fd,     */
					/*          pad, off_lo, off_hi }          */
	movl	$197, %r0		/* SYS_mmap                                */
	chmk	%r0			/* trap: kernel reads 8 args via %ap       */
	bcs	2f			/* carry set == error (errno in r0)        */
	ret				/* success: r0 = mapped address            */
2:	mnegl	%r0, %r0		/* return -errno                           */
	ret
	.size imgact_vax_mmap,.-imgact_vax_mmap

/*
 * long __udiv(unsigned long a, unsigned long b)   ->  a / b   (unsigned)
 * long __urem(unsigned long a, unsigned long b)   ->  a % b   (unsigned)
 *
 * VAX GCC emits calls to __udiv/__urem for unsigned 32-bit `/` and `%`: the VAX
 * has no unsigned-divide instruction (DIVL2/DIVL3 are SIGNED; EDIV divides a
 * SIGNED 64-bit dividend by a SIGNED 32-bit divisor). On NetBSD/vax these two
 * helpers live in libc, but IMGACT.EXE is -nostdlib and links against nothing,
 * so it provides them here (rd vms-33b). They are written to the public VAX
 * EDIV semantics — the canonical unsigned-divide-via-EDIV method — with the
 * standard CALLS convention (args at 4(%ap)/8(%ap), result in r0) the sysroot
 * libc stubs also use, confirmed by objdump of libc.a(udiv.o)/(urem.o).
 *
 * The three-way split is forced by EDIV's SIGNED interpretation of both
 * operands:
 *   divisor >= 2^31 (sign bit set): a/b is 0 or 1, a%b is a or a-b.
 *   dividend < 2^31:                DIVL (signed) already gives the right answer.
 *   dividend >= 2^31, divisor < 2^31: zero-extend the dividend to the quadword
 *                                     {0:a} and EDIV by the positive divisor.
 * Division by zero faults exactly as a hardware divide would (IMGACT never does).
 */
	.globl __udiv
	.hidden __udiv
	.type __udiv,@function
__udiv:
	.word 0x0000
	movl	0x08(%ap), %r2		/* r2 = divisor b                          */
	blss	4f			/* b >= 2^31 -> big-divisor case           */
	movl	0x04(%ap), %r0		/* r0 = dividend a                         */
	blss	3f			/* a >= 2^31 -> 64-bit EDIV case           */
	divl2	%r2, %r0		/* a,b both < 2^31: signed DIVL == unsigned*/
	ret
3:	clrl	%r1			/* {r1:r0} = {0:a} (zero-extend to 64-bit) */
	ediv	%r2, %r0, %r0, %r1	/* {r1:r0}/r2 -> quotient r0, remainder r1 */
	ret
4:	clrl	%r0			/* b >= 2^31: quotient is 0 ...            */
	cmpl	0x04(%ap), %r2		/* ... unless a >= b (unsigned)            */
	blssu	5f			/* a < b (unsigned) -> quotient 0          */
	incl	%r0			/* a >= b -> quotient 1                    */
5:	ret
	.size __udiv,.-__udiv

	.globl __urem
	.hidden __urem
	.type __urem,@function
__urem:
	.word 0x0000
	movl	0x08(%ap), %r2		/* r2 = divisor b                          */
	blss	7f			/* b >= 2^31 -> big-divisor case           */
	movl	0x04(%ap), %r0		/* r0 = dividend a                         */
	blss	6f			/* a >= 2^31 -> 64-bit EDIV case           */
	divl3	%r2, %r0, %r1		/* r1 = a / b (signed == unsigned here)    */
	mull2	%r2, %r1		/* r1 = (a / b) * b                        */
	subl2	%r1, %r0		/* r0 = a - (a / b) * b = a % b            */
	ret
6:	clrl	%r1			/* {r1:r0} = {0:a}                         */
	ediv	%r2, %r0, %r1, %r0	/* quotient -> r1 (unused), remainder -> r0*/
	ret
7:	movl	0x04(%ap), %r0		/* r0 = a                                  */
	cmpl	%r0, %r2		/* b >= 2^31: rem = a if a < b (unsigned)  */
	blssu	8f			/* a < b -> remainder a                    */
	subl2	%r2, %r0		/* a >= b -> remainder a - b               */
8:	ret
	.size __urem,.-__urem

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
