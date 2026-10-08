/*
 * crtl_filespec.h - the C RTL's Unix<->VMS file-specification translator
 * (vms-32ae), shared by decc$to_vms/decc$from_vms/decc$translate_vms and the
 * C RTL file layer. See src/vmsrms/crtl_filespec.c for the rules.
 */
#ifndef OVMX_CRTL_FILESPEC_H
#define OVMX_CRTL_FILESPEC_H

#include <stddef.h>

/* decc$to_vms action-routine file types (DEC C <unixlib.h>). */
#ifndef DECC$K_FOREIGN
#define DECC$K_FOREIGN   0
#define DECC$K_FILE      1
#define DECC$K_DIRECTORY 2
#endif

/* ovmx_crtl_unix_to_vms mode (decc$to_vms no_directory: 0, 1, 2) and result. */
#define OVMX_FS_AUTO     0   /* a trailing '/' or "."/".." makes a directory */
#define OVMX_FS_FILE     1   /* always a file (no_directory = 1)             */
#define OVMX_FS_DIR      2   /* always a directory (no_directory = 2)        */
#define OVMX_FS_PASSTHRU 3   /* result: the input was already VMS syntax    */

/* 1 if `s` is an OpenVMS-syntax specification (':' '[' '<' '>' ']' or ';',
 * and no '/'), else 0. */
int ovmx_crtl_is_vms_syntax(const char *s);

/* Translate a UNIX-style spec to OpenVMS syntax into out[outsz]. Returns
 * OVMX_FS_FILE or OVMX_FS_DIR for the kind produced, OVMX_FS_PASSTHRU when the
 * input was already VMS syntax (copied verbatim), or -1 with errno set
 * (EINVAL, EISDIR, ENAMETOOLONG). */
int ovmx_crtl_unix_to_vms(const char *in, char *out, size_t outsz, int mode);

/* Translate an OpenVMS spec to UNIX syntax into out[outsz]: 0, or -1 with
 * errno (EINVAL, ENAMETOOLONG). A UNIX-syntax input is copied verbatim. */
int ovmx_crtl_vms_to_unix(const char *in, char *out, size_t outsz);

#endif /* OVMX_CRTL_FILESPEC_H */
