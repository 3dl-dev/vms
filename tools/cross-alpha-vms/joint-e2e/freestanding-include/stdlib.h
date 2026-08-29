/* Minimal freestanding <stdlib.h> shim (vms-8208).
 *
 * vms-ucrt0.c (the GPLv3 GCC-port crt0) does `#include <stdlib.h>' but the bare
 * alpha-dec-vms cross cc1 ships no libc headers. It uses ONLY NULL and size_t
 * from stdlib.h (argv64[argc]=NULL; sizeof), so this supplies exactly those and
 * nothing else -- enough to compile crt0 FRESH with the current $15 toolchain
 * (build-joint-image.sh) without pulling in a full libc header set. Not a real
 * stdlib.h; do not use for anything but compiling vms-ucrt0.c.
 */
#ifndef OVMX_CRT0_FREESTANDING_STDLIB_H
#define OVMX_CRT0_FREESTANDING_STDLIB_H
#ifndef NULL
#define NULL ((void *)0)
#endif
typedef unsigned long size_t;
#endif
