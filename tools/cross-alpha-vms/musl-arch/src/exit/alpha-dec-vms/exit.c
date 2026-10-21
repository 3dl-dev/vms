/*
 * exit.c -- alpha-dec-vms arch override of musl's src/exit/exit.c (vms-599).
 * musl's Makefile replaces the generic file with this one for ARCH=alpha-dec-vms
 * (src/exit/$(ARCH)/*.c -> REPLACED_OBJS).
 *
 * Why: the generic exit.c reaches the atexit handlers (__funcs_on_exit, atexit.c)
 * and the stdio flush (__stdio_exit, __stdio_exit.c) through WEAK aliases of a
 * local no-op, which the strong definitions override only when the linker sees
 * every reference by name. The alpha-dec-vms back end binds same-TU calls to a
 * local weak alias section-relatively instead, never naming the symbol -- the
 * vms-032 lite_malloc case -- so exit() ran neither: an image's buffered output
 * and its atexit handlers were lost (observed on the qemu-system-alpha boot of
 * the crtl-fd gate: a line printed without a newline and then exit()ed never
 * reached the console). The DEC C RTL's exit() runs the atexit handlers and
 * flushes and closes the files before SYS$EXIT.
 *
 * DECC$SHR always links atexit.c and __stdio_exit.c, so this override calls both
 * by NAME, with no local definition for a self-bind to land on. _fini and
 * __libc_exit_fini keep musl's local forms: nothing overrides them here.
 */
#include <stdlib.h>
#include <stdint.h>
#include "libc.h"
#include "stdio_impl.h"

static void dummy()
{
}

weak_alias(dummy, _fini);

extern weak hidden void (*const __fini_array_start)(void), (*const __fini_array_end)(void);

static void libc_exit_fini(void)
{
	uintptr_t a = (uintptr_t)&__fini_array_end;
	for (; a>(uintptr_t)&__fini_array_start; a-=sizeof(void(*)()))
		(*(void (**)())(a-sizeof(void(*)())))();
	_fini();
}

weak_alias(libc_exit_fini, __libc_exit_fini);

_Noreturn void exit(int code)
{
	__funcs_on_exit();
	__libc_exit_fini();
	__stdio_exit();
	_Exit(code);
}
