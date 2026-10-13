/* SPDX-License-Identifier: GPL-2.0 */
/* vms_internal.h (lnm_shim/) - redirects src/kernel-core/vms_lnm.c's
 * #include "vms_internal.h" to the host stand-in (rd vms-94a), exactly as
 * lock_shim/ does for vms_lock.c. */
#include "../lnm_host_internal.h"
