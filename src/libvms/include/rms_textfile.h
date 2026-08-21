/*
 * rms_textfile.h - read a VMS text file's records the VMS way (epic vms-208,
 * vms-274).
 *
 * THE SUBSTRATE SWAP FOR THE AUTHORISATION READERS. SYSUAF.DAT, RIGHTSLIST.DAT
 * and $GETUAI used to reach their file with fopen(vmsfs_to_linux_path(SPEC))
 * -- a POSIX read on the /vms passthrough. That passthrough is being retired
 * (docs/design-files11-acp-executive.md). These files are opened the VMS way
 * now: RMS $OPEN a channel $ASSIGNed to the mounted ODS-2 volume, and $GET
 * successive records through the file's IO$_ACCESS window (IO$_READVBLK) -- all
 * in the executive, over /dev/vms. So a login authenticates by reading SYSUAF
 * from the genuine ODS-2 SYS$DISK, not from a Linux file.
 *
 * ONE CALL SHAPE, ARCH SPLIT HIDDEN HERE. The __linux__ build is RMS-over-ACP;
 * the netbsd-vax standalone cross keeps its POSIX fopen/fgets path until the VAX
 * mount retires with it (vms-d5d). Callers use the same open/getline/close
 * vocabulary on both, so the reroute is not duplicated at every reader.
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6). On __linux__ every read rides RMS ->
 * $QIO -> the ACP, which returns the real SS$_/RMS$_ status (RMS$_FNF when the
 * file is absent, and a failed $OPEN when there is no mounted ACP volume /
 * no /dev/vms). rms_textfile_open() then returns NULL. There is NO silent POSIX
 * fallback: a reader whose file cannot be reached via the ACP finds nothing,
 * exactly as it would find nothing from an unreadable file -- it never reads a
 * private substitute.
 */
#ifndef RMS_TEXTFILE_H
#define RMS_TEXTFILE_H

#include <stddef.h>

typedef struct rms_textfile rms_textfile_t;

/*
 * Open a VMS text file for sequential record reads. `vms_spec` is a VMS
 * filespec (e.g. "SYS$SYSTEM:SYSUAF.DAT"); device-field logical names are
 * resolved through LNM$FILE_DEV before the volume is $ASSIGNed, exactly as
 * $PARSE resolves them. Returns an opaque handle, or NULL if the file cannot be
 * reached (fail-honest -- no ACP volume, no /dev/vms, or no such file).
 */
rms_textfile_t *rms_textfile_open(const char *vms_spec);

/*
 * Read the next record into `buf` (always NUL-terminated). Returns 1 when a
 * record was read, 0 at end-of-file or on any read error. If the record does
 * not fit `bufsz`, it is truncated to bufsz-1 bytes and, when `too_long` is
 * non-NULL, *too_long is set to 1 (0 otherwise) -- the same over-length signal
 * the fgets-based sysuaf_read_line() reported, so a caller keeps skipping an
 * over-length row rather than parsing its prefix.
 */
int rms_textfile_getline(rms_textfile_t *tf, char *buf, size_t bufsz,
                         int *too_long);

/* Close the file and free the handle. NULL-safe. */
void rms_textfile_close(rms_textfile_t *tf);

/*
 * Append one text record to a VMS file via RMS $PUT positioned at EOF (the
 * guaranteed per-boot OPERATOR.LOG writer, vms-274). The file is $CREATEd if it
 * does not exist. `line` is written as one stream-LF record (no caller newline
 * needed). Returns 0 on success, -1 on any failure (fail-honest: no ACP volume
 * / no /dev/vms -> -1, never a POSIX write). Linux/ACP; the netbsd-vax cross
 * keeps POSIX append.
 */
int rms_textfile_append_line(const char *vms_spec, const char *line);

/*
 * Write one text record to a VMS file, superseding any prior contents, via RMS
 * $CREATE + $PUT (the guaranteed per-boot LASTLOGIN writer, vms-274). Returns 0
 * on success, -1 on any failure (fail-honest). Linux/ACP; netbsd-vax keeps
 * POSIX.
 */
int rms_textfile_write_line(const char *vms_spec, const char *line);

#endif /* RMS_TEXTFILE_H */
