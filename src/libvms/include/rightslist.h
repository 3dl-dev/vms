/*
 * rightslist.h - the rights database (SYS$SYSTEM:RIGHTSLIST.DAT)
 *
 * vms-2f8. OVMX shipped a populated RIGHTSLIST.DAT from the day the boot
 * path provisioned it, and no code read it: F$IDENTIFIER answered from a
 * hardcoded pair of names in src/vmsdcl/dcl_lexical.c. This is the reader
 * that makes the shipped file the source.
 *
 * TWO KINDS OF IDENTIFIER, TWO SOURCES, ONE COPY OF EACH:
 *
 *   general identifier   %X8xxxxxxx   from RIGHTSLIST.DAT
 *   UIC identifier       [g,m] octal  DERIVED from SYSUAF.DAT
 *
 * The oracle (docs/oracle/vax73-rights-database.md) shows both kinds in one
 * AUTHORIZE listing, because on VMS AUTHORIZE maintains an account and its
 * UIC identifier together. OVMX has no such coupled path, so keeping the UIC
 * rows in a second file would let two files disagree about one account's
 * UIC -- the defect vms-e60 closed. See the shipped RIGHTSLIST.DAT header.
 *
 * NOT A VMS API. sys$asctoid/sys$idtoasc are the VMS system services for
 * this conversion and OVMX does not implement them; they were deferred in
 * tests/libvms/test_lib_fb3.c for want of operator-signed status constants,
 * and that is still true (SS$_NOSUCHID / SS$_IVIDENT in ssdef.h are in
 * vms-c90's placeholder population). This interface deliberately returns
 * plain 0/-1 rather than a VMS status, so that nothing here depends on an
 * unpinned constant. Building the real services on top of this reader is
 * follow-up work, not this file's claim.
 */

#ifndef RIGHTSLIST_H
#define RIGHTSLIST_H

#include <stdint.h>
#include <stddef.h>
#include "ovmx_layout.h"

#define RIGHTSLIST_PATH VMS_RIGHTSLIST_PATH

/* Longest identifier name the rights database holds, plus NUL.
   VMS identifier names are at most 31 characters. */
#define RIGHTSLIST_NAME_MAX 32

/*
 * Resolve an identifier NAME to its 32-bit value.
 * The name comparison is case-insensitive.
 * Returns 0 and writes *value on success; -1 if no such identifier.
 */
int rightslist_name_to_value(const char *name, uint32_t *value);

/*
 * Resolve a 32-bit identifier VALUE to its name.
 * Returns 0 and writes up to bufsz bytes (NUL-terminated) on success;
 * -1 if no identifier holds that value.
 */
int rightslist_value_to_name(uint32_t value, char *buf, size_t bufsz);

#endif /* RIGHTSLIST_H */
