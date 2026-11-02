#ifndef MALLOC_GLUE_H
#define MALLOC_GLUE_H

#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <elf.h>
#include <string.h>
#include "atomic.h"
#include "syscall.h"
#include "libc.h"
#include "lock.h"
#include "dynlink.h"

// use macros to appropriately namespace these.
#define size_classes __malloc_size_classes
#define ctx __malloc_context
#define alloc_meta __malloc_alloc_meta
#define is_allzero __malloc_allzerop
#define dump_heap __dump_heap

#define malloc __libc_malloc_impl
#define realloc __libc_realloc
#define free __libc_free

#define USE_MADV_FREE 0

#if USE_REAL_ASSERT
#include <assert.h>
#else
#undef assert
#define assert(x) do { if (!(x)) a_crash(); } while(0)
#endif

/*
 * OVMX/Alpha (rd vms-bc53): THE HEAP'S ADDRESS SPACE COMES FROM THE EXECUTIVE.
 * This file replaces musl 1.2.5's src/malloc/mallocng/glue.h verbatim except
 * for this block (tools/cross-alpha-vms/musl-arch/build-musl.sh copies it over
 * the original). Upstream, mallocng grows with Linux brk and mmap/munmap/
 * mremap; on OVMX every page the DEC C heap holds is created by the
 * executive's $EXPREG and deleted by its $DELTVA (src/vmslink/ovmx_decc_vm.c ->
 * KIF_SVC_VA -> src/kernel/vms_va.c), so the heap is P0 address space charged
 * to the process's PGFLQUOTA, and running out is the executive's SS$_EXQUOTA /
 * SS$_VASFULL -- surfaced here as NULL with errno ENOMEM, as the C standard
 * requires. No call below reaches a Linux system call.
 *
 *   brk      VMS has no program break. brk() answers "no", so mallocng takes
 *            its meta areas from mmap as it does whenever brk fails.
 *   mmap     anonymous, address-less only (all mallocng asks for): $EXPREG.
 *            The pages come back read/write demand-zero, as VMS creates them.
 *   munmap   $DELTVA of the range.
 *   mremap   not offered: realloc then takes its portable path (allocate,
 *            copy, free), which needs no Linux remap and moves only bytes.
 *   mprotect the meta-area "unprotect" asks for read/write on pages $EXPREG
 *            already made read/write: nothing to do. Anything else -> ENOSYS,
 *            which mallocng accepts.
 *   madvise  USE_MADV_FREE is 0, so mallocng never calls it; defined so that
 *            if it ever did, it would not reach Linux.
 */
#include <errno.h>

unsigned int ovmx$vm_expreg(unsigned long long len, unsigned long long *addr);
unsigned int ovmx$vm_deltva(unsigned long long addr, unsigned long long len);

#define brk(p) ((uintptr_t)0)

static inline void *__ovmx_heap_map(void *a, size_t len, int prot, int flags,
                                    int fd, off_t off)
{
	unsigned long long p = 0;
	(void)prot;
	if (a || fd != -1 || off || !(flags & MAP_ANONYMOUS)) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (!(ovmx$vm_expreg(len, &p) & 1)) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return (void *)(uintptr_t)p;
}

static inline int __ovmx_heap_unmap(void *a, size_t len)
{
	if (!(ovmx$vm_deltva((uintptr_t)a, len) & 1)) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static inline void *__ovmx_heap_remap(void *a, size_t old, size_t new, int flags, ...)
{
	(void)a; (void)old; (void)new; (void)flags;
	errno = ENOMEM;
	return MAP_FAILED;
}

static inline int __ovmx_heap_protect(void *a, size_t len, int prot)
{
	(void)a; (void)len;
	if (prot == (PROT_READ|PROT_WRITE))
		return 0;
	errno = ENOSYS;
	return -1;
}

static inline int __ovmx_heap_advise(void *a, size_t len, int advice)
{
	(void)a; (void)len; (void)advice;
	return 0;
}

#define mmap __ovmx_heap_map
#define munmap __ovmx_heap_unmap
#define mremap __ovmx_heap_remap
#define mprotect __ovmx_heap_protect
#define madvise __ovmx_heap_advise

#define DISABLE_ALIGNED_ALLOC (__malloc_replaced && !__aligned_alloc_replaced)

static inline uint64_t get_random_secret()
{
	uint64_t secret = (uintptr_t)&secret * 1103515245;
	for (size_t i=0; libc.auxv[i]; i+=2)
		if (libc.auxv[i]==AT_RANDOM)
			memcpy(&secret, (char *)libc.auxv[i+1]+8, sizeof secret);
	return secret;
}

#ifndef PAGESIZE
#define PAGESIZE PAGE_SIZE
#endif

#define MT (libc.need_locks)

#define RDLOCK_IS_EXCLUSIVE 1

__attribute__((__visibility__("hidden")))
extern int __malloc_lock[1];

#define LOCK_OBJ_DEF \
int __malloc_lock[1]; \
void __malloc_atfork(int who) { malloc_atfork(who); }

static inline void rdlock()
{
	if (MT) LOCK(__malloc_lock);
}
static inline void wrlock()
{
	if (MT) LOCK(__malloc_lock);
}
static inline void unlock()
{
	UNLOCK(__malloc_lock);
}
static inline void upgradelock()
{
}
static inline void resetlock()
{
	__malloc_lock[0] = 0;
}

static inline void malloc_atfork(int who)
{
	if (who<0) rdlock();
	else if (who>0) resetlock();
	else unlock();
}

#endif
