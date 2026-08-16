/*
 * syscall.S - Raw Linux syscall trampoline for Alpha (alpha-linux-gnu)
 *
 * Alpha/Linux syscall convention:
 *   syscall number: $0  (v0)
 *   arguments:      $16, $17, $18, $19, $20, $21   (a0-a5)
 *   instruction:    callsys   (call_pal 0x83)
 *   return value:   $0  (v0)
 *   error flag:     $19 (a3) != 0  =>  $0 holds a POSITIVE errno
 *
 * This is the one place Alpha diverges from the x86_64/aarch64 backends:
 * those architectures return the raw kernel value in negative-errno form
 * (ret < 0 == -errno), and the entire C layer (vms_syscall.h) is written
 * to that contract.  Alpha instead signals error out-of-band in a3 and
 * leaves a *positive* errno in v0.  So on error we negate v0, converting
 * to the negative-errno convention the C layer expects.  With that one
 * normalization, every vms_sys_* wrapper works unchanged.
 *
 * C ABI (Alpha): first six integer args in $16..$21, a 7th at 0($sp).
 * Our trampoline receives (nr, a1, a2, ...) so we move nr -> $0 and
 * shift a1..aN down into the kernel arg registers $16..$21.
 */
	.set noat
	.text

/* --- syscall0(nr) --------------------------------------------------- */
	.globl __vms_syscall0
	.ent   __vms_syscall0
__vms_syscall0:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0		/* v0 = -errno */
	ret
	.end   __vms_syscall0

/* --- syscall1(nr, a1) ----------------------------------------------- */
	.globl __vms_syscall1
	.ent   __vms_syscall1
__vms_syscall1:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	mov	$17, $16
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0
	ret
	.end   __vms_syscall1

/* --- syscall2(nr, a1, a2) ------------------------------------------- */
	.globl __vms_syscall2
	.ent   __vms_syscall2
__vms_syscall2:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	mov	$17, $16
	mov	$18, $17
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0
	ret
	.end   __vms_syscall2

/* --- syscall3(nr, a1, a2, a3) --------------------------------------- */
	.globl __vms_syscall3
	.ent   __vms_syscall3
__vms_syscall3:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	mov	$17, $16
	mov	$18, $17
	mov	$19, $18
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0
	ret
	.end   __vms_syscall3

/* --- syscall4(nr, a1, a2, a3, a4) ----------------------------------- */
	.globl __vms_syscall4
	.ent   __vms_syscall4
__vms_syscall4:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	mov	$17, $16
	mov	$18, $17
	mov	$19, $18
	mov	$20, $19
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0
	ret
	.end   __vms_syscall4

/* --- syscall5(nr, a1, a2, a3, a4, a5) ------------------------------- */
	.globl __vms_syscall5
	.ent   __vms_syscall5
__vms_syscall5:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	mov	$17, $16
	mov	$18, $17
	mov	$19, $18
	mov	$20, $19
	mov	$21, $20
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0
	ret
	.end   __vms_syscall5

/* --- syscall6(nr, a1, a2, a3, a4, a5, a6) --------------------------- */
/* a6 is the 7th incoming arg: passed at 0($sp) per the Alpha C ABI.    */
	.globl __vms_syscall6
	.ent   __vms_syscall6
__vms_syscall6:
	.frame $30, 0, $26, 0
	.prologue 0
	mov	$16, $0
	mov	$17, $16
	mov	$18, $17
	mov	$19, $18
	mov	$20, $19
	mov	$21, $20
	ldq	$21, 0($30)		/* a6 from stack -> kernel a5 */
	callsys
	bne	$19, 9f
	ret
9:	subq	$31, $0, $0
	ret
	.end   __vms_syscall6

	.section .note.GNU-stack,"",@progbits
