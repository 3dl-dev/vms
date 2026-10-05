/*
 * unixlib.h - DEC C UNIX-compatibility routines
 *
 * DEC C declares its UNIX-style process/environment/file-name routines
 * (getenv, getcwd, chdir, mkdir, getuid/getgid/geteuid/getegid, ...) in
 * <unixlib.h>.  On OVMX these are the C library's own, so this header
 * re-exports their standard declarations.  Ported VMS C sources include it
 * by name -- e.g. GCC's libiberty getopt.c under `#ifdef VMS`.
 *
 * The DEC C file-specification translators (decc$to_vms, decc$from_vms,
 * decc$translate_vms, decc$match_wild) are not provided by OVMX's DECC$SHR
 * yet, so they are not declared: a caller fails at compile time rather than
 * linking against nothing.
 */
#ifndef __UNIXLIB_H
#define __UNIXLIB_H

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#endif /* __UNIXLIB_H */
