/*
 * rightslist_live.h - LIVE identifier resolution over the binary $RDBDEF
 * RIGHTSLIST (vms-f15a, epic vms-d0c). The $ASCTOID (name->value) / $IDTOASC
 * (value->name) executive-context reads of SYS$SYSTEM:RIGHTSLIST.DAT as a
 * genuine RMS Prolog-3 indexed file of 48-byte $RDBDEF records -- NOT the
 * retired ASCII colon-delimited facade.
 *
 * ====================================================================
 * WHY THESE ARE EXECUTIVE-CONTEXT READS (the protection fix)
 * ====================================================================
 * RIGHTSLIST.DAT is protected WORLD:none, like SYSUAF.DAT. The earlier
 * F$IDENTIFIER backend opened it in the CALLER's context (rms_textfile ->
 * RMS $OPEN as the unprivileged process), which on the runtime ACP path is a
 * protection violation: an ordinary process cannot read the rights database
 * directly. On real VMS the identifier<->value conversion is done by the
 * executive services $ASCTOID / $IDTOASC, which read the rights database in
 * PRIVILEGED (SYSPRV/READALL) context on the caller's behalf -- so an
 * unprivileged image resolves an identifier WITHOUT holding read access to the
 * file. These entry points model exactly that: they open the file through
 * rms_open_named_handle, whose ACP window is the executive channel ($ASSIGN of
 * the mounted volume) -- the read is performed by the executive, not the
 * caller, so WORLD:none does not bar it.
 *
 * ====================================================================
 * WHY THESE LIVE IN VMSRMS (the library-layering seam)
 * ====================================================================
 * The binary engine (rms_prolog3.c, rightslist_rms.c) and the raw ACP/POSIX
 * handle opener (rms_open_named_handle, rms_core.c) live in LIBVMSRMS, which
 * links LIBVMS -- not the other way round. So the F$IDENTIFIER backend in
 * LIBVMS (src/libvms/rtl/rightslist.c) reaches THIS file the same way sysuaf.c
 * reaches sysuaf_live: through a WEAK reference. An image that also links
 * LIBVMSRMS (DCL, LOGINOUT) binds these and reads the real binary RIGHTSLIST;
 * a bare LIBVMS unit test that does not sees them NULL and falls through to a
 * fail-honest miss (Rule 9 / INV-6) -- never a silent ASCII/POSIX fallback.
 *
 * ====================================================================
 * SUBSTRATE (Rule 9 / INV-6)
 * ====================================================================
 * Every open goes through rms_open_named_handle: an ACP channel+window when
 * /dev/vms is present (the runtime), a POSIX-wrap of the resolved on-volume
 * path when the executive is absent (host ctest / plain-container link & self-
 * host gates / netbsd-vax cross). There is NO /vms passthrough on the runtime
 * path and NO ASCII anywhere: the record bytes are the 48-byte $RDBDEF record.
 */
#ifndef RIGHTSLIST_LIVE_H
#define RIGHTSLIST_LIVE_H

#include <stddef.h>
#include <stdint.h>

#include "rightslist_rms.h"   /* rightslist_rms_file_t + the $RDBDEF record   */

/* ------------------------------------------------------------------ *
 * $ASCTOID / $IDTOASC over an ALREADY-BOUND handle (the testable core).
 * The path-based entry points below delegate to these after opening the
 * file; a host test can author a binary RIGHTSLIST over a temp fd, bind it
 * with rightslist_rms_open, and exercise the real resolution + status
 * mapping against genuine binary records with no /dev/vms and no /vms.
 * ------------------------------------------------------------------ */

/* $ASCTOID: resolve identifier NAME -> 32-bit VALUE over `rf` (the name is
 * upcased/blank-padded internally). Returns SS$_NORMAL, SS$_NOSUCHID (no such
 * identifier), or SS$_BADPARAM for a NULL argument. */
uint32_t rightslist_live_asctoid_rf(rightslist_rms_file_t *rf,
                                    const char *name, uint32_t *value);

/* $IDTOASC: resolve 32-bit identifier VALUE -> NAME over `rf` (writes up to
 * bufsz bytes, NUL-terminated). Returns SS$_NORMAL, SS$_NOSUCHID, or
 * SS$_BADPARAM for a NULL argument. */
uint32_t rightslist_live_idtoasc_rf(rightslist_rms_file_t *rf,
                                    uint32_t value, char *name, size_t bufsz);

/* ------------------------------------------------------------------ *
 * $ASCTOID / $IDTOASC over SYS$SYSTEM:RIGHTSLIST.DAT (the runtime entry
 * points reached by the F$IDENTIFIER weak seam). These open the rights
 * database in EXECUTIVE context, resolve, and close.
 * ------------------------------------------------------------------ */

/* $ASCTOID name->value. Returns SS$_NORMAL, SS$_NOSUCHID (absent identifier OR
 * an unopenable/unbindable rights database -- rendered as the miss), or
 * SS$_BADPARAM. */
uint32_t ovmx_rightslist_asctoid(const char *name, uint32_t *value);

/* $IDTOASC value->name. Returns SS$_NORMAL, SS$_NOSUCHID, or SS$_BADPARAM. */
uint32_t ovmx_rightslist_idtoasc(uint32_t value, char *name, size_t bufsz);

#endif /* RIGHTSLIST_LIVE_H */
