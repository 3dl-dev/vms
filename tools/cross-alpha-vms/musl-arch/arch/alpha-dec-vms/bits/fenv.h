/*
 * fenv.h - Alpha floating-point environment.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * RUNG-1 STATUS: no-op fenv (HONEST). We do NOT ship an Alpha src/fenv/ asm
 * backend yet, so the generic src/fenv/fenv.c no-ops are used: fegetround()
 * always reports FE_TONEAREST and fesetround()/exception ops do nothing. To
 * avoid advertising rounding modes we do not honor (which would be a lie), only
 * FE_TONEAREST and FE_ALL_EXCEPT==0 are exposed here, matching the no-op
 * backend. Real Alpha FPCR control (mf_fpcr/mt_fpcr + excb, dynamic-rounding
 * bits 58..59, exception bits 52..57) is a later rung.
 */
#define FE_ALL_EXCEPT 0
#define FE_TONEAREST  0

typedef unsigned long fexcept_t;

typedef struct {
	unsigned long __cw;
} fenv_t;

#define FE_DFL_ENV      ((const fenv_t *) -1)
