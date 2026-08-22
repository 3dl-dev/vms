	## absval_ref.s — EVAX (Alpha/VMS) object REFERENCING the absolute global
	## __gcc_main_flags (defined in absval_def.obj) and a normal psect-relative
	## global REFUSER. Each `.quad SYM` against an UNDEFINED external produces a
	## REFQUAD data relocation LINK.EXE resolves at link time:
	##   FLAGSLOT (this object $DATA$+0) -> &__gcc_main_flags : must fold to the
	##       ABSOLUTE constant 3 (no psect base, no load-bias) — the vms-1bc fix.
	##   NORMREF  (this object $DATA$+8) -> &REFUSER          : must resolve to
	##       REFUSER's psect-relative image address (base + value) — unchanged.
	## Regenerate: alpha-dec-vms-as -o absval_ref.obj absval_ref.s   (bead vms-1bc)
	.data
	.globl FLAGSLOT
FLAGSLOT:
	.quad __gcc_main_flags
	.globl NORMREF
NORMREF:
	.quad REFUSER
