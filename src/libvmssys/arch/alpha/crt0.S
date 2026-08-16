/*
 * crt0.S - Custom _start entry point for Alpha (no glibc)
 *
 * At process entry the kernel leaves the stack as:
 *   0($sp):   argc
 *   8($sp):   argv[0] ... argv[argc-1], NULL
 *   then:     envp[0] ... envp[n], NULL
 *   then:     auxv pairs ... AT_NULL
 *
 * Alpha calling convention: integer args in $16..$21, return addr in
 * $26, gp in $29.  Unlike x86_64/aarch64, $29 (gp) is NOT valid at the
 * ELF entry point, so we establish it ourselves before any call that
 * may reference global data.  Stack is 16-byte aligned at entry and we
 * add no frame, so no realignment is needed.
 *
 * We call:
 *   __vms_runtime_init(argc, argv, envp)
 *   main(argc, argv, envp)
 *   exit_group(main_ret)      -- __NR_exit_group == 405 on Alpha
 */
	.set noat
	.text
	.globl _start
	.ent   _start
_start:
	.frame $30, 0, $26, 0
	.prologue 0

	/* Establish gp ($29) without relying on $27/pv at kernel entry. */
	br	$29, 1f
1:	ldgp	$29, 0($29)

	/* Load argc / argv / envp from the initial stack. */
	ldq	$16, 0($30)		/* argc  = *sp            */
	lda	$17, 8($30)		/* argv  = sp + 8         */
	addq	$16, 1, $18		/* argc + 1               */
	s8addq	$18, $17, $18		/* envp  = argv + 8*(argc+1) */

	/* Preserve argc/argv/envp across the runtime-init call. */
	mov	$16, $9
	mov	$17, $10
	mov	$18, $11

	/* __vms_runtime_init(argc, argv, envp) */
	jsr	$26, __vms_runtime_init
	ldgp	$29, 0($26)		/* reload gp after the call */

	/* main(argc, argv, envp) */
	mov	$9,  $16
	mov	$10, $17
	mov	$11, $18
	jsr	$26, main
	ldgp	$29, 0($26)

	/* exit_group(main_ret) -- main's return value is in $0. */
	mov	$0, $16			/* status */
	lda	$0, 405($31)		/* __NR_exit_group */
	callsys

	/* Unreachable. */
	call_pal 0x80			/* bpt -- trap if we ever get here */
	.end   _start

	.section .note.GNU-stack,"",@progbits
