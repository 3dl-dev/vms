/*
 * vms_alpha_syscall.c - the REAL syscall backend for the OVMX alpha-dec-vms
 * musl port (vms-157, the last rung of P1).
 *
 * This replaces the rung-1 honest -ENOSYS stub with an actual Alpha/OSF-1
 * `callsys` trap into the Linux-Alpha kernel. Every __syscallN in
 * arch/alpha-dec-vms/syscall_arch.h, and the cancellable path in
 * src/thread/alpha-dec-vms/syscall_cp.s, funnels through this one function,
 * so wiring it here wires the whole port. (INV-6: no path here fakes success;
 * a failed syscall returns a genuine negative errno.)
 *
 * Alpha/Linux syscall convention (see src/libvmssys/arch/alpha/syscall.S, the
 * in-tree reference this mirrors):
 *   - syscall number in $0 (v0)
 *   - args a0..a5 in $16..$21  (this port's C ABI hands us n,a1..a6, which map
 *     n -> $0 and a1..a6 -> $16..$21)
 *   - trap instruction: `callsys` (call_pal 0x83)
 *   - error is OUT OF BAND: after the trap, $19 (a3) == 0 means success and $0
 *     holds the result; $19 != 0 means error and $0 holds a *positive* errno.
 *     Alpha does NOT return -errno like x86_64/aarch64 do.
 *
 * musl's __syscall_ret expects the classic negative-errno contract, so on the
 * error path we negate v0. With that one normalization every syscall wrapper in
 * musl works unchanged.
 *
 * The trap and the a3/v0 handling MUST be assembly (a plain C wrapper cannot
 * read the out-of-band error register). We use an explicit-register inline-asm
 * `callsys` -- the same idiom src/libvmssys/vms_runtime_init.c already proves
 * under qemu-alpha for call_pal -- so the alpha-dec-vms cc1 still emits the
 * correct OpenVMS-calling-standard procedure descriptor and linkage that the
 * cc1-compiled callers (and syscall_cp.s's linkage pair) expect.
 */

long __vms_alpha_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	register long r0  __asm__("$0")  = n;   /* syscall number -> result/errno  */
	register long r16 __asm__("$16") = a1;  /* a0 */
	register long r17 __asm__("$17") = a2;  /* a1 */
	register long r18 __asm__("$18") = a3;  /* a2 */
	register long r19 __asm__("$19") = a4;  /* a3 in; error flag out           */
	register long r20 __asm__("$20") = a5;  /* a4 */
	register long r21 __asm__("$21") = a6;  /* a5 */

	__asm__ __volatile__ (
		"callsys"
		: "+r"(r0), "+r"(r19)
		: "r"(r16), "r"(r17), "r"(r18), "r"(r20), "r"(r21)
		: "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8",
		  "$22", "$23", "$24", "$25", "$27", "$28", "memory");

	/* $19 (a3) nonzero => error; $0 holds POSITIVE errno. Normalize to the
	 * negative-errno form musl's __syscall_ret consumes. */
	if (r19 != 0)
		return -r0;
	return r0;
}
