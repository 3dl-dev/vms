/*
 * _Fork.c - alpha-dec-vms override of musl's src/process/_Fork.c (vms-fb4).
 *
 * Identical to the generic _Fork except for ONE thing: the child's thread
 * pointer is handed to the kernel explicitly (CLONE_SETTLS) instead of being
 * left to inheritance.
 *
 * WHY. On Alpha the thread pointer is the PALcode UNIQUE value, set with
 * call_pal WRUNIQUE (__set_thread_area). Linux/Alpha's copy_thread() gives a
 * plain fork child the parent's UNIQUE by block-copying the parent's in-memory
 * HWPCB (thread_info->pcb.unique). That copy is only current if the PALcode
 * stored UNIQUE into the HWPCB at WRUNIQUE time. PALcode that caches UNIQUE in
 * an internal register and writes it back only at SWPCTX -- qemu-palcode, the
 * PALcode of the qemu-system-alpha machine OVMX boots on, does exactly that --
 * leaves pcb.unique holding whatever value was live at the parent's LAST
 * context switch. A process that set its thread pointer and forked before it
 * was ever switched out hands its child a STALE thread pointer: the child's
 * very first __pthread_self() dereference (self->tid in __post_Fork below,
 * with every signal blocked) faults and the child dies with SIGSEGV before it
 * runs a line of caller code. Observed on the vfork gate: parent TP 0x102d90,
 * child TP 0x202a2740 (the pre-DECC$SHR value), child killed by SIGSEGV, in
 * roughly half the boots (whether the parent happened to be switched out
 * between __set_thread_area and fork).
 *
 * CLONE_SETTLS makes copy_thread() set childti->pcb.unique = tls, so the child
 * starts with the parent's LIVE thread pointer (read with RDUNIQUE, which every
 * PALcode answers from its current value) regardless of PALcode caching or
 * scheduling history. The child shares nothing else with the parent: plain
 * fork semantics otherwise (SIGCHLD, no CLONE_VM, no tid pointers).
 *
 * Linux/Alpha clone() argument order is the generic one:
 *   clone(flags, newsp, parent_tidptr, child_tidptr, tls).
 */
#define _GNU_SOURCE             /* CLONE_SETTLS from <sched.h> */
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include "syscall.h"
#include "libc.h"
#include "lock.h"
#include "pthread_impl.h"
#include "aio_impl.h"
#include "fork_impl.h"

static void dummy(int x) { }
weak_alias(dummy, __aio_atfork);

void __post_Fork(int ret)
{
	if (!ret) {
		pthread_t self = __pthread_self();
		self->tid = __syscall(SYS_set_tid_address, &__thread_list_lock);
		self->robust_list.off = 0;
		self->robust_list.pending = 0;
		self->next = self->prev = self;
		__thread_list_lock = 0;
		libc.threads_minus_1 = 0;
		if (libc.need_locks) libc.need_locks = -1;
	}
	UNLOCK(__abort_lock);
	if (!ret) __aio_atfork(1);
}

pid_t _Fork(void)
{
	pid_t ret;
	sigset_t set;
	__block_all_sigs(&set);
	LOCK(__abort_lock);
	ret = __syscall(SYS_clone, SIGCHLD | CLONE_SETTLS, 0, 0, 0, __get_tp());
	__post_Fork(ret);
	__restore_sigs(&set);
	return __syscall_ret(ret);
}
