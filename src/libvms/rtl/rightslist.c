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
 * BINARY $RDBDEF, READ IN EXECUTIVE CONTEXT (vms-f15a).
 * ===================================================================
 * The general-identifier half no longer scans an ASCII colon-delimited
 * RIGHTSLIST.DAT in the CALLER's context. RIGHTSLIST.DAT is protected
 * WORLD:none (like SYSUAF.DAT); a caller-context RMS $OPEN of it on the
 * runtime ACP path is a protection violation, which is why F$IDENTIFIER used
 * to fail there. It now resolves identifiers the VMS way: through the
 * executive services $ASCTOID (name->value) / $IDTOASC (value->name), which
 * read the genuine binary $RDBDEF indexed file in PRIVILEGED context on the
 * caller's behalf -- so an unprivileged process resolves an identifier without
 * holding read access to the file.
 *
 * Those services live in LIBVMSRMS (ovmx_rightslist_asctoid / _idtoasc in
 * src/vmsrms/rightslist_live.c) and are reached through a WEAK reference, the
 * same layering seam sysuaf.c uses to reach sysuaf_live: an image that links
 * LIBVMSRMS (DCL, LOGINOUT) binds them; a bare LIBVMS unit test that does not
 * sees them NULL and takes the fail-honest miss. There is NO ASCII reader and
 * NO /vms passthrough left on this path (Rule 9 / INV-6).
 *
 * UIC identifiers are still DERIVED from SYSUAF (itself now the binary
 * $UAFDEF file, vms-d92), so an account has exactly one UIC in the whole
 * system (rightslist.h).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rightslist.h"
#include "sysuaf.h"
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

    /* General identifiers first: the rights database proper, read by the
     * executive $ASCTOID over the binary $RDBDEF. Order does not matter for
     * correctness -- a name cannot be both a general identifier and an
     * account -- but it puts the file this reader owns on the primary path.
     * A missing LIBVMSRMS (weak symbol NULL) or an unreachable rights
     * database is a MISS, never a fall-back to a built-in table (Rule 9). */
    if (ovmx_rightslist_asctoid) {
        uint32_t v = 0;
        uint32_t st = ovmx_rightslist_asctoid(name, &v);
        if ($VMS_STATUS_SUCCESS(st)) {
            *value = v;
            return 0;
        }
        /* SS$_NOSUCHID: not a general identifier -- try the UIC path below. */
    }

    /* UIC identifiers, derived from SYSUAF (the binary $UAFDEF file) so an
     * account has exactly one UIC in the whole system (rightslist.h). */
    sysuaf_record_t rec;
    if (sysuaf_lookup(name, &rec) == 0) {
        *value = (rec.uic_group << 16) | rec.uic_member;
        return 0;
    }

    return -1;
}

int rightslist_value_to_name(uint32_t value, char *buf, size_t bufsz)
{
    if (!buf || !bufsz)
        return -1;

    /* General identifiers via the executive $IDTOASC over the binary $RDBDEF. */
    if (ovmx_rightslist_idtoasc) {
        uint32_t st = ovmx_rightslist_idtoasc(value, buf, bufsz);
        if ($VMS_STATUS_SUCCESS(st))
            return 0;
    }

    sysuaf_record_t rec;
    if (sysuaf_lookup_by_uic(value, &rec) == 0) {
        snprintf(buf, bufsz, "%s", rec.username);
        return 0;
    }

    return -1;
}
