/*
 * std_call_stub_alpha.S -- a stand-in "VMS transfer address" for the
 * imgact_vms_transfer standard-call proof (test/test_std_call_alpha.c, vms-f60d).
 *
 * It plays the role of the port crt0's __main: the trampoline standard-calls it
 * with R16..R21 = the six activation-context args, R25 = AI, R27 = PV (the
 * procedure descriptor), R26 = the return address back into the trampoline.
 *
 * It records everything it received so the harness can prove the call was built
 * correctly, then returns a distinctive VMS-style condition value in R0.
 *
 * It locates its capture buffer via R27 (= PV = the PDSC), reading the pointer
 * the harness stored at PDSC+16 -- so it needs NO gp of its own, exactly
 * exercising the "PV = procedure descriptor, entry code at PV+8" convention the
 * trampoline implements. Capture-buffer layout (nine quadwords):
 *   [0..5] = R16..R21   [6] = R25 (AI)   [7] = R27 (PV)   [8] = R26 (RA)
 */
	.set noat
	.text

	.globl std_stub_entry
	.hidden std_stub_entry
	.ent   std_stub_entry
std_stub_entry:
	.frame $30, 0, $26, 0
	.prologue 0
	ldq	$1, 16($27)		/* $1 = &capture (from PDSC+16); no gp used */
	stq	$16, 0($1)		/* R16 (progxfer)        */
	stq	$17, 8($1)		/* R17 (cli_util)        */
	stq	$18, 16($1)		/* R18 (imghdr)          */
	stq	$19, 24($1)		/* R19 (image_file_desc) */
	stq	$20, 32($1)		/* R20 (linkflag)        */
	stq	$21, 40($1)		/* R21 (cliflag)         */
	stq	$25, 48($1)		/* R25 (AI)              */
	stq	$27, 56($1)		/* R27 (PV)              */
	stq	$26, 64($1)		/* R26 (RA into caller)  */
	lda	$0, 0x0BAD($31)		/* return a distinctive odd condition value */
	ret	$31, ($26)		/* return to the trampoline via RA          */
	.end   std_stub_entry

	.section .note.GNU-stack,"",@progbits
