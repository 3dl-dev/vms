/*
 * JPIDEF.H - JPI$ Item Code Definitions for SYS$GETJPI
 *
 * OpenVMX compatibility layer - Standalone header providing JPI$_
 * item codes. On real VMS, programs include <jpidef.h> to get these
 * constants. In OVMX, they are also available via prcdef.h.
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$GETJPI)
 */

#ifndef __JPIDEF_H
#define __JPIDEF_H

#include "prcdef.h"

/* All JPI$_ constants are defined in prcdef.h.
 * This header exists for VMS source compatibility where
 * programs do: #include <jpidef.h>
 */

#endif /* __JPIDEF_H */
