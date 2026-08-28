/*
 * img.S -- a minimal hand-crafted VMS-standard Alpha image for the IMGACT
 * VMS-standard activation proof (bead vms-f60d). See run_vms_std_activation_
 * alpha.sh. It stands in for an `alpha-dec-vms` GCC-port `.EXE` until LINK.EXE
 * can emit `.vms$xfer` for Alpha (the declared cross-lane dependency).
 *
 * It is a symbol-vector image (no PT_DYNAMIC): IMGACT reaches
 * activate_symbol_vector, reads `.vms$xfer`, and -- for XFER_FLAVOR == VMS_STD
 * (1) -- standard-calls stub_main (the transfer address) with the six-arg VMS
 * activation context, then routes the returned condition value to $EXIT. For
 * XFER_FLAVOR == SYSV (0) IMGACT tail-jumps to _start instead (today's path).
 *
 * The C preprocessor runs on .S; XFER_FLAVOR selects the two builds.
 */
#ifndef XFER_FLAVOR
#define XFER_FLAVOR 1        /* 1 == OVMX_ACT_VMS_STD, 0 == OVMX_ACT_SYSV */
#endif

	.set noat
	.text
	.globl _start
	.ent _start
_start:
	/* SYSV / legacy tail-jump lands here (today's behavior): IMGACT jumps to
	 * AT_ENTRY with the Linux initial stack. Exit 77 via a raw Linux
	 * exit_group so the test can see the standard-call/$EXIT path was NOT
	 * taken for a SYSV-flavor image. The VMS_STD build never reaches here. */
	lda	$16, 77($31)         /* exit_group arg0 = 77 */
	lda	$0, 405($31)         /* SYS_exit_group (alpha) */
	callsys
	.end _start

	/* stub_main: the transfer address (stands in for the port crt0 __main).
	 * IMGACT standard-calls it with R27 = PV = &main_pdsc, R26 = return into
	 * IMGACT. Return a distinctive ODD condition value (0x0BAD): if IMGACT
	 * ever FAKED the exit as (cond & 1 ? 0 : 1) it would exit 0, so a
	 * fail-honest nonzero exit proves no fabrication (INV-6). */
	.globl stub_main
	.ent stub_main
stub_main:
	.frame $30,0,$26,0
	.prologue 0
	lda	$0, 0x0BAD($31)
	ret	$31, ($26)
	.end stub_main

	.data
	.align 3
	.globl main_pdsc
main_pdsc:
	.quad 0                 /* pdsc[0]: flags (unused)                        */
	.quad stub_main         /* pdsc[1]: entry code addr, biased via .vms$rel  */

	/* .vms$imp: consumer imports. count == 0 (this stub imports nothing);
	 * present because activate_symbol_vector requires a valid header. */
	.section .vms$imp, "aw"
	.align 3
	.long 0x31504d49        /* OVMX_IMP_MAGIC "IMP1" */
	.long 0                 /* count = 0             */
	.long 16                /* names_off             */
	.long 0                 /* names_size            */

	/* .vms$xfer: transfer-address array + activation flavor (vms-f60d). */
	.section .vms$xfer, "aw"
	.align 3
	.long 0x31465358        /* OVMX_XFER_MAGIC "XSF1" */
	.long XFER_FLAVOR       /* flavor: VMS_STD (1) or SYSV (0) */
	.long 1                 /* count = 1 (main transfer address only) */
	.long 0                 /* reserved */
	.quad main_pdsc         /* entry_off[0]: image-relative PDSC offset (unbiased) */

	/* .vms$rel: slots needing +load_bias. Only pdsc[1] (the entry code addr)
	 * is a run-time address; entry_off[0] stays image-relative (IMGACT adds
	 * the base itself). This image links at a fixed vaddr (ET_EXEC), so the
	 * bias is 0 and the values are self-consistent either way. */
	.section .vms$rel, "aw"
	.align 3
	.long 0x314c4552        /* OVMX_REL_MAGIC "REL1" */
	.long 1                 /* count = 1 */
	.quad main_pdsc + 8     /* image-relative offset of the pdsc[1] slot */
