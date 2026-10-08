/*
 * unixlib.h - DEC C UNIX-compatibility routines
 *
 * DEC C declares its UNIX-style process/environment/file-name routines
 * (getenv, getcwd, chdir, mkdir, getuid/getgid/geteuid/getegid, ...) in
 * <unixlib.h>.  On OVMX these are the C library's own, so this header
 * re-exports their standard declarations.  Ported VMS C sources include it
 * by name -- e.g. GCC's libiberty getopt.c under `#ifdef VMS`.
 *
 * The DEC C file-specification translators decc$to_vms, decc$from_vms and
 * decc$translate_vms are declared below (vms-32ae; carried by the RMS-backed
 * DECC$SHR -- wildcard expansion is an RMS directory search). decc$match_wild
 * is not provided, so it is not declared: a caller fails at compile time rather
 * than linking against nothing.
 */
#ifndef __UNIXLIB_H
#define __UNIXLIB_H

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* decc$to_vms action-routine file types. */
#define DECC$K_FOREIGN   0
#define DECC$K_FILE      1
#define DECC$K_DIRECTORY 2

/* UNIX -> OpenVMS: call action once per resulting spec (every match when
 * allow_wild and the spec has wildcards) until it returns 0; returns the
 * number of calls. no_directory: 0 either, 1 a file only, 2 a directory. */
int decc$to_vms(const char *__unix_spec,
                int (*__action)(char *__vms_spec, int __type),
                int __allow_wild, int __no_directory);
/* OpenVMS -> UNIX, the same contract. */
int decc$from_vms(const char *__vms_spec,
                  int (*__action)(char *__unix_spec), int __wild_flag);
/* OpenVMS -> UNIX into a buffer the RTL owns (reused by the next call), or a
 * null pointer for an invalid specification. */
char *decc$translate_vms(const char *__vms_spec);


/* DEC C feature switches (vms-db7; carried by the RMS-backed DECC$SHR). Only the
 * features OVMX honours exist: an unknown name is -1 with errno EINVAL, never
 * accepted and ignored. mode 0 = the default value, 1 = the current value.
 * Index 0 is never valid. */
int decc$feature_get_index(const char *__name);
char *decc$feature_get_name(int __index);
int decc$feature_get_value(int __index, int __mode);
int decc$feature_set_value(int __index, int __mode, int __value);

#ifdef __cplusplus
}
#endif

#endif /* __UNIXLIB_H */
