/*
 * sysboot.h - SYSBOOT> conversational-boot prompt (vms-b81)
 *
 * STARTUP.EXE's SYSBOOT role: when the boot-flag register requests it, halt
 * on the console at "SYSBOOT> " before the executive attaches, let the
 * operator inspect/change the parameter set for THIS boot only (VMS
 * persistence semantics -- SET is in-memory, only WRITE touches the file),
 * then CONTINUE.
 *
 * Design record: docs/design-boot-faithful.md §2.2/§3.1/§4.2 (READ FIRST --
 * carries the byte-shape captures the SHOW table is pinned to).
 *
 * THE SAME FILE AS EVERYONE ELSE (vms-d34/vms-b6a7): this operates on
 * SYS$SYSTEM:OVMXVMSSYS.PAR, the real versioned parameter store vms_sysgen.c,
 * scsd.c and read_boot_parameters() already read/write through
 * sysgen_params.h's sysgen_read_string()/sysgen_current_path(). SYSBOOT
 * cannot use THAT reader directly, though: it must run before the VMS
 * filespec translator is available (the device table isn't populated until
 * main()'s Step 1b, which is after bare_metal_init() and therefore after
 * where SYSBOOT has to run -- see ovmx_init.c's integration comment). So
 * this file is handed the SYS$SYSTEM directory as a raw Linux path
 * (ovmx_layout.h's VMS_SYSTEM_DIR, which needs no device table -- the header
 * comment there says as much: "Use these ONLY in code that runs before the
 * VMS filespec translator is available") plus the file's name/ext, and
 * calls the exact same vmsfs_get_highest_version()/vmsfs_create_new_version()
 * primitives tools/vms_sysgen.c's USE CURRENT / WRITE CURRENT already use --
 * so a version SYSBOOT writes is visible to SYSGEN and vice versa, and a
 * version SYSBOOT reads at boot is whatever the highest version on disk
 * really is.
 */
#ifndef OVMX_SYSBOOT_H
#define OVMX_SYSBOOT_H

#include "sysgen_params.h"

/*
 * Does the kernel command line request conversational boot?
 *
 * OVMX DESIGN CHOICE (Rule 8), not oracle-pinned: real VMS's R5 boot-flags
 * bit layout is console-firmware-internal and was not read from VSI source.
 * The platform analogue chosen here is a kernel-cmdline pair,
 * "ovmx.flags=<r5>,<r6>", with bit 0 of the SECOND value meaning
 * conversational -- chosen to match the oracle's own example command line
 * verbatim (`boot -flags 0,1`; docs/design-boot-faithful.md §2.2/§3.1).
 *
 * Reads /proc/cmdline -- caller must already have /proc mounted. Produces
 * no output. Returns 1 if conversational boot is requested, 0 otherwise
 * (including when the flag is entirely absent -- a flagless boot must
 * never enter the prompt).
 */
int sysboot_conversational_requested(void);

/*
 * Load the working parameter set: tries the highest existing version of
 * <dir>/<name>.<ext> first (the real SYS$SYSTEM:OVMXVMSSYS.PAR, resolved by
 * the caller to a raw Linux directory -- see the file header), falls back
 * to compiled-in factory defaults if none exists. Never prints anything --
 * safe to call before the SYSBOOT> prompt exists.
 */
void sysboot_load_working_set(struct sysgen_file *ws, const char *dir,
                               const char *name, const char *ext);

/*
 * Run the interactive SYSBOOT> prompt against `ws` until CONTINUE (or EOF
 * on the console, treated the same way). Blocks on stdin. `dir`/`name`/`ext`
 * are what USE CURRENT / WRITE resolve against (same file as
 * sysboot_load_working_set()); `startup_file` is the fixed value
 * SHOW /STARTUP reports.
 *
 * Commands: SHOW <param> | SHOW /STARTUP, SET <param> <value>,
 * USE CURRENT | USE DEFAULT, WRITE, CONTINUE. Unknown commands error the
 * way SYSGEN already errors (same %SYSGEN-E-IVVERB shape) -- SYSBOOT has
 * no oracle-captured error vocabulary of its own, so the existing,
 * already-established OVMX SYSGEN message vocabulary is reused rather than
 * inventing a new one (Rule 8).
 */
void sysboot_run_prompt(struct sysgen_file *ws, const char *dir,
                         const char *name, const char *ext,
                         const char *startup_file);

/*
 * Look up a string-typed parameter's current value, trimmed of trailing
 * pad. Returns NULL if not found or not string-typed; otherwise a pointer
 * into a static buffer valid until the next call.
 */
const char *sysboot_get_string(const struct sysgen_file *ws, const char *name);

#endif /* OVMX_SYSBOOT_H */
