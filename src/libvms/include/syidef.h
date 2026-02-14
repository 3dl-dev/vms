/*
 * SYIDEF.H - SYI$ Item Code Definitions for SYS$GETSYI
 *
 * OpenVMX compatibility layer - Standalone header providing SYI$_
 * item codes. On real VMS, programs include <syidef.h> to get these
 * constants. In OVMX, they are also available via prcdef.h.
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$GETSYI)
 */

#ifndef __SYIDEF_H
#define __SYIDEF_H

#include "prcdef.h"

/* All SYI$_ constants are defined in prcdef.h.
 * This header exists for VMS source compatibility where
 * programs do: #include <syidef.h>
 */

#endif /* __SYIDEF_H */
