/* SPDX-License-Identifier: GPL-2.0 */
/*
 * exec_host_common.h - tiny shared helper for the R1 HOST backend of the
 * exec_kbackend.h / exec_list.h / exec_hash.h / exec_rbtree.h seam (rd
 * FC-P4.9, docs/plan-faithful-cluster-executive.md P4, design sec 3.9).
 *
 * The container macros in exec_list_host.h / exec_hash_host.h /
 * exec_rbtree_host.h all need the same "recover the enclosing struct from an
 * embedded node pointer" trick (container_of). Defined ONCE here, included by
 * all three, so a TU that includes more than one of them (vms_lock.c includes
 * all three) does not see a macro-redefinition warning/error.
 */

#ifndef OVMX_EXEC_HOST_COMMON_H
#define OVMX_EXEC_HOST_COMMON_H

#include <stddef.h>   /* offsetof */

#define EXEC_CONTAINER_OF(ptr, type, member) \
	((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#endif /* OVMX_EXEC_HOST_COMMON_H */
