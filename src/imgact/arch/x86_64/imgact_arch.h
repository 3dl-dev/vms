/*
 * IMGACT.EXE — x86_64 architecture definitions.
 *
 * Part of OVMX (OpenVMS-compatible environment for Linux), bead vms-913.11.
 * Design contract: docs/design-image-activation.md (§2 relocations, §10 TLS).
 *
 * Clean-room: all VMS-facing semantics are derived from the OVMX design spec
 * and public VMS behavior only. The ELF loading structure is adapted from
 * musl libc's ldso/dynlink.c (MIT license) — see imgact.c header comment. The
 * x86_64 relocation numbers and TLS variant come from the public System V
 * x86-64 psABI and Ulrich Drepper's "ELF Handling For Thread-Local Storage".
 */
#ifndef OVMX_IMGACT_ARCH_X86_64_H
#define OVMX_IMGACT_ARCH_X86_64_H

/* --------------------------------------------------------------------------
 * Linux x86_64 syscall numbers and the freestanding syscall primitive.
 *
 * IMGACT.EXE is -nostdlib; the arch owns the raw kernel entry. On x86_64 the
 * `syscall` instruction takes the number in %rax and args in
 * rdi, rsi, rdx, r10, r8, r9 (note: r10, NOT rcx — rcx and r11 are clobbered).
 * -------------------------------------------------------------------------- */

#define SYS_openat      257
#define SYS_close       3
#define SYS_read        0
#define SYS_pread64     17
#define SYS_write       1
#define SYS_mmap        9
#define SYS_mprotect    10
#define SYS_munmap      11
#define SYS_exit_group  231

static inline long syscall6(long n, long a, long b, long c, long d, long e,
			    long f)
{
	long ret;
	register long r10 __asm__("r10") = d;
	register long r8  __asm__("r8")  = e;
	register long r9  __asm__("r9")  = f;
	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(n), "D"(a), "S"(b), "d"(c),
			   "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}

/* --------------------------------------------------------------------------
 * x86_64 dynamic relocation types (Elf64 R_TYPE). Values from the x86-64
 * psABI. Confirmed against `readelf -r` of gcc-built (-mtls-dialect=gnu2)
 * shared objects. Mapped onto the generic IMGACT_R_* names imgact.c switches
 * on, so the relocation loop stays architecture-independent.
 * -------------------------------------------------------------------------- */

#define R_X86_64_64        1    /* S + A (absolute 64-bit)                   */
#define R_X86_64_GLOB_DAT  6    /* S     (set GOT entry to symbol value)     */
#define R_X86_64_JUMP_SLOT 7    /* S     (set PLT GOT entry to symbol value) */
#define R_X86_64_RELATIVE  8    /* B + A (base-relative)                     */
#define R_X86_64_TLSDESC   36   /* TLS descriptor (module offset resolver)   */

#define IMGACT_R_RELATIVE   R_X86_64_RELATIVE
#define IMGACT_R_GLOB_DAT   R_X86_64_GLOB_DAT
#define IMGACT_R_JUMP_SLOT  R_X86_64_JUMP_SLOT
#define IMGACT_R_TLSDESC    R_X86_64_TLSDESC
#define IMGACT_R_ABS64      R_X86_64_64

/* --------------------------------------------------------------------------
 * TLS model.
 *
 * x86_64 uses TLS Variant II (TLS_ABOVE_TP is false): the thread pointer
 * (the %fs segment base) points at the thread control block; the static TLS
 * blocks live at NEGATIVE offsets below it. TP[0] must hold a self-pointer
 * (== TP) — the gnu2/TLSDESC access sequence reads the thread pointer from
 * `%fs:0` and adds the descriptor-returned offset to reach the variable.
 *
 * IMGACT_TLS_VARIANT selects the block-layout math in imgact.c:
 *   1 = Variant I  (aarch64): blocks above TP, offsets positive.
 *   2 = Variant II (x86_64):  blocks below TP, offsets negative, self-pointer.
 * -------------------------------------------------------------------------- */

#define IMGACT_TLS_VARIANT  2
#define TLS_TCB_SIZE        64   /* TCB reserved at/above TP (self-ptr + slack) */

/* Assembly helpers (arch/x86_64/start.S). */
void _start(void);                 /* ELF entry point                        */
void __tlsdesc_static(void);       /* TLSDESC static resolver (%rax in/out)  */
void imgact_set_tp(void *tp);      /* arch_prctl(ARCH_SET_FS, tp)            */
void *imgact_get_tp(void);         /* mov %fs:0, %rax (TCB self-pointer)     */

#endif /* OVMX_IMGACT_ARCH_X86_64_H */
