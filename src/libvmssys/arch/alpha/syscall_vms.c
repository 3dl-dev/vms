/*
 * syscall_vms.c (vms-a7a) — the EVAX/VMS-native Alpha syscall trampolines
 * __vms_syscall0..6 for libvmssys, the alpha-dec-vms (OpenVMS-calling-standard)
 * counterpart of arch/alpha/syscall.S.
 *
 * WHY THIS FILE EXISTS. arch/alpha/syscall.S is written in GNU-ELF Alpha
 * assembler (`.ent`/`.frame`/`.prologue`, `@progbits`, `.hidden`) for the
 * alpha-LINUX-gnu toolchain; the VMS-native alpha-dec-vms assembler (which
 * emits EVAX objects for OVMX shareables) cannot assemble that syntax at all
 * ("junk at end of line", "unknown section attribute", "no entry symbol").
 * When libvmssys is cross-compiled into the VMS-native LIBVMSSYS$SHR (the
 * producer the alpha RMS substrate --use's), the kif transport
 * (kif_transport_linux.c: vms_sys_openat/ioctl/close/mmap over /dev/vms) still
 * needs __vms_syscall0..6, so they must be provided in a form the alpha-dec-vms
 * cc1 accepts. This file is that form — the SAME `callsys` (CALL_PAL 0x83) trap
 * mechanism, expressed as explicit-register inline asm (which the cc1 lowers to
 * a correct OpenVMS-calling-standard procedure), mirroring the PROVEN idiom in
 * tools/cross-alpha-vms/musl-arch/src/internal/vms_alpha_syscall.c (the alpha
 * musl port's live syscall backend, exercised under qemu-alpha, vms-157).
 *
 * SEMANTICS (identical to syscall.S / vms_alpha_syscall.c): number in $0, args
 * a0..a5 in $16..$21, trap `callsys`; $19 (a3) != 0 on return => error and $0
 * holds a POSITIVE errno, which we negate so callers see the negative-errno
 * contract vms_syscall.h's wrappers expect. This backend does NOT fake success
 * (INV-6): a failed syscall returns a genuine negative errno.
 *
 * KNOWN RUNTIME GAP — LLP64 pointer width (tracked as vms-1fc, which BLOCKS the
 * rung-4 runtime proof vms-f49). vms_syscall.h declares these as `long`, and its
 * vms_sys_* wrappers cast pointer arguments through `(long)`. On the
 * alpha-dec-vms LLP64 model `long` is 32 bits while the Linux-Alpha kernel
 * takes full 64-bit register arguments, so a pointer argument is truncated —
 * the same class of bug the musl port fixed by widening its syscall_arg_t to
 * `long long` (syscall_arch.h). Rung 1's deliverable is BUILD + STRICT-LINK
 * only; this trampoline faithfully passes whatever width it is handed and links
 * clean. Making the transport RUN correctly requires widening the alpha
 * vms_syscall.h path to `long long` (arch-isolated: `long long` == `long` on
 * the alpha-linux-gnu LP64 build, so it is safe there too) — filed as vms-1fc,
 * landing with the un-fakeable /dev/vms runtime proof in rung 4 (vms-f49); NOT
 * papered over here.
 */

#if defined(__alpha__)

static long long vms_alpha_callsys(long long n, long long a1, long long a2,
                                   long long a3, long long a4, long long a5,
                                   long long a6)
{
	register long long r0  __asm__("$0")  = n;
	register long long r16 __asm__("$16") = a1;
	register long long r17 __asm__("$17") = a2;
	register long long r18 __asm__("$18") = a3;
	register long long r19 __asm__("$19") = a4;   /* a3 in; error flag out */
	register long long r20 __asm__("$20") = a5;
	register long long r21 __asm__("$21") = a6;

	__asm__ __volatile__(
		"callsys"
		: "+r"(r0), "+r"(r19)
		: "r"(r16), "r"(r17), "r"(r18), "r"(r20), "r"(r21)
		: "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8",
		  "$22", "$23", "$24", "$25", "$27", "$28", "memory");

	if (r19 != 0)
		return -r0;          /* out-of-band error: v0 holds +errno */
	return r0;
}

/* The __vms_syscallN signatures MATCH vms_syscall.h's `extern long` prototypes
 * so the declaration and definition agree at link. Each shifts nr -> $0 and
 * a1..aN -> $16.. and traps. (The LLP64 width of `long` here is the documented
 * rung-4 gap above; the trampoline itself is faithful.) */
long __vms_syscall0(long nr)
{ return (long)vms_alpha_callsys(nr, 0, 0, 0, 0, 0, 0); }

long __vms_syscall1(long nr, long a1)
{ return (long)vms_alpha_callsys(nr, a1, 0, 0, 0, 0, 0); }

long __vms_syscall2(long nr, long a1, long a2)
{ return (long)vms_alpha_callsys(nr, a1, a2, 0, 0, 0, 0); }

long __vms_syscall3(long nr, long a1, long a2, long a3)
{ return (long)vms_alpha_callsys(nr, a1, a2, a3, 0, 0, 0); }

long __vms_syscall4(long nr, long a1, long a2, long a3, long a4)
{ return (long)vms_alpha_callsys(nr, a1, a2, a3, a4, 0, 0); }

long __vms_syscall5(long nr, long a1, long a2, long a3, long a4, long a5)
{ return (long)vms_alpha_callsys(nr, a1, a2, a3, a4, a5, 0); }

long __vms_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{ return (long)vms_alpha_callsys(nr, a1, a2, a3, a4, a5, a6); }

#endif /* __alpha__ */
