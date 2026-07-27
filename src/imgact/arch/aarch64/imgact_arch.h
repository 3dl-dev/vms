/*
 * IMGACT.EXE — aarch64 architecture definitions.
 *
 * Part of OVMX (OpenVMS-compatible environment for Linux), bead vms-913.2.
 * Design contract: docs/design-image-activation.md.
 *
 * Clean-room: all VMS-facing semantics are derived from the OVMX design spec
 * and public VMS behavior only. The ELF loading structure is adapted from
 * musl libc's ldso/dynlink.c (MIT license) — see imgact.c header comment.
 */
#ifndef OVMX_IMGACT_ARCH_AARCH64_H
#define OVMX_IMGACT_ARCH_AARCH64_H

/* --------------------------------------------------------------------------
 * Linux aarch64 syscall numbers and the freestanding syscall primitive.
 *
 * IMGACT.EXE is -nostdlib; the arch owns the raw kernel entry. On aarch64 the
 * `svc 0` instruction takes the number in x8 and args in x0..x5.
 * -------------------------------------------------------------------------- */

#define SYS_openat      56
#define SYS_close       57
#define SYS_read        63
#define SYS_pread64     67
#define SYS_write       64
#define SYS_mmap        222
#define SYS_mprotect    226
#define SYS_munmap      215
#define SYS_exit_group  94

static inline long syscall6(long n, long a, long b, long c, long d, long e,
			    long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc 0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
			 : "memory", "cc");
	return x0;
}

/*
 * AArch64 dynamic relocation types (Elf64 R_TYPE).
 * Values from the AArch64 ELF ABI (public documentation). Confirmed against
 * `readelf -r` output of musl-gcc built shared objects.
 */
#define R_AARCH64_ABS64        257
#define R_AARCH64_GLOB_DAT     1025
#define R_AARCH64_JUMP_SLOT    1026
#define R_AARCH64_RELATIVE     1027
#define R_AARCH64_TLS_DTPMOD   1028
#define R_AARCH64_TLS_DTPREL   1029
#define R_AARCH64_TLS_TPREL    1030
#define R_AARCH64_TLSDESC      1031

/* Generic relocation names imgact.c switches on (architecture-independent). */
#define IMGACT_R_RELATIVE   R_AARCH64_RELATIVE
#define IMGACT_R_GLOB_DAT   R_AARCH64_GLOB_DAT
#define IMGACT_R_JUMP_SLOT  R_AARCH64_JUMP_SLOT
#define IMGACT_R_TLSDESC    R_AARCH64_TLSDESC
#define IMGACT_R_ABS64      R_AARCH64_ABS64

/*
 * AArch64 uses TLS Variant I (TLS_ABOVE_TP): the thread pointer (TPIDR_EL0)
 * points at the thread control block; TLS blocks live at positive offsets
 * above it, after a reserved 2-word (16 byte) TCB area.
 *
 * IMGACT_TLS_VARIANT selects the block-layout math in imgact.c:
 *   1 = Variant I  (aarch64): blocks above TP, offsets positive.
 *   2 = Variant II (x86_64):  blocks below TP, offsets negative, self-pointer.
 */
#define IMGACT_TLS_VARIANT  1
#define TLS_TCB_SIZE        16

/* Assembly helpers (arch/aarch64/start.S). */
void _start(void);                 /* ELF entry point */
void __tlsdesc_static(void);       /* TLSDESC static resolver */
void imgact_set_tp(void *tp);      /* msr tpidr_el0, x0 */

#endif /* OVMX_IMGACT_ARCH_AARCH64_H */
