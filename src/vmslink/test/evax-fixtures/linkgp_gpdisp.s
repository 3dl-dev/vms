	## linkgp_gpdisp.s — EVAX (Alpha/VMS) SHAREABLE fixture for the OVMX-labeled
	## EVAX_R_OVMX_GPDISP relocation round-trip (bead vms-4ed, component C2 of
	## vms-5f5; docs/design-alpha-per-image-gp.md §2.1/§2.2).
	##
	## [OVMX] This is the C2 counterpart of C1's linkgp_two_proc.s. It exports the
	## SAME two procedures (FIRST_PROC, SECOND_PROC) so their PDSCs land at two
	## DIFFERENT offsets in the module's single $LINK$ psect: FIRST_PROC's .pdesc
	## first (K==0, at the linkage-section base) and SECOND_PROC's second (K!=0).
	## Each procedure body invokes the new `.ovmx_gpdisp <proc>` assembler
	## directive, which EMITS a GP-establish ldah/lda immediate pair
	## (ldah $29,0($27); lda $29,0($29)) and marks it with the OVMX-private
	## ETIR__C_OVMX_GPDISP (0xEF01) command. At OVMX link time the
	## linker looks up K for the named procedure (C1's evax_gp_entry table) and
	## patches -K, signed-split, into the pair:
	##   * FIRST_PROC  site: K==0 -> ldah/lda immediates patched to 0 / 0 (no-op)
	##   * SECOND_PROC site: K!=0 -> ldah = HIGH(-K), lda = LOW(-K)
	## $29 is used as the module-GP register purely to prove the patch; register
	## choice (and the whole GP-displacement encoding) is an OVMX detail (§2.1).
	##
	## NOT a VMS-authentic encoding (EVAX publishes no GP-displacement reloc) —
	## labeled OVMX throughout (Rule 8). Regenerate with the PATCHED alpha-dec-vms
	## binutils (tools/cross-alpha-vms/patches/0006-vms-4ed-evax-ovmx-gpdisp.patch):
	##   alpha-dec-vms-as -o linkgp_gpdisp.obj linkgp_gpdisp.s
	.text
	.align 4
	.globl FIRST_PROC
	.ent FIRST_PROC
FIRST_PROC..en:
	.frame $sp, 0, $26, 0
	ldgp $gp, 0($27)
	.prologue
	## [OVMX] GP-establish pair for THIS procedure (FIRST_PROC): K==0 -> 0/0.
	## The .ovmx_gpdisp directive EMITS the ldah/lda pair (ldah $29,0($27);
	## lda $29,0($29)) and marks it with the OVMX GP-displacement relocation.
	## vms-095 (C3) added the explicit module-GP register operand; this reloc-
	## round-trip fixture keeps $29 (the register is content the linker never
	## patches, so the .obj bytes are unchanged; OVMX cc1 itself uses $15).
	.ovmx_gpdisp $29, FIRST_PROC
	bis $31, 1, $0
	ret $31, ($26), 1
	.link
	.align 3
FIRST_PROC:
	.pdesc FIRST_PROC..en, stack
	.end FIRST_PROC

	.text
	.align 4
	.globl SECOND_PROC
	.ent SECOND_PROC
SECOND_PROC..en:
	.frame $sp, 0, $26, 0
	ldgp $gp, 0($27)
	.prologue
	## [OVMX] GP-establish pair for THIS procedure (SECOND_PROC): K!=0 -> -K.
	## The .ovmx_gpdisp directive EMITS the ldah/lda pair and marks it.
	.ovmx_gpdisp $29, SECOND_PROC
	bis $31, 2, $0
	ret $31, ($26), 1
	.link
	.align 3
SECOND_PROC:
	.pdesc SECOND_PROC..en, stack
	.end SECOND_PROC
