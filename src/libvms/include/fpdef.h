/*
 * FPDEF.H - VMS FastPath (FP$) Function Codes
 *
 * OpenVMX compatibility layer - Defines the FP$K_ function codes used
 * with sys$io_fastpathw to rebalance I/O FastPath port/CPU affinity.
 *
 * PROVENANCE: the $IO_FASTPATHW function-code mechanism is documented
 * in the public OpenVMS System Services Reference Manual. Exact
 * numeric value not confirmed against a fetchable public source this
 * session — sequential OVMX assignment, flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual ($IO_FASTPATHW)
 */

#ifndef __FPDEF_H
#define __FPDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * FP$K_ — SYS$IO_FASTPATHW function codes
 * ================================================================ */

#define FP$K_BALANCE_PORTS    1   /* Rebalance FastPath ports across the given CPU set */

#ifdef __cplusplus
}
#endif

#endif /* __FPDEF_H */
