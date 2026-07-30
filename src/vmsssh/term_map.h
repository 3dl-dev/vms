/*
 * term_map.h - SSH TERM environment variable -> OVMX terminal device-type
 *              string, factored out of vmssshd.c so it can be unit tested
 *              against the real product code (vms-97d).
 *
 * RULE 8 / RULE 10 LABEL (do not remove): the mapping below, and the
 * bare strings "VT100"/"VT200"/"VT300"/"VT400" it returns, are an OVMX
 * DESIGN CHOICE, not a reproduction of OpenVMS behavior. OpenVMS has no
 * mechanism that infers a terminal device type from a Unix TERM string —
 * device type is set explicitly with SET TERMINAL/DEVICE_TYPE, and the
 * $TTDEF constants it renders (TT$_VT100, TT$_VT200_SERIES, ...) print via
 * SHOW TERMINAL as e.g. "VT400_Series", not the bare "VT400" used here.
 * This function exists only to give the OVMX SSH daemon *some* device type
 * to advertise until real per-process terminal characteristics (vms-d0b)
 * exist; it must never be presented as, or tested as, VMS-authentic.
 */

#ifndef OVMX_VMSSSH_TERM_MAP_H
#define OVMX_VMSSSH_TERM_MAP_H

/* Maps a TERM value (may be NULL or empty) to an OVMX-invented terminal
 * device-type label. See the file header: this is NOT a $TTDEF value and
 * NOT VMS-pinned. */
const char *vmsssh_map_term_to_device_type(const char *term);

#endif /* OVMX_VMSSSH_TERM_MAP_H */
