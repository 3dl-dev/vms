#ifndef DCL_RMS_H
#define DCL_RMS_H

/*
 * dcl_rms.h - DCL file access through RMS / $QIO-to-ACP (vms-481, epic vms-208).
 *
 * DCL file commands (DIRECTORY, TYPE, COPY, CREATE, SET DEFAULT) and the F$
 * file lexicals (F$SEARCH, F$FILE_ATTRIBUTES, F$PARSE) reach files the VMS way:
 * through RMS ($OPEN/$GET/$CREATE/$PUT/$SEARCH) and the OVMX attribute accessor
 * rms_file_attr -- which on the product runtime (__linux__) route to the
 * Files-11 ODS-2 ACP over /dev/vms (src/vmsrms/rms_core.c + rms_search.c), and
 * on the netbsd-vax standalone cross keep RMS's POSIX backend (vms-d5d). DCL no
 * longer calls opendir/stat/fopen on a vmsfs_to_linux_path("/vms/...") path for
 * these commands.
 *
 * FAIL-HONEST (CLAUDE.md Rule 9 / INV-6): with no ACP-mounted SYS$DISK (no
 * /dev/vms) these helpers return the real RMS/SS$ error; there is NO silent
 * POSIX fallback. That is the ATOMIC-FLIP-GROUP behaviour -- until the boot flip
 * ACP-mounts SYS$DISK, the plain ctest environment sees these fail-honest.
 *
 * These are thin DCL-side helpers; the executive file semantics live in RMS.
 */

#include <stddef.h>
#include <stdint.h>
#include "rms/rms.h"          /* struct rms_fileattr, sys$*, rms_file_attr */

struct dcl_context;

/* Build the effective VMS filespec (device/directory defaulted from the context
 * default directory) that DCL hands to RMS. Mirrors the defaulting dcl_resolve_path
 * does before translation, but STOPS at the VMS spec (no Linux path). A Linux
 * passthrough spec (starts with '/' or './') is returned unchanged. Returns 0 on
 * success, -1 on bad args. */
int dcl_rms_effective_spec(struct dcl_context *ctx, const char *spec,
                           char *out, size_t outsz);

/* ---- Sequential record READ (TYPE, COPY source) ---- */
struct dcl_rms_reader;   /* opaque: FAB+RAB over an RMS $OPEN/$CONNECT */
/* Open `spec` for sequential $GET. On failure returns NULL and sets *rms_status
 * (if non-NULL) to the RMS error (e.g. RMS$_FNF). */
struct dcl_rms_reader *dcl_rms_read_open(struct dcl_context *ctx, const char *spec,
                                         uint32_t *rms_status);
/* $GET the next record into buf (NUL-terminated, no record terminator). Returns
 * the record length (>= 0) on success, or -1 at end-of-file / error (sets *eof
 * to 1 at clean EOF, 0 on a hard error). */
int  dcl_rms_read_record(struct dcl_rms_reader *r, char *buf, size_t bufsz, int *eof);
void dcl_rms_read_close(struct dcl_rms_reader *r);

/* ---- Sequential record WRITE (CREATE, COPY dest) ---- */
struct dcl_rms_writer;   /* opaque: FAB+RAB over an RMS $CREATE/$CONNECT */
/* Create `spec` (new highest version) with record format rfm/rat/mrs and open it
 * for sequential $PUT. On failure returns NULL and sets *rms_status. Use
 * FAB$C_VAR / (FAB$M_CR) / 0 for an ordinary text file. */
struct dcl_rms_writer *dcl_rms_write_create(struct dcl_context *ctx, const char *spec,
                                            uint8_t rfm, uint8_t rat, uint16_t mrs,
                                            uint32_t *rms_status);
/* $PUT one record (len bytes). Returns 0 on success, -1 on error. */
int      dcl_rms_write_record(struct dcl_rms_writer *w, const char *buf, size_t len);
/* $CLOSE + $DASSGN. Returns the RMS close status. */
uint32_t dcl_rms_write_close(struct dcl_rms_writer *w);

/* ---- Wildcard directory iteration (DIRECTORY, F$SEARCH) ---- */
struct dcl_rms_dir;      /* opaque: FAB+NAM over $PARSE/$SEARCH */
/* Open a wildcard search over `pattern` (a VMS filespec, wildcards allowed). The
 * pattern is device/dir-defaulted from the context. Returns NULL on a $PARSE
 * failure. */
struct dcl_rms_dir *dcl_rms_dir_open(struct dcl_context *ctx, const char *pattern);
/* Next match. Fills `spec` with the full resultant "DEV:[DIR]NAME.TYP;VER". If
 * `fid` is non-NULL, fills the genuine {num,seq,rvn} File ID of the match (from
 * the ACP search; all-zero on the POSIX cross). Returns 1 on a match, 0 at end
 * (RMS$_NMF) or on error. */
int dcl_rms_dir_next(struct dcl_rms_dir *d, char *spec, size_t specsz,
                     uint16_t *fid_num, uint16_t *fid_seq, uint8_t *fid_rvn);
/* The RMS status of the LAST $SEARCH on this context (the value dcl_rms_dir_next
 * saw when it returned 0). Lets a caller tell RMS$_NMF (the directory exists and
 * iteration is exhausted -- 0 matches means %DIRECT-W-NOFILES) apart from
 * RMS$_DNF (the directory itself does not exist -- %RMS-E-DNF). VMS distinguishes
 * the two; DIRECTORY must too. Valid only after dcl_rms_dir_next has returned 0. */
uint32_t dcl_rms_dir_status(const struct dcl_rms_dir *d);
void dcl_rms_dir_close(struct dcl_rms_dir *d);

/* ---- File erase (DELETE) ---- */
/* $ERASE `spec` (a VMS filespec with an explicit version) over the Files-11
 * ODS-2 ACP -- $ASSIGN + IO$_DELETE removes the directory entry and deallocates
 * the file (src/vmsrms/rms_core.c rms_impl_erase). Device/dir-defaults `spec`
 * from the context. Returns the RMS status (RMS$_NORMAL on success, RMS$_FNF /
 * RMS$_PRV / RMS$_ACC on failure). Fail-honest on an absent ACP (INV-6): the
 * underlying RMS $ERASE defers to the legacy POSIX unlink only when /dev/vms is
 * unreachable, exactly like every other RMS path here. */
uint32_t dcl_rms_erase(struct dcl_context *ctx, const char *spec);

/* ---- One file's genuine ODS-2 attributes (DIRECTORY /FULL, F$FILE_ATTRIBUTES) ---- */
/* Device/dir-defaults `spec` from the context, then reads the header via
 * rms_file_attr (IO$_ACCESS ATR list). Returns RMS$_NORMAL (out filled) or a
 * fail-honest RMS$_ (out zeroed). */
uint32_t dcl_rms_attr(struct dcl_context *ctx, const char *spec,
                      struct rms_fileattr *out);

/* ---- Stage an image's genuine bytes off the ODS-2 volume over the ACP (vms-104) ----
 * Device/dir-defaults `spec` from the context, then reads its bytes THROUGH the
 * Files-11 ACP (IO$_READVBLK, rms_stage_over_acp) into the Linux path `dest`
 * (mode 0755) -- NEVER a /vms POSIX read (Rule 9 / INV-6). Used by the foreign-
 * command resolver to give a native bootstrap tool a POSIX home sourced from the
 * volume. Returns RMS$_NORMAL, a fail-honest RMS$_ when absent, or RMS$_ACC when
 * no ACP-mounted volume is reachable (caller must fail honestly, not /vms-fall). */
uint32_t dcl_rms_stage(struct dcl_context *ctx, const char *spec,
                       const char *dest);

#endif /* DCL_RMS_H */
