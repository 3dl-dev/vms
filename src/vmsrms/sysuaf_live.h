/*
 * sysuaf_live.h - LIVE SYSUAF access over the binary $UAFDEF indexed file
 * (vms-d92, epic vms-d0c). The atomic-flip entry points that read, enumerate
 * and write SYS$SYSTEM:SYSUAF.DAT as a genuine RMS Prolog-3 indexed file of
 * 644-byte $UAFDEF records -- NOT the retired ASCII+SHA-256 facade.
 *
 * ====================================================================
 * WHY THESE LIVE IN VMSRMS (the library-layering seam)
 * ====================================================================
 * The binary engine (rms_prolog3.c, sysuaf_rms.c) and the raw ACP/POSIX handle
 * opener (rms_open_named_handle, rms_core.c) live in LIBVMSRMS, which links
 * LIBVMS (purdy, the flags/priv converters, ovmx_layout) -- not the other way
 * round. So the LIVE auth path in LIBVMS (src/libvms/rtl/sysuaf.c,
 * src/libvms/syssvc/sys_uai.c) reaches THIS file the same way rms_textfile.c
 * reaches sys$open: through a WEAK reference. An image that also links
 * LIBVMSRMS (LOGINOUT, VMSSSHD, DCL, PROVISION, MAIL, the QEMU facility tests)
 * binds these and reads/writes the real binary SYSUAF; a bare LIBVMS unit test
 * that does not sees them NULL and fails honestly (Rule 9 / INV-6) -- never a
 * silent POSIX/ASCII fallback.
 *
 * ====================================================================
 * SUBSTRATE (Rule 9 / INV-6)
 * ====================================================================
 * Every open goes through rms_open_named_handle: an ACP channel+window when
 * /dev/vms is present (the runtime), a POSIX-wrap of the resolved on-volume
 * path when the executive is absent (host ctest / plain-container link & self-
 * host gates / netbsd-vax cross). There is NO /vms passthrough on the runtime
 * path and NO ASCII/SHA-256 anywhere: the record bytes are the 644-byte
 * $UAFDEF record and the password is the Purdy quadword.
 */
#ifndef SYSUAF_LIVE_H
#define SYSUAF_LIVE_H

#include <stdint.h>
#include "sysuaf_rms.h"   /* sysuaf_rms_record_t (644-byte $UAFDEF) + accessors */

/* Read the $UAFDEF record for `username` by the primary USERNAME key.
 * Returns RMS$_NORMAL, RMS$_RNF (no such user), or an open/engine error. */
uint32_t ovmx_sysuaf_read_user(const char *username, sysuaf_rms_record_t *out);

/* Read a $UAFDEF record holding UIC `uic` ((group<<16)|member) by the secondary
 * UIC key. Returns RMS$_NORMAL, RMS$_RNF, or an open/engine error. */
uint32_t ovmx_sysuaf_read_uic(uint32_t uic, sysuaf_rms_record_t *out);

/* Enumerate EVERY account in primary-key order (for home-directory provisioning
 * and AUTHORIZE LIST). `cb` receives each 644-byte record and returns 0 to
 * continue / non-0 to stop. Returns RMS$_NORMAL (incl. empty file), or an
 * open/engine error. */
typedef int (*ovmx_sysuaf_enum_cb)(const sysuaf_rms_record_t *rec, void *arg);
uint32_t ovmx_sysuaf_enum(ovmx_sysuaf_enum_cb cb, void *arg);

/* Persist ONE record into the EXISTING SYSUAF: in-place $UPDATE if the username
 * is already present (password/flags edits keep the record 644 bytes and leave
 * the USERNAME/UIC keys unchanged, so the index is untouched), else $PUT it.
 * Returns RMS$_NORMAL, or an open/engine error. Used by $SETUAI, SET PASSWORD
 * and AUTHORIZE ADD/MODIFY. */
uint32_t ovmx_sysuaf_store_user(const sysuaf_rms_record_t *rec);

/* Author a FRESH SYSUAF (seed / AUTHORIZE full rebuild): create-and-supersede
 * the indexed file and $PUT every record in `recs`. Returns RMS$_NORMAL, or an
 * open/create/engine error. */
uint32_t ovmx_sysuaf_write_all(const sysuaf_rms_record_t *recs, unsigned n);

#endif /* SYSUAF_LIVE_H */
