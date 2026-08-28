/*
 * std_call_stub_alpha.S -- a stand-in "VMS transfer address" for the
 * imgact_vms_transfer standard-call proof (test/test_std_call_alpha.c, vms-f60d).
 *
 * It plays the role of the port crt0's __main: the trampoline standard-calls it
 * with R16..R21 = the six activation-context args, R25 = AI, R27 = PV, R26 = the
 * return address back into the trampoline.
 *
 * CONVENTION (vms-f60d fix): R27 = the ENTRY code address (PDSC$Q_ENTRY), NOT the
 * descriptor. So -- exactly like a real GCC-compiled __main -- this stub OPENS
 * with `ldgp $29, 0($27)`, establishing its GP *from R27*. It then reaches its
 * capture buffer GP-relative (a normal ELF-Alpha global reference). This is a
 * DELIBERATE regression guard: if the trampoline ever again set R27 = the
 * descriptor (the original SIGSEGV bug), the ldgp here would derive a bogus GP,
 * the GP-relative load of `std_capture` would miss, and the test would fault or
 * mis-capture -- catching what the previous `.prologue 0` stub silently masked.
 *
 * Capture-buffer layout (nine quadwords):
 *   [0..5] = R16..R21   [6] = R25 (AI)   [7] = R27 (PV = entry)   [8] = R26 (RA)
 */
	.set noat
	.text

	.globl std_stub_entry
	.hidden std_stub_entry
	.ent   std_stub_entry
std_stub_entry:
	ldgp	$29, 0($27)		/* ELF-Alpha: GP from R27 = ENTRY addr (the fix) */
	.frame	$30, 0, $26, 0
	.prologue 1
	lda	$1, std_capture		/* GP-relative address of the capture buffer     */
	stq	$16, 0($1)		/* R16 (progxfer)        */
	stq	$17, 8($1)		/* R17 (cli_util)        */
	stq	$18, 16($1)		/* R18 (imghdr)          */
	stq	$19, 24($1)		/* R19 (image_file_desc) */
	stq	$20, 32($1)		/* R20 (linkflag)        */
	stq	$21, 40($1)		/* R21 (cliflag)         */
	stq	$25, 48($1)		/* R25 (AI)              */
	stq	$27, 56($1)		/* R27 (PV = entry addr) */
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
