/*
 * __set_thread_area.c - Alpha thread-pointer install for the OVMX
 * alpha-dec-vms musl port (vms-157).
 *
 * This is the fix for the gap-11 first-fatal. The generic musl
 * src/thread/__set_thread_area.c routes through SYS_set_thread_area and so
 * reaches __vms_alpha_syscall -- but on Alpha the thread pointer is NOT set by
 * a syscall. It lives in the PALcode "unique" register, written with
 * PAL_wruniq (call_pal 0x9f) taking the new value in $16 (a0). A syscall for
 * this would (correctly) fail, and at rung 1 that ENOSYS was the a_crash that
 * halted __init_tls.
 *
 * So this arch override replaces the generic implementation: program the TP
 * directly with wruniq and return success. This matches the in-tree, qemu-alpha
 * -proven references:
 *   - src/libvmssys/vms_runtime_init.c setup_tls() __alpha__ branch (wruniq)
 *   - src/imgact/arch/alpha/start.S imgact_set_tp (call_pal 0x9f)
 * and the read side, __get_tp() in arch/alpha-dec-vms/pthread_arch.h, uses the
 * matching PAL_rduniq (0x9e).
 *
 * Alpha TLS is variant I (TLS block above the TP), TCB 16 bytes, tprel offset
 * 16 -- see pthread_arch.h (GAP_ABOVE_TP 16). musl's __init_tls computes the TP
 * (TP_ADJ) and passes it here; we only have to load it into the PAL register.
 *
 * Inline asm (not a hand-written VMS-calling-standard .s) so the alpha-dec-vms
 * cc1 emits the correct procedure descriptor / linkage its C callers expect.
 */

int __set_thread_area(void *p)
{
	register unsigned long __r16 __asm__("$16") = (unsigned long)p;
	__asm__ __volatile__ ("call_pal 0x9f"   /* PAL_wruniq: TP <- $16 */
			      : : "r"(__r16) : "memory");
	return 0;
}
