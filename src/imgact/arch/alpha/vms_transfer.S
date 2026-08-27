/*
 * vms_transfer.S -- IMGACT.EXE Alpha VMS standard-call trampoline (vms-f60d).
 *
 * Part of OVMX (OpenVMS-compatible environment for Linux). The Alpha backend of
 * the VMS-standard image class: it issues a GENUINE OpenVMS Alpha STANDARD CALL
 * to a VMS image's transfer address, so the `alpha-dec-vms` GCC port's crt0
 * (__main -> decc$main -> main, libgcc/config/vms/vms-ucrt0.c) runs. This is the
 * activation-time face of R8 (the Alpha calling standard).
 *
 * Design: docs/design-imgact-vms-activation-context.md.
 *
 * Clean-room (CLAUDE.md Rule 8): the register conventions used here -- integer
 * args in R16-R21 (a0-a5), R25 = AI (Argument Information), R26 = RA (return
 * address), R27 = PV (procedure value) -- are ALL from the PUBLIC OpenVMS Alpha
 * Calling Standard (VSI/HPE). No VSI/HPE source or binary was read.
 *
 * ---------------------------------------------------------------------------
 * FIRST-LIVE-RUN CONVENTION FIX (vms-f60d): R27 = the ENTRY code, not the PDSC.
 *
 * The `.vms$xfer` transfer entry names the procedure VALUE (the address of the
 * procedure descriptor); the entry code address sits at PDSC$Q_ENTRY == PDSC+8.
 * The OVMX `alpha-dec-vms` port, however, is an ELF-Alpha (musl) target: GCC
 * emits the ELF-Alpha linkage, so a real procedure's prologue begins with
 * `ldgp $gp, 0($27)` -- it computes its own GP *from R27*, which therefore MUST
 * hold the ENTRY code address, not the descriptor. (This matches the OVMX EVAX
 * local-call sequence `ldq $27,quad[0]=code-entry; jsr $26,($27)` that LINK.EXE
 * assembles -- see src/vmslink/link.c, evax_add_ximport: R27 is loaded with the
 * code entry, and the callee ldgp's from it.)
 *
 * The original trampoline set R27 = PV = the *descriptor* address and jumped to
 * *(PV+8). Against a real GCC-compiled `__main` that computed GP from R27 = the
 * descriptor, giving a bogus GP; the first GP-relative load (`ldq $27,0($gp)`
 * resolving decc$main) then read garbage and the call jumped to a wild address
 * -> SIGSEGV before IMGACT printed anything. It was invisible until now because
 * every stand-in transfer address exercised so far (stub_main / std_stub_entry)
 * used `.prologue 0` and never touched GP, so R27's value did not matter to them.
 * The fix loads the entry from PDSC+8 straight into R27 and enters there, so a
 * real ELF-Alpha `__main` establishes the correct GP.
 * ---------------------------------------------------------------------------
 *
 * Kept in its own translation unit (separate from start.S's entry/TLS/TP stubs)
 * so it can be unit-tested in isolation under qemu-alpha
 * (test/test_std_call_alpha.c) as well as linked into IMGACT.EXE.
 */
	.set noat
	.text

/*
 * unsigned long imgact_vms_transfer(void *pv, unsigned long ai,
 *                                   const unsigned long args[6])
 *   $16 = pv    (procedure value -> R27; entry code address is at *(pv+8))
 *   $17 = ai    (Argument Information -> R25)
 *   $18 = args  (pointer to six quadwords -> R16..R21)
 *
 * Deliberately the OPPOSITE of _start's tail-jump: it loads the six
 * activation-context arguments into R16..R21, sets R25 = AI by the documented
 * Argument-Information layout, sets R26 = a RETURN address back INTO the caller
 * (so __main/decc$main/main RETURNS here and IMGACT can route the returned VMS
 * condition value to $EXIT), and sets R27 = the ENTRY code address = the
 * quadword at PV+8 (PDSC$Q_ENTRY of the transfer procedure descriptor), so the
 * ELF-Alpha callee's `ldgp $gp,0($27)` prologue establishes the correct GP.
 * Returns R0, the condition value the called procedure produced.
 */
	.globl imgact_vms_transfer
	.hidden imgact_vms_transfer
	.ent   imgact_vms_transfer
imgact_vms_transfer:
	ldgp	$29, 0($27)		/* our own gp (normal call: $27 = our PV)  */
	lda	$30, -16($30)		/* allocate a 16-byte-aligned frame        */
	.frame	$30, 16, $26, 0
	stq	$26, 0($30)		/* save our return address                 */
	.prologue 1

	mov	$16, $3			/* $3 = pv  (target procedure value)       */
	mov	$17, $4			/* $4 = ai  (Argument Information)          */
	mov	$18, $5			/* $5 = args (six-quadword context array)   */

	ldq	$16, 0($5)		/* R16 = args[0]  progxfer                 */
	ldq	$17, 8($5)		/* R17 = args[1]  cli_util                 */
	ldq	$18, 16($5)		/* R18 = args[2]  imghdr                   */
	ldq	$19, 24($5)		/* R19 = args[3]  image_file_desc          */
	ldq	$20, 32($5)		/* R20 = args[4]  linkflag                 */
	ldq	$21, 40($5)		/* R21 = args[5]  cliflag                  */
	mov	$4, $25			/* R25 = AI (argument information)         */
	ldq	$27, 8($3)		/* R27 = PV = ENTRY code addr = *(PDSC + 8) */
					/*   (PDSC$Q_ENTRY). R27 is the ENTRY, not   */
					/*   the descriptor: the ELF-Alpha callee    */
					/*   does ldgp $gp,0($27) from it (see hdr). */
	jsr	$26, ($27)		/* STANDARD CALL; R26 = return-into-caller  */
	ldgp	$29, 0($26)		/* reload our gp after the call (canonical) */

	ldq	$26, 0($30)		/* restore our return address              */
	lda	$30, 16($30)		/* pop the frame                           */
	ret				/* R0 already holds the condition value    */
	.end   imgact_vms_transfer

	.section .note.GNU-stack,"",@progbits
