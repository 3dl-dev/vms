/*
 * PAL_SERVICES.H - VMS PALcode Service Declarations
 *
 * OpenVMX compatibility layer - On real OpenVMS/Alpha, this header
 * declares wrapper routines/macros around PALcode calls (privileged
 * architecture library instructions), used by low-level performance
 * and diagnostic code such as SYS$RPCC_64.
 *
 * The current corpus program that includes this header
 * (tests/corpus/tier1-examples/sys_rpcc_64.c) does not reference any
 * symbol from it directly — it only needs the header to exist so the
 * #include succeeds. Per the project's no-gold-plating rule, no
 * PAL$/__PAL_ symbols are speculatively declared here; the
 * architecture-specific PAL builtins used elsewhere in the corpus
 * (__PAL_RD_PS, __PAL_PROBER, __PAL_PROBEW) come from <builtins.h>,
 * which the affected programs already include separately.
 *
 * Reference: OpenVMS Calling Standard / Alpha Architecture Reference
 *            Manual, PALcode chapter (background only — no symbols
 *            from this manual are declared in this header)
 */

#ifndef __PAL_SERVICES_H
#define __PAL_SERVICES_H

#ifdef __cplusplus
extern "C" {
#endif

/* Intentionally empty: see header comment above. */

#ifdef __cplusplus
}
#endif

#endif /* __PAL_SERVICES_H */
