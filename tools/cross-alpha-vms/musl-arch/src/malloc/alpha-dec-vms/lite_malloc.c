/*
 * lite_malloc.c -- alpha-dec-vms arch override of musl's src/malloc/lite_malloc.c
 * (vms-032 / vms-3320). musl's Makefile replaces the generic file with this one
 * for ARCH=alpha-dec-vms (src/malloc/$(ARCH)/*.c -> REPLACED_OBJS).
 *
 * Why: generic lite_malloc.c carries a second allocator, the header-less brk
 * bump allocator __simple_malloc, exposed as a WEAK alias of __libc_malloc_impl
 * that mallocng's malloc.o overrides with a STRONG definition. That override
 * relies on the linker seeing every reference by name. The alpha-dec-vms back
 * end instead binds the same-TU calls in default_malloc/__libc_malloc to the
 * local weak alias as SECTION-RELATIVE relocations that never name the symbol.
 * Observed on the qemu-system-alpha boot of the crtl_rms3 port image (traced
 * allocator entry points, 2026-10-04): every malloc/calloc landed in
 * __simple_malloc (blocks carved from the brk at 0x20001002000) while free()
 * is mallocng's -- two allocators, one brk (mallocng's alloc_meta also takes
 * its meta area + PROT_NONE guard page from that same brk). Faces: the calloc
 * memset SIGSEGV at DECC$SHR+0x6D274 (a block running into an unmapped page)
 * and free()'s get_meta NULL-meta crash on a header-less block (vms-032).
 *
 * DECC$SHR is a shareable image with mallocng always linked, so the bump
 * allocator is never the right choice here. This override keeps the public
 * entry points lite_malloc.c provides (__libc_malloc and the weak malloc) and
 * forwards both to __libc_malloc_impl by NAME, with no local definition for a
 * section-relative self-bind to land on. There is exactly one heap.
 */
#include <stdlib.h>
#include "libc.h"

void *__libc_malloc(size_t n)
{
	return __libc_malloc_impl(n);
}

static void *default_malloc(size_t n)
{
	return __libc_malloc_impl(n);
}

weak_alias(default_malloc, malloc);
