/*
 * crt_arch.h - Alpha process entry (_start) for musl-linked programs.
 *
 * OVMX alpha-dec-vms musl port (vms-960, RUNG 1).
 *
 * NOTE ON SCOPE: this _start is for programs linked directly against this musl
 * (the classic Alpha/ELF entry convention: kernel enters at the image entry
 * point with $sp pointing at argc and $27 = the entry PV so gp can be derived
 * via ldgp). The OVMX Alpha GCC *port* images use their own crt0 / IMGACT
 * transfer (the VMS calling standard), NOT this entry; this file exists so the
 * musl static-link path is complete and self-consistent, and is a minimal,
 * documented OSF/Alpha-style _start rather than a VMS-calling-standard one.
 *
 * _start is the raw image entry, not a VMS-callable procedure, so it is a plain
 * global label (no .ent/.pdesc procedure descriptor).
 *
 * Sequence:
 *   ldgp $29,0($27)   establish the global pointer from the entry PV
 *   mov  $30,$16      arg0 = pointer to {argc, argv[], envp[], auxv[]}
 *   mov  $31,$17      arg1 = 0 (static: no _DYNAMIC; _start_c ignores it)
 *   align $sp to 16
 *   jsr  _start_c
 *
 * (Static-link crt only; the VMS/EVAX assembler has no ELF .weak/.hidden, so
 * _DYNAMIC is not referenced here - the dynamic linker is a later rung anyway.)
 */

#define START "_start"

__asm__(
".text \n"
".align 3 \n"
".set noreorder \n"
".global _start \n"
"_start: \n"
"	ldgp $29,0($27) \n"
"	mov $30,$16 \n"
"	mov $31,$17 \n"
"	bic $30,15,$30 \n"
"	jsr $26,_start_c \n"
);
