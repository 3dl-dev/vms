/*
 * rightslist.c - the rights database reader (vms-2f8; flipped to binary
 * $RDBDEF by vms-f15a, epic vms-d0c)
 *
 * See rightslist.h for the interface and the two-sources rationale, and
 * docs/oracle/vax73-rights-database.md for every value this file resolves,
 * measured on OpenVMS VAX V7.3.
 *
 * WHAT THIS REPLACES. src/vmsdcl/dcl_lexical.c's lex_identifier() held two
 * hardcoded branches -- SYSTEM and DEFAULT -- and answered the miss for
 * everything else. This is the F$IDENTIFIER backend that reads the facility
 * that owns the answer instead.
 *
 * ===================================================================
 * BINARY $RDBDEF, READ OVER THE ACP FROM THE WORLD-READABLE RIGHTS DB.
 * ===================================================================
 * BOTH general AND UIC identifiers are resolved from SYS$SYSTEM:RIGHTSLIST.DAT
 * (vms-930), the way real VMS does it: RIGHTSLIST holds a row for every general
 * identifier AND a UIC identifier for every account (oracle
 * docs/oracle/vax73-rights-database.md §4), and the file is WORLD-READABLE
 * (World:R, vms-109). So a caller-context RMS $OPEN of it over the runtime ACP
 * SUCCEEDS for an unprivileged process -- an unprivileged F$IDENTIFIER resolves
 * an identifier by reading the world-readable rights database directly, without
 * SYSPRV and WITHOUT touching the protected (World:none) SYSUAF. UIC
 * identifiers are NO LONGER derived from SYSUAF: an unprivileged caller cannot
 * read SYSUAF, and sourcing identifier resolution from a protected file was the
 * gap that left scenario-G's unprivileged F$IDENTIFIER failing.
 *
 * The resolvers live in LIBVMSRMS (ovmx_rightslist_asctoid / _idtoasc in
 * src/vmsrms/rightslist_live.c) and are reached through a WEAK reference: an
 * image that links LIBVMSRMS (DCL, LOGINOUT) binds them; a bare LIBVMS unit
 * test that does not sees them NULL and takes the fail-honest miss. There is
 * NO ASCII reader, NO SYSUAF fallback and NO /vms passthrough left on this
 * path (Rule 9 / INV-6).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rightslist.h"
#include "ssdef.h"
#include "stsdef.h"          /* $VMS_STATUS_SUCCESS */

/* ------------------------------------------------------------------ */
/* The executive identifier-resolution seam (LIBVMSRMS, weak).         */
/*                                                                     */
/* Declared + #pragma weak exactly as sysuaf.c declares ovmx_sysuaf_*: */
/* unresolved-weak in LIBVMS$SHR, bound at activation when an image     */
/* also loads LIBVMSRMS$SHR. NULL when LIBVMSRMS is absent (bare unit   */
/* test) -> the honest miss below, never an ASCII fallback.            */
/* ------------------------------------------------------------------ */
uint32_t ovmx_rightslist_asctoid(const char *name, uint32_t *value);
uint32_t ovmx_rightslist_idtoasc(uint32_t value, char *name, size_t bufsz);
#pragma weak ovmx_rightslist_asctoid
#pragma weak ovmx_rightslist_idtoasc

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

int rightslist_name_to_value(const char *name, uint32_t *value)
{
    if (!name || !*name || !value)
        return -1;

    /* Both general and UIC identifiers come from the world-readable
     * RIGHTSLIST.DAT, resolved by the executive $ASCTOID over the binary
     * $RDBDEF (the NAME key finds either kind). A missing LIBVMSRMS (weak
     * symbol NULL) or an unreachable rights database is a MISS, never a
     * fall-back to SYSUAF or a built-in table (Rule 9 / vms-930). */
    if (ovmx_rightslist_asctoid) {
        uint32_t v = 0;
        uint32_t st = ovmx_rightslist_asctoid(name, &v);
        if ($VMS_STATUS_SUCCESS(st)) {
            *value = v;
            return 0;
        }
    }

    return -1;
}

int rightslist_value_to_name(uint32_t value, char *buf, size_t bufsz)
{
    if (!buf || !bufsz)
        return -1;

    /* Both general and UIC identifiers via the executive $IDTOASC over the
     * binary $RDBDEF (the VALUE key finds either kind); no SYSUAF fallback
     * (vms-930). */
    if (ovmx_rightslist_idtoasc) {
        uint32_t st = ovmx_rightslist_idtoasc(value, buf, bufsz);
        if ($VMS_STATUS_SUCCESS(st))
            return 0;
    }

    return -1;
}
