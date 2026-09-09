/*
 * IMGACT.EXE — VAX (elf32-vax) architecture definitions.
 *
 * rd vms-73b2, epic vms-404. Design contract:
 * docs/design/vax-symbol-vector-imgact.md (§0 "mirror x86_64, NOT Alpha";
 * §1a field-by-field; §1c the elf32 core generalization).
 *
 * Clean-room (CLAUDE.md Rule 8): all VMS-facing semantics come from the OVMX
 * design spec and public VMS behavior only. The ELF-loading structure is the
 * shared imgact.c body (adapted in spirit from musl ldso). The VAX relocation
 * numbers, syscall ABI, and page geometry come from the public elf32-vax ABI
 * (binutils/NetBSD headers) and the NetBSD/vax system-call convention —
 * confirmed empirically with the cross toolchain's readelf/objdump, exactly as
 * the design directs (§1a, §2).
 *
 * VAX is the FIRST NetBSD IMGACT backend (every other targets Linux) and the
 * FIRST elf32 backend. It mirrors x86_64/aarch64 — imgact_arch.h + start.S,
 * SysV tail-jump, symbol-vector binding — and deliberately ships NO
 * vms_transfer.S and does NOT define IMGACT_HAVE_VMS_STD: VAX has no procedure
 * descriptors (the procedure value IS the entry), so an Alpha-style PDSC
 * trampoline would itself be a LARP (design §0/§3).
 */
#ifndef OVMX_IMGACT_ARCH_VAX_H
#define OVMX_IMGACT_ARCH_VAX_H

/* --------------------------------------------------------------------------
 * NetBSD/vax syscall numbers (<sys/syscall.h>) + the freestanding syscall
 * primitive.
 *
 * IMGACT.EXE is -nostdlib; the arch owns the raw kernel entry. On VAX the
 * kernel is entered with `chmk` (change-mode-to-kernel): the syscall number is
 * the chmk operand and the arguments are read by the kernel from the caller's
 * CALLS/CALLG argument list via AP (AP+4 = arg1, sysent supplies the count);
 * on return the carry flag signals an error (errno in r0), else r0 is the
 * result. syscall6() is implemented in start.S (not a static inline) because
 * the AP-relative argument-list shuffle is expressed in assembly; it converts
 * the VAX carry/errno convention into the Linux-style negative-errno return
 * the shared imgact.c wrappers (and the other backends) expect.
 *
 * The generic imgact.c names below map onto NetBSD numbers: imgact.c calls
 * sys_openat/pread64/exit_group by their Linux spelling, so the numbers are
 * remapped here (openat=468, pread=173, exit=1).
 * -------------------------------------------------------------------------- */

#define SYS_openat      468   /* NetBSD openat (AT_FDCWD == -100, as imgact.c) */
#define SYS_close       6
#define SYS_pread64     173   /* NetBSD pread                                  */
#define SYS_write       4
#define SYS_mmap        197
#define SYS_mprotect    74
#define SYS_munmap      73
#define SYS_mincore     78
#define SYS_exit_group  1     /* NetBSD exit                                   */
#define SYS_ioctl       54

/* Implemented in arch/vax/start.S. Returns the syscall result, or -errno on
 * error (VAX carry-set), matching the Linux backends' convention. */
long syscall6(long n, long a, long b, long c, long d, long e, long f);

/* NetBSD/vax mmap(2) primitive (rd vms-33b). Implemented in arch/vax/start.S
 * because the syscall takes EIGHT words — { addr, len, prot, flags, fd, pad,
 * off_lo, off_hi } — with a padding longword before the 64-bit off_t, which
 * syscall6() cannot express. The shared sys_mmap() routes here on VAX. Returns
 * the mapped address, or -errno on error (VAX carry-set). */
void *imgact_vax_mmap(void *addr, unsigned long len, int prot, int flags,
		      int fd, long off);

/* --------------------------------------------------------------------------
 * elf32-vax dynamic relocation types (Elf32 R_TYPE). Values from the public
 * elf32-vax ABI (binutils bfd / NetBSD <sys/elf_machdep.h>), CONFIRMED with
 * `vax--netbsdelf-readelf -r` over -fPIC -shared objects and the sysroot
 * libc.so (which carries the full RELATIVE/GLOB_DAT/JMP_SLOT set). Mapped onto
 * the generic IMGACT_R_* names imgact.c's reloc loop switches on, so the loop
 * stays architecture-independent.
 *
 * elf32-vax uses RELA (a `.rela.dyn`, addend in r_addend) — confirmed — so the
 * shared apply_rela()/self_relocate() paths apply unchanged; R_VAX_RELATIVE is
 * B + A (base + addend), exactly the RELATIVE form those paths already write.
 * -------------------------------------------------------------------------- */

#define R_VAX_32        1    /* S + A (absolute 32-bit)                        */
#define R_VAX_GLOB_DAT  20   /* S     (set GOT entry to symbol value)          */
#define R_VAX_JMP_SLOT  21   /* S     (set PLT GOT entry to symbol value)      */
#define R_VAX_RELATIVE  22   /* B + A (base-relative)                          */

#define IMGACT_R_RELATIVE   R_VAX_RELATIVE
#define IMGACT_R_GLOB_DAT   R_VAX_GLOB_DAT
#define IMGACT_R_JUMP_SLOT  R_VAX_JMP_SLOT
/* On VAX "abs" is a 32-bit word (there is no 64-bit abs reloc); the shared
 * IMGACT_R_ABS64 handler writes res.value+addend through an `unsigned long *`,
 * which is 32-bit on VAX — the correct width for R_VAX_32. */
#define IMGACT_R_ABS64      R_VAX_32

/* NO IMGACT_R_TLSDESC and NO IMGACT_R_TLS_DTPMOD: the VAX shareable graph is
 * kept _Thread_local-free (design R4). See start.S for the fail-stop TLS asm
 * stubs that satisfy the link without implementing (or faking) VAX TLS. */

/* --------------------------------------------------------------------------
 * SysV .hash entry width: 4 bytes (Elf32_Word) — the imgact.c default. VAX does
 * NOT define IMGACT_HASH_XWORD (that is Alpha's 8-byte psABI .hash only).
 *
 * IMGACT_TLS_VARIANT is intentionally left UNDEFINED: the `#if
 * IMGACT_TLS_VARIANT == 1/2` selections in imgact.c then fall to the (harmless)
 * default arithmetic, which reduces to zero because no object ever sets has_tls
 * on the TLS-free VAX graph — assign_tls_offsets()/setup_tls() compute
 * g_tls_total == 0 and setup_tls() returns before it would call the fail-stop
 * imgact_set_tp stub. Do not define IMGACT_TLS_VARIANT without wiring real VAX
 * TLS (design R4 escalation).
 * -------------------------------------------------------------------------- */

/* TCB reserved at/above TP. The shared TLS-offset arithmetic references this
 * unconditionally; on the TLS-free VAX graph it is 0 (no TCB, no TLS). */
#define TLS_TCB_SIZE 0

/* IMGACT_KPAGE (mprotect base-alignment granularity for an arbitrary
 * in-segment address): NetBSD/vax logical page is 4 KiB, matching the generic
 * PAGE_SIZE fallback, so no override is needed here. The segment-level mmap/
 * mprotect alignment the loader relies on is PAGE_SIZE. */

/* Assembly helpers (arch/vax/start.S). */
void _start(void);                 /* ELF/PT_INTERP entry point               */
/* TLS asm seam — declared for the shared symbol-vector TLS code paths that
 * reference them unconditionally (symvec_tls_place/absorb_tls_over_crtl); on
 * VAX they are FAIL-STOP stubs (the graph is TLS-free, so they are never
 * installed or called — see start.S). Not a partial-TLS implementation. */
void __tlsdesc_static(void);
void imgact_set_tp(void *tp);
void *imgact_get_tp(void);

#endif /* OVMX_IMGACT_ARCH_VAX_H */
