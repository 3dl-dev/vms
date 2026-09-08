/*
 * vms/ssdef.h - port include-surface shim (vms-714c, R5).
 *
 * The alpha-dec-vms GCC port's own host sources #include this VMS-convention
 * name (a directory literally named "vms", lowercase basename + ".h") to
 * reach the DEC C RTL's SS$_ condition-value definitions - the Unix-hosted
 * analog of the real VMS text-library resolution (STARLET.TLB /
 * SYS$STARLET_C.TLB) that a native VMS DEC C compiler performs for a bare
 * `#include <ssdef.h>`. See docs/design-gcc-port-surface-gaps-register.md
 * S1.2 row R5.
 *
 * OVMX's SS$_ values already live in the canonical top-level ssdef.h used
 * throughout the executive; nothing here duplicates them - this is a plain
 * redirect so the port's include path shape resolves without a second
 * source of truth for status codes.
 */
#ifndef __VMS_SSDEF_H
#define __VMS_SSDEF_H
#include "../ssdef.h"
#endif /* __VMS_SSDEF_H */
