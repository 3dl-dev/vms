/*
 * __stdio_exit.c -- alpha-dec-vms arch override of musl's
 * src/stdio/__stdio_exit.c (vms-599). musl's Makefile replaces the generic file
 * with this one for ARCH=alpha-dec-vms (src/stdio/$(ARCH)/*.c -> REPLACED_OBJS).
 *
 * Why: the generic file reaches stdin/stdout/stderr through WEAK aliases of a
 * local NULL (dummy_file), which stdin.c/stdout.c/stderr.c override with the real
 * FILEs only when the linker sees every reference by name. The alpha-dec-vms back
 * end binds same-TU references to a local weak alias section-relatively (the
 * vms-032 lite_malloc case), so exit() flushed nothing: a line an image printed
 * without a newline was lost at exit (the crtl-fd gate, qemu-system-alpha boot).
 * DECC$SHR always links stdin.c, stdout.c and stderr.c, so this override names
 * the three FILEs and defines none of them. The body is musl's.
 */
#include "stdio_impl.h"

static void close_file(FILE *f)
{
	if (!f) return;
	FFINALLOCK(f);
	if (f->wpos != f->wbase) f->write(f, 0, 0);
	if (f->rpos != f->rend) f->seek(f, f->rpos-f->rend, SEEK_CUR);
}

void __stdio_exit(void)
{
	FILE *f;
	for (f=*__ofl_lock(); f; f=f->next) close_file(f);
	close_file(__stdin_used);
	close_file(__stdout_used);
	close_file(__stderr_used);
}

weak_alias(__stdio_exit, __stdio_exit_needed);
