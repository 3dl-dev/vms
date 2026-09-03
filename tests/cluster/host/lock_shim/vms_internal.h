/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_internal.h (lock_shim/) - a 3-line REDIRECTION, not a copy of the real
 * src/kernel/vms_internal.h.
 *
 * rd FC-P4.9. src/kernel-core/vms_lock.c itself (not a frozen seam header --
 * vms_lock.c is this item's own subject file, but it is left byte-for-byte
 * UNCHANGED, per this item's instructions) does `#include "vms_internal.h"`.
 * On a real kmod build that resolves to src/kernel/vms_internal.h (Linux) or
 * src/kernel-netbsd/vms_internal.h (NetBSD) via -I ordering, exactly like
 * exec_kbackend_linux.h's mechanism (see that file in this directory). This
 * item's host lock-manager CMake target puts this directory first on
 * vms_lock.c's include path, so this forwarder resolves instead.
 *
 * The real content is lock_host_internal.h, one directory up -- see that
 * file for why it is a NEW, scoped file rather than an attempt to compile
 * the real (Linux-kernel-only) vms_internal.h on the host.
 */
#include "../lock_host_internal.h"
