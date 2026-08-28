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
 * CONVENTION (vms-04e, ground-truth): R27 = PV = the PROCEDURE DESCRIPTOR (PDSC),
 * and the trampoline jumps to the entry = *(PV+8).
 *
 * The `.vms$xfer` transfer entry names the procedure VALUE (the address of the
 * procedure descriptor); the entry code address sits at PDSC$Q_ENTRY == PDSC+8.
 * The genuine OVMX `alpha-dec-vms` GCC-port __main is a VMS-native procedure
 * (`.base $27` + `.linkage`/`.pdesc`): its prologue establishes GP with
 * `mov $sp,$gp` and it reaches its OWN linkage pairs GP-relative FROM R27 = the
 * descriptor -- e.g. it calls decc$main via `ldq $ra,48($27); ldq $27,56($27)`
 * where 48($27)/56($27) are that linkage pair's {entry,PDSC} at PDSC+0x30. So
 * R27 MUST hold the descriptor. It NEVER does `ldgp $gp,0($27)`. This matches
 * how crt0 itself calls decc$main (`ldq $27,lk+8` = PDSC), how LINK.EXE fills a
 * LOCAL linkage pair (link.c: quad[0]=entry, quad[1]=PDSC), and how IMGACT fills
 * a CROSS-IMAGE import (quad[1] = PDSC). The whole pipeline is R27 = PDSC.
 *
 * A PRIOR revision (vms-f60d) set R27 = the ENTRY on the mistaken premise that
 * the port emits ELF-Alpha `ldgp $gp,0($27)` prologues. It did not: the real
 * __main derives NO GP from R27 and instead reads its linkage pairs at R27+off.
 * With R27 = entry, __main read its decc$main linkage at entry+0x30 = garbage
 * code, jumped to a wild address -> SIGSEGV before IMGACT printed anything. It
 * was invisible because every stand-in transfer address exercised then
 * (stub_main / the old std_stub_entry) was itself written to that wrong ELF-Alpha
 * convention (`ldgp $gp,0($27)`), so it "passed" against the very bug it hid.
 * This revision restores R27 = PDSC (jumping to *(PV+8) via a scratch register,
 * since R27 and the jsr target register cannot both be $27), so the real
 * VMS-native __main runs. Confirmed end-to-end under qemu-alpha on the joint-e2e
 * image (real port crt0 vs the genuine alpha DECC$SHR).
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
 * condition value to $EXIT), and sets R27 = PV = the PROCEDURE DESCRIPTOR (the
 * pv argument), then jumps to the entry code = *(PV+8) (PDSC$Q_ENTRY) via a
 * scratch register -- so the VMS-native callee reaches its linkage pairs
 * GP-relative from R27 = PDSC. Returns R0, the condition value it produced.
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
	ldq	$1, 8($3)		/* $1 = ENTRY code addr = *(PDSC + 8)       */
					/*   (PDSC$Q_ENTRY) -- the jump TARGET only. */
	mov	$3, $27			/* R27 = PV = the PROCEDURE DESCRIPTOR       */
					/*   (PDSC) address. The genuine alpha-dec-  */
					/*   vms port __main is VMS-native (.base $27*/
					/*   + .linkage/.pdesc): it establishes GP   */
					/*   via `mov $sp,$gp` and reaches its own    */
					/*   linkage pairs GP-relative FROM R27=PDSC  */
					/*   (e.g. `ldq $ra,48($27)` = decc$main lk). */
					/*   So R27 must hold the DESCRIPTOR, exactly */
					/*   as crt0 itself calls decc$main with      */
					/*   `ldq $27,lk+8`=PDSC (and gap-2 fills the */
					/*   import quad[1]=PDSC). R27 and $27 are one */
					/*   register, so the entry rides $1, not $27.*/
	jsr	$26, ($1)		/* STANDARD CALL to the entry; R26 = return- */
					/*   into-caller, R27 = PV = PDSC preserved.  */
	ldgp	$29, 0($26)		/* reload our gp after the call (canonical) */

	ldq	$26, 0($30)		/* restore our return address              */
	lda	$30, 16($30)		/* pop the frame                           */
	ret				/* R0 already holds the condition value    */
	.end   imgact_vms_transfer

/*
 * unsigned long imgact_sv_call(unsigned long pv, unsigned long a0,
 *                              unsigned long a1)
 *   $16 = pv  (PROCEDURE VALUE = the PROCEDURE DESCRIPTOR address; the entry
 *              code address is at PDSC$Q_ENTRY == *(pv+8))
 *   $17 = a0  -> R16 (callee integer arg 0)
 *   $18 = a1  -> R17 (callee integer arg 1)
 * returns R0 (the callee's return value)
 *
 * The PV-PRESERVING standard-call trampoline for IMGACT's OWN bootstrap calls
 * into a symbol-vector procedure (drive_crtl_init -> __init_libc; the exe-main
 * TLS re-drive's __copy_tls / __init_tp / ovmx_get_libc; publish_resident_
 * producers -> imgact_publish_producers). It is imgact_vms_transfer's sibling:
 * SAME R27=PV + jump-to-*(PV+8) contract, but it RETURNS (a `jsr` that comes
 * back here), because these bootstrap procedures return to IMGACT -- unlike the
 * one-way final image transfer that routes the returned condition to $EXIT.
 *
 * Why BOTH halves are mandatory (vms-fe9): the entry deref alone (gap-6,
 * imgact_sv_code_entry) is only half the Alpha Calling Standard. A raw C
 * function-pointer call to the resolved entry sets R27 = the ENTRY, but a
 * VMS-native `alpha-dec-vms` procedure (musl is built to that ABI) reaches its
 * linkage cells GP-relative FROM R27 = the PV = the PDSC -- e.g. __init_libc's
 * first statement `__environ = envp` compiles to `ldq t0,-64(t12)` (t12 == R27)
 * to load &__environ, then `stq a0,0(t0)`. With R27 = entry (not PDSC) that
 * reads raw code bytes as a pointer and the store SIGSEGVs one instruction into
 * the callee. So R27 MUST hold the PV (PDSC). This trampoline sets it, exactly
 * as imgact_vms_transfer does for the final transfer.
 *
 * Args: pv is passed in $16 so it does NOT consume a callee arg register; the
 * two forwarded integer args shift down (a0 -> R16, a1 -> R17). Every current
 * call site takes 0..2 integer/pointer args (ovmx_get_libc()=0; __copy_tls,
 * __init_tp=1; __init_libc, imgact_publish_producers=2), all covered; unused
 * arg registers are simply ignored by the callee. A fixed-arg (non-varargs)
 * callee does not read R25=AI, so it is not set here (a bare call did not set it
 * either, and gap-6 already reached the callee's first statement without it).
 *
 * Clean-room (Rule 8): R16-R21 args, R26=RA, R27=PV=PDSC, PDSC$Q_ENTRY at PV+8
 * are the PUBLIC OpenVMS Alpha Calling Standard. No VSI/HPE source was read.
 */
	.globl imgact_sv_call
	.hidden imgact_sv_call
	.ent   imgact_sv_call
imgact_sv_call:
	ldgp	$29, 0($27)		/* our own gp (normal call: $27 = our PV)  */
	lda	$30, -16($30)		/* allocate a 16-byte-aligned frame        */
	.frame	$30, 16, $26, 0
	stq	$26, 0($30)		/* save our return address                 */
	.prologue 1

	mov	$16, $3			/* $3 = pv = PDSC (save before reshuffle)  */
	ldq	$1, 8($3)		/* $1 = ENTRY = *(PDSC + 8) = PDSC$Q_ENTRY  */
					/*   -- the jump TARGET only.               */
	mov	$17, $16		/* R16 = a0 (read $17 before clobbering it) */
	mov	$18, $17		/* R17 = a1                                */
	mov	$3, $27			/* R27 = PV = the PROCEDURE DESCRIPTOR;     */
					/*   the alpha-dec-vms callee reaches its   */
					/*   linkage cells GP-relative FROM R27.    */
	jsr	$26, ($1)		/* STANDARD CALL to the entry; RETURNS here */
					/*   (R26 = return addr set by jsr).        */
	ldgp	$29, 0($26)		/* reload our gp after the call (canonical) */

	ldq	$26, 0($30)		/* restore our return address              */
	lda	$30, 16($30)		/* pop the frame                           */
	ret				/* R0 = the callee's return value          */
	.end   imgact_sv_call

	.section .note.GNU-stack,"",@progbits
