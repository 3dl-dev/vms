/*
 * vmssys.h - Master include for the VMS syscall layer
 *
 * Include this single header to get access to all libvmssys
 * facilities: syscall wrappers, types, string functions, formatted
 * output, synchronization, buffered I/O, and math.
 */

#ifndef _VMSSYS_H
#define _VMSSYS_H

#include "vms_types.h"
#include "vms_syscall.h"
#include "vms_errno.h"
#include "vms_string.h"
#include "vms_snprintf.h"
#include "vms_futex.h"
#include "vms_stdio.h"
#include "vms_math.h"
#include "vms_runtime_init.h"

#endif /* _VMSSYS_H */
