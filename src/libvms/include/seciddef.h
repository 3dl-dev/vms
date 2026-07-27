/*
 * SECIDDEF.H - VMS Section Identification (SECID) Structure
 *
 * OpenVMX compatibility layer - Defines the SECID type used as the
 * "sectid" argument to global-section services such as
 * sys$crmpsc_gdzro_64, sys$mgblsc_64, sys$mgblsc_gpfn_64, and
 * sys$dgblsc.
 *
 * PROVENANCE: the OpenVMS System Services Reference Manual documents
 * the SECTID parameter as a 2-longword array: a match-control value
 * (SEC$K_MATxxx, from secdef.h) followed by a section-identification
 * value used only when the match control requests exact-ID matching.
 * That 2-field shape is what corpus programs rely on (aggregate
 * initialization with 1 or 2 values). The exact field NAMES used by
 * real VMS's internal SECIDDEF macro are not published verbatim in
 * the sources available this session; the names below are OVMX's own
 * choice (clean-room, CLAUDE.md rule 8), flagged in vms-531 findings.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            ($CRMPSC_GDZRO_64, $MGBLSC_64 — "sectid" parameter)
 */

#ifndef __SECIDDEF_H
#define __SECIDDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * SECID — section identification array (2 longwords)
 * ================================================================ */

typedef struct _seciddef {
    unsigned int secid$l_matchctl;  /* Match control — SEC$K_MATxxx (secdef.h) */
    unsigned int secid$l_secid;     /* Section ID value (used iff SEC$K_MATIDENT) */
} SECID;

#ifdef __cplusplus
}
#endif

#endif /* __SECIDDEF_H */
