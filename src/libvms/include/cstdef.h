/*
 * CSTDEF.H - VMS CPU State Transition Definitions
 *
 * OpenVMX compatibility layer - Defines the CST$K_ constants used with
 * sys$cpu_transition and sys$cpu_transitionw to start, stop, or query
 * CPUs in a multiprocessor system.
 *
 * Reference: OpenVMS System Services Reference Manual
 */

#ifndef __CSTDEF_H
#define __CSTDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * CST$K_ — CPU transition function codes
 *
 * Passed as the first argument to sys$cpu_transitionw to specify
 * the desired CPU state transition.
 * ================================================================ */

#define CST$K_CPU_START         1   /* Start (bring into active set) a CPU */
#define CST$K_CPU_STOP          2   /* Stop (remove from active set) a CPU */
#define CST$K_CPU_HOLD          3   /* Hold a CPU at IPL 31 */
#define CST$K_CPU_RELEASE       4   /* Release a held CPU */

/* ================================================================
 * CST$K_ — Special CPU ID values
 * ================================================================ */

#define CST$K_ANY_STOPPED_CPU   -1  /* Target any stopped CPU */
#define CST$K_PRIMARY_CPU        0  /* Target the primary CPU */

/* ================================================================
 * CST$M_ — CPU transition flag bits
 * ================================================================ */

#define CST$M_NORESUME          0x00000001  /* Do not resume stopped CPU */

#ifdef __cplusplus
}
#endif

#endif /* __CSTDEF_H */
