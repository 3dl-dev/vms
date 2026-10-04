/*
 * unixio.h - UNIX-style low-level I/O (open, read, write, close, ...)
 *
 * DEC C declares the POSIX descriptor-level I/O calls in <unixio.h>; on OVMX
 * these are the C library's own, so this header only re-exports them.  Ported
 * VMS C sources include it by name.
 */
#ifndef __UNIXIO_H
#define __UNIXIO_H

#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#endif /* __UNIXIO_H */
