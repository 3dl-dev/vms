/*
 * std_call_stub_alpha.S -- a stand-in "VMS transfer address" for the
 * imgact_vms_transfer standard-call proof (test/test_std_call_alpha.c, vms-f60d;
 * convention corrected by vms-04e).
 *
 * It plays the role of the port crt0's __main: the trampoline standard-calls it
 * with R16..R21 = the six activation-context args, R25 = AI, R27 = PV, R26 = the
 * return address back into the trampoline.
 *
 * CONVENTION (vms-04e, ground-truth from the real alpha-dec-vms port __main):
 * R27 = PV = the PROCEDURE DESCRIPTOR address (PDSC), NOT the entry. The genuine
 * alpha-dec-vms GCC-port __main is a VMS-native procedure (`.base $27` +
 * `.linkage`/`.pdesc`): it establishes its GP with `mov $sp,$gp` and reaches its
 * OWN linkage pairs GP-relative FROM R27 = the descriptor (e.g. `ldq $ra,48($27)`
 * = decc$main's linkage) -- so R27 MUST hold the descriptor, exactly as crt0
 * itself calls decc$main (`ldq $27,lk+8` = PDSC) and as LINK.EXE fills a local
 * linkage pair (quad[1] = PDSC) and IMGACT fills a cross-image import
 * (quad[1] = PDSC). It does NOT `ldgp $gp,0($27)`.
 *
 * This stub therefore models the real convention faithfully: it CAPTURES the
 * delivered R27 (which the test asserts == the descriptor address), and it
 * establishes its own GP SELF-relatively (the `br $27,1f; ldgp` idiom), NOT from
 * the delivered R27. This is a DELIBERATE regression guard: if the trampoline
 * ever again set R27 = the entry (the vms-f60d convention that SIGSEGV'd the real
 * crt0, because __main then read its linkage pairs at entry+48 = garbage), the
 * captured R27 would be the entry and the test's `R27 == descriptor` CHECK fails.
 *
 * Capture-buffer layout (nine quadwords):
 *   [0..5] = R16..R21   [6] = R25 (AI)   [7] = R27 (PV = descriptor)   [8] = R26 (RA)
 */
	.set noat
	.text

	.globl std_stub_entry
	.hidden std_stub_entry
	.ent   std_stub_entry
std_stub_entry:
	mov	$27, $2			/* $2 = R27 as DELIVERED (must == descriptor)    */
	br	$27, 1f			/* $27 = PC of 1f (self); branch there            */
1:	ldgp	$29, 0($27)		/* GP established SELF-relatively, NOT from the   */
					/*   delivered R27 (the real __main sets GP via  */
					/*   `mov $sp,$gp`; it never ldgp's from R27).   */
	.frame	$30, 0, $26, 0
	.prologue 1
	lda	$1, std_capture		/* GP-relative address of the capture buffer     */
	stq	$16, 0($1)		/* R16 (progxfer)        */
	stq	$17, 8($1)		/* R17 (cli_util)        */
	stq	$18, 16($1)		/* R18 (imghdr)          */
	stq	$19, 24($1)		/* R19 (image_file_desc) */
	stq	$20, 32($1)		/* R20 (linkflag)        */
	stq	$21, 40($1)		/* R21 (cliflag)         */
	stq	$25, 48($1)		/* R25 (AI)                       */
	stq	$2,  56($1)		/* R27 as delivered (PV = descriptor) */
	stq	$26, 64($1)		/* R26 (RA into caller)  */
	lda	$0, 0x0BAD($31)		/* return a distinctive odd condition value */
	ret	$31, ($26)		/* return to the trampoline via RA          */
	.end   std_stub_entry

	.data
	.align 3
	.globl std_capture
std_capture:
	.zero 72			/* nine quadwords, filled by std_stub_entry */

	.section .note.GNU-stack,"",@progbits
