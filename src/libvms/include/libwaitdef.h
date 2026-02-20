/*
 * LIBWAITDEF.H - VMS LIB$WAIT Float Format Definitions
 *
 * OpenVMX compatibility layer - Defines the LIB$K_ floating-point
 * format constants used with lib$wait to specify the float format
 * of the wait time argument.
 *
 * lib$wait suspends the calling process for a specified number of
 * seconds given as a floating-point value.  The format of the float
 * must be declared so the RTL can convert it to a VMS delta time.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 */

#ifndef __LIBWAITDEF_H
#define __LIBWAITDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * LIB$K_ — Floating-point format codes for lib$wait
 *
 * Passed as the optional "float_type" argument to lib$wait.
 * ================================================================ */

#define LIB$K_VAX_F     1   /* VAX F_floating (single precision) */
#define LIB$K_VAX_D     2   /* VAX D_floating (double precision) */
#define LIB$K_VAX_G     3   /* VAX G_floating (double, wider range) */
#define LIB$K_VAX_H     4   /* VAX H_floating (quad precision) */
#define LIB$K_IEEE_S    5   /* IEEE single precision */
#define LIB$K_IEEE_T    6   /* IEEE double precision (T_floating) */

#ifdef __cplusplus
}
#endif

#endif /* __LIBWAITDEF_H */
