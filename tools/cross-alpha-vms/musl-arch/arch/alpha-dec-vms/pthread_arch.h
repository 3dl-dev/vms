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
 * RUNG-1 note: the TLS layout constants below are provisional. No thread is
 * created and no TLS block is set up at rung 1 (thread creation needs the
 * executive backend, GAP3), so these are compiled but never exercised; they
 * will be confirmed against the OVMX Alpha loader/executive at a later rung.
 * Alpha is TLS variant I (TLS block above the thread pointer).
 */

static inline uintptr_t __get_tp(void)
{
	register uintptr_t tp __asm__("$0");
	__asm__ ("call_pal 0x9e" : "=r"(tp)); /* PAL_rduniq -> v0 */
	return tp;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 0

#define MC_PC sc_pc
