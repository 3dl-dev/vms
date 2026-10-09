/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_native_p0_netbsd.c -- the dedicated uvm TU for VMS_IOCTL_NATIVE_PAGE0
 * (rd vms-b869): let ONE process map virtual page 0, for an OpenVMS VAX native
 * image fixed at P0 0x200.
 *
 * NetBSD keeps page 0 out of reach of user mappings by raising each new
 * address space's minimum to PAGE_SIZE at exec (kern_exec.c exec_vm_minaddr,
 * driven by vm.user_va0_disable). Baron's ruling (2026-10-09): the defence
 * stays on system-wide; page 0 is allowed per process, only through the
 * executive's native-image activation path. So the executive lowers the
 * minimum of the CALLER's map alone, and only when the caller is running the
 * native image activator. The change lives in that address space and goes with
 * it at the process's next execve; the sysctl and every other process keep
 * the defence.
 *
 * WHY A DEDICATED TU: <uvm/uvm_map.h> pulls <sys/rbtree.h>, whose rb_left /
 * rb_right macros collide with the executive's intrusive rbtree header (the
 * same reason as vms_sysmem_netbsd.c / vms_lnm_arena_netbsd.c), so this file
 * includes uvm and none of the executive headers. OVMX glue over the public
 * NetBSD uvm(9)/proc(9) interfaces; no NetBSD or VSI source is copied.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <uvm/uvm_extern.h>
#include <uvm/uvm_map.h>

/* Also prototyped in exec_kbackend_netbsd.h (the caller). */
int ovmx_native_page0_allow(struct proc *p, const char *activator);

static int
path_is(const char *path, const char *want)
{
	if (path == NULL)
		return 0;
	while (*path && *path == *want) {
		path++;
		want++;
	}
	return *path == '\0' && *want == '\0';
}

/* 0 when granted, EPERM when the caller is not the activator. */
int
ovmx_native_page0_allow(struct proc *p, const char *activator)
{
	struct vm_map *map;

	if (p == NULL || p->p_vmspace == NULL || !path_is(p->p_path, activator))
		return EPERM;
	map = &p->p_vmspace->vm_map;
	vm_map_lock(map);
	map->header.end = 0;            /* vm_map_min(map): page 0 mappable here */
	vm_map_unlock(map);
	return 0;
}
