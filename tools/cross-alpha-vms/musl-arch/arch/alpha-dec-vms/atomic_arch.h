/*
 * atomic_arch.h - Alpha (EV4+) load-locked / store-conditional atomics.
 *
 * OVMX alpha-dec-vms musl port (vms-960, RUNG 1). CORRECTNESS-CRITICAL - real.
 *
 * Alpha primitives (Alpha Architecture Handbook, Ver. 4, ch. 4):
 *   LDL_L / STL_C  - 32-bit load-locked / store-conditional
 *   LDQ_L / STQ_C  - 64-bit load-locked / store-conditional
 *   MB             - memory barrier (full fence)
 *
 * STx_C writes its source register with 1 (success) or 0 (failure).
 * We tie that register as both input (value) and output (flag).
 *
 * Alpha has the weakest memory model of any musl target: it does NOT honor
 * data/address dependencies for ordering, so an explicit MB is required around
 * every atomic RMW. musl's generic atomic.h builds a_cas/a_swap/a_fetch_* from
 * a_ll/a_sc and brackets each loop with a_pre_llsc()/a_post_llsc(); we point
 * both at a full MB so every RMW is sequentially consistent. This is
 * conservative but correct - never omit the barrier on Alpha.
 */

#define a_ll a_ll
static inline int a_ll(volatile int *p)
{
	int v;
	__asm__ __volatile__ ("ldl_l %0,%1" : "=r"(v) : "m"(*p));
	return v;
}

#define a_sc a_sc
static inline int a_sc(volatile int *p, int v)
{
	int r;
	__asm__ __volatile__ (
		"stl_c %1,%0"
		: "=m"(*p), "=&r"(r)
		: "1"(v)
		: "memory");
	return r;
}

#define a_ll_p a_ll_p
static inline void *a_ll_p(volatile void *p)
{
	void *v;
	__asm__ __volatile__ ("ldq_l %0,%1" : "=r"(v) : "m"(*(void *volatile *)p));
	return v;
}

#define a_sc_p a_sc_p
static inline int a_sc_p(volatile void *p, void *v)
{
	long r;
	__asm__ __volatile__ (
		"stq_c %1,%0"
		: "=m"(*(void *volatile *)p), "=&r"(r)
		: "1"(v)
		: "memory");
	return (int)r;
}

#define a_barrier a_barrier
static inline void a_barrier(void)
{
	__asm__ __volatile__ ("mb" : : : "memory");
}

/*
 * Bracket every generic ll/sc RMW loop with full barriers. On Alpha this is
 * mandatory for correctness, not an optimization knob.
 */
#define a_pre_llsc a_barrier
#define a_post_llsc a_barrier

#define a_crash a_crash
static inline void a_crash(void)
{
	/* Alpha has no dedicated trap-always encoding usable here; a call to
	 * the reserved instruction opcode 0 (CALL_PAL 0 / HALT is privileged)
	 * is not appropriate from user mode. Force a fault via a null store. */
	__asm__ __volatile__ ("stq $31,0($31)" : : : "memory");
}
