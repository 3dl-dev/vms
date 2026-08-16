/*
 * IMGACT.EXE — Alpha (AXP) architecture definitions.
 *
 * Part of OVMX (OpenVMS-compatible environment for Linux), bead vms-e11
 * (Alpha[A2], epic vms-8954). Design contract: docs/design-image-activation.md.
 *
 * Clean-room: all VMS-facing semantics are derived from the OVMX design spec
 * and public VMS behavior only. The ELF loading structure is adapted from
 * musl libc's ldso/dynlink.c (MIT license) — see imgact.c header comment.
 * The Alpha syscall convention, relocation numbers, and TLS model come from
 * the public Linux/Alpha ABI (glibc sysdeps/alpha, binutils bfd/elf64-alpha.c)
 * and were confirmed empirically against `alpha-linux-gnu-readelf -r` output
 * of gcc-built objects and `alpha-linux-gnu-gcc`'s own <elf.h> — see
 * src/imgact/test/run_test_alpha.sh for the oracle commands.
 */
#ifndef OVMX_IMGACT_ARCH_ALPHA_H
#define OVMX_IMGACT_ARCH_ALPHA_H

/* --------------------------------------------------------------------------
 * Linux/Alpha syscall numbers and the freestanding syscall primitive.
 *
 * IMGACT.EXE is -nostdlib; the arch owns the raw kernel entry. Alpha's
 * `callsys` (PALcode call_pal 0x83) takes the syscall number in $0 (v0) and
 * up to 6 arguments in $16..$21 (a0-a5); the SEVENTH Alpha ABI wrinkle vs.
 * x86_64/aarch64 is the OUT-OF-BAND error signal: on return, $19 (a3) != 0
 * means $0 holds a POSITIVE errno, not the negative-errno-in-return-value
 * both other backends use. This is the exact convention already proven by
 * src/libvmssys/arch/alpha/syscall.S (bead vms-a090); syscall6() below
 * normalizes it the same way so imgact.c's `sys_*` wrappers (which all test
 * `< 0`) work unchanged.
 * -------------------------------------------------------------------------- */

#define SYS_openat      450
#define SYS_close       6
#define SYS_read        3
#define SYS_pread64     349
#define SYS_write       4
#define SYS_mmap        71
#define SYS_mprotect    74
#define SYS_munmap      73
#define SYS_exit_group  405

static inline long syscall6(long n, long a, long b, long c, long d, long e,
			    long f)
{
	register long v0 __asm__("$0")  = n;
	register long a0 __asm__("$16") = a;
	register long a1 __asm__("$17") = b;
	register long a2 __asm__("$18") = c;
	register long a3 __asm__("$19") = d;
	register long a4 __asm__("$20") = e;
	register long a5 __asm__("$21") = f;
	__asm__ volatile("callsys"
			 : "+r"(v0), "+r"(a3)
			 : "r"(a0), "r"(a1), "r"(a2), "r"(a4), "r"(a5)
			 : "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8",
			   "$22", "$23", "$24", "$25", "$27", "$28", "memory");
	return a3 ? -v0 : v0;
}

/* --------------------------------------------------------------------------
 * Alpha dynamic relocation types (Elf64 R_TYPE). Values from binutils'
 * bfd/elf64-alpha.c / glibc's <elf.h>, confirmed against
 * `alpha-linux-gnu-readelf -r` of gcc-built (-fPIC -shared) shared objects
 * (see run_test_alpha.sh). Mapped onto the generic IMGACT_R_* names imgact.c
 * switches on, so the relocation loop stays architecture-independent.
 *
 * Alpha has NO TLSDESC (GCC alpha rejects -mtls-dialect=desc/gnu2 — that is
 * an ARM/AArch64/x86/RISC-V extension). Its only TLS access model is the
 * traditional General Dynamic model: a JMP_SLOT-resolved call to
 * `__tls_get_addr(&tls_index)`, with the two-word tls_index filled by
 * DTPMOD64 (runtime module id) + DTPREL64 (module-relative offset)
 * relocations. IMGACT_R_TLSDESC is therefore deliberately NOT defined here
 * (imgact.c guards that case on #ifdef); IMGACT_R_TLS_DTPMOD/DTPREL select
 * the GD-model case block imgact.c adds instead.
 * -------------------------------------------------------------------------- */

#define R_ALPHA_REFQUAD   2    /* S + A (absolute 64-bit)                   */
#define R_ALPHA_GLOB_DAT  25   /* S     (set GOT entry to symbol value)     */
#define R_ALPHA_JMP_SLOT  26   /* S     (set PLT GOT entry to symbol value) */
#define R_ALPHA_RELATIVE  27   /* B + A (base-relative)                     */
#define R_ALPHA_DTPMOD64  31   /* TLS General Dynamic: runtime module id    */
#define R_ALPHA_DTPREL64  33   /* TLS General Dynamic: module-relative off  */

/* Alpha's ELF ABI (binutils' Alpha psABI, predating the gABI convention that
 * later 64-bit ports settled on) stores the SysV .hash section's fields as
 * 8-byte Elf64_Xword, not the 4-byte Elf32_Word every other architecture
 * (including aarch64/x86_64) uses -- confirmed both by `readelf -S` (this
 * section's sh_entsize == 8) and empirically: reading it with 4-byte
 * arithmetic produced a garbage bucket-chain index and a genuine SIGSEGV out
 * of symtab[] bounds under qemu-alpha. imgact.c's obj_find() is generalized
 * on this macro so x86_64/aarch64 (where it stays undefined) are unaffected.
 */
#define IMGACT_HASH_XWORD 1

#define IMGACT_R_RELATIVE    R_ALPHA_RELATIVE
#define IMGACT_R_GLOB_DAT    R_ALPHA_GLOB_DAT
#define IMGACT_R_JUMP_SLOT   R_ALPHA_JMP_SLOT
#define IMGACT_R_ABS64       R_ALPHA_REFQUAD
#define IMGACT_R_TLS_DTPMOD  R_ALPHA_DTPMOD64
#define IMGACT_R_TLS_DTPREL  R_ALPHA_DTPREL64
/* (no IMGACT_R_TLSDESC on Alpha — see comment above) */

/* --------------------------------------------------------------------------
 * TLS block layout.
 *
 * The DTPMOD/DTPREL General Dynamic model doesn't care how blocks sit
 * relative to the thread pointer (unlike TLSDESC's fixed access sequence):
 * __tls_get_addr() computes the address directly from IMGACT's own module
 * table, so any of the existing IMGACT_TLS_VARIANT layouts works. Reuse
 * Variant I (aarch64's: blocks above TP, positive offsets) — Alpha's thread
 * pointer (the PALcode "unique" value, read/written via `rduniq`/`wrunique`,
 * i.e. call_pal 0x9e/0x9f) carries no self-pointer or TCB convention of its
 * own (that was an x86_64 TLSDESC-ABI requirement), so no TCB reservation is
 * needed either.
 * -------------------------------------------------------------------------- */

#define IMGACT_TLS_VARIANT  1
#define TLS_TCB_SIZE        0

/* Assembly helpers (arch/alpha/start.S). */
void _start(void);                 /* ELF entry point                        */
void __tlsdesc_static(void);       /* OVMX-native symbol-vector TLS resolver
				     * (arg in $16, returns in $0) — NOT the
				     * ELF TLSDESC ABI, which Alpha has no
				     * hardware/compiler support for. Used only
				     * by the .vms$imp/.vms$tls symbol-vector
				     * activation path (imgact.c), not by the
				     * ELF DT_HASH GD-model relocations above. */
void imgact_set_tp(void *tp);      /* call_pal 0x9f (wrunique)                */
void *imgact_get_tp(void);         /* call_pal 0x9e (rduniq)                  */

#endif /* OVMX_IMGACT_ARCH_ALPHA_H */
