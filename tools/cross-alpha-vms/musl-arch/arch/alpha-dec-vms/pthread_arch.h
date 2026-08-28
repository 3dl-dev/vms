/*
 * pthread_arch.h - Alpha thread-pointer access.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * Alpha keeps the per-thread "unique" value in a hidden register read by the
 * PAL_rduniq call-PAL (0x9e), result in $0 (v0). This is the standard Alpha TP
 * mechanism (Alpha Architecture Handbook, PALcode; used by OSF/1, Tru64 and
 * Linux/Alpha alike). GCC also exposes it as __builtin_thread_pointer(); we use
 * the explicit call_pal so the sequence is unambiguous.
 *
 * vms-157: the TLS layout is now exercised (musl's __init_tls runs once the
 * syscall backend + wruniq __set_thread_area are wired). Alpha is TLS variant I
 * (TLS block ABOVE the thread pointer) with a 16-byte TCB / tprel offset 16 --
 * pinned empirically under qemu-alpha and used by the in-tree references
 * (src/libvmssys/vms_runtime_init.c setup_tls __alpha__ branch: data copied to
 * TP+16). So the gap between TP and the TLS segment is 16 bytes, exactly like
 * AArch64 (GAP_ABOVE_TP 16, TP_OFFSET 0). With GAP_ABOVE_TP 0 musl would place
 * the TLS data at TP+0 while the compiler reads it at TP+16 -- a 16-byte skew.
 */

static inline uintptr_t __get_tp(void)
{
	register uintptr_t tp __asm__("$0");
	__asm__ ("call_pal 0x9e" : "=r"(tp)); /* PAL_rduniq -> v0 */
	return tp;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 16

#define MC_PC sc_pc
