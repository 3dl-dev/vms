/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_kbackend_linux.h (lock_shim/) - a 3-line REDIRECTION, not a backend.
 *
 * rd FC-P4.9. The frozen src/kernel-core/exec_kbackend.h picks this exact
 * filename via a plain quoted #include when __linux__/__KERNEL__/
 * OVMX_KBACKEND_LINUX is set -- see its own "Backend selection" comment,
 * which documents that the CONCRETE file found under this name is chosen
 * entirely by the compiler's -I search order, never by editing
 * exec_kbackend.h itself (the real Linux kmod backend lives at
 * src/kernel/exec_kbackend_linux.h and is found the identical way, by
 * putting src/kernel on ITS target's -I path).
 *
 * This item's host lock-manager CMake target puts THIS directory
 * (tests/cluster/host/lock_shim/) first on vms_lock.c's include path and
 * never adds src/kernel at all, so this forwarder -- not the real kmod
 * backend -- is what exec_kbackend.h's #include "exec_kbackend_linux.h"
 * resolves to for that one target. exec_kbackend.h is NEVER modified.
 *
 * The real content is exec_kbackend_host.h (pthreads/malloc), one directory
 * up -- see that file for the actual op implementations and their scope.
 */
#include "../exec_kbackend_host.h"
