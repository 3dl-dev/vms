/*
 * test_conformance_constants.c — vms-f16
 *
 * Pins the conformance-gap constants added for vms-f16 (see
 * docs/conformance-gap-report.md §3.3) so their values can never silently
 * drift, and so the header homes stay wired. The assertions are compile-time
 * (_Static_assert): drift fails the build.
 *
 * GROUNDING (clean-room Rule 8). Every VMS-authentic value below was pinned on
 * 2026-08-13 by assembling the relevant PUBLIC definition macro ($SSDEF,
 * $RMSDEF, $XABDEF/$XABITMDEF, $CHPDEF, $ISSDEF) as a module of GLOBAL symbols
 * and reading the exact defined longword out of the object's GSD with
 * ANALYZE/OBJECT/GSD — documented tool output, not disassembly. Anchors
 * SS$_NORMAL=1, SS$_ACCVIO=12, RMS$_EOF=98938, XAB$C_KEY=21 were verified in
 * the same dumps.
 *   - SYSTEM-facility SS$_ codes and RMS$_/XAB$/CHP$M/ISS$C values: OpenVMS
 *     VAX V7.3 (lab-2 node VAX1). These are architecture-invariant.
 *   - SS$_EXITFORCED and SS$_LOWPREC do not exist in VAX V7.3 $SSDEF; their
 *     values come from OpenVMS Alpha V8.4 (lab-Alpha node ALPHA1). SS$_ACCVIO=12
 *     matched on both, confirming the SYSTEM facility is architecture-invariant.
 *
 * A handful of families (SYI$_ item codes, FAB$L_FOP mask bits, OSS$M_ flags)
 * use OVMX-PRIVATE numbering in the existing headers — the oracle dump shows
 * OVMX already diverges there, so the authentic values would collide with
 * existing OVMX bits. Those constants are asserted against OVMX's chosen values
 * (a drift guard), and are explicitly labeled below as design choices, NOT as
 * VMS-authentic values.
 */

#include <stdio.h>
#include <ssdef.h>
#include <rmsdef.h>
#include <chpdef.h>
#include <issdef.h>
#include <ossdef.h>
#include <prcdef.h>     /* SYI$_ item codes */
#include "rms/fab.h"
#include "rms/xab.h"

/* ---- DEC C boolean convention (ssdef.h) ---- */
_Static_assert(FALSE == 0, "FALSE must be 0");
_Static_assert(TRUE  == 1, "TRUE must be 1");

/* ---- SYSTEM-facility condition values (ssdef.h) — ORACLE-PINNED ---- */
_Static_assert(SS$_LKWSETFUL     == 404,   "SS$_LKWSETFUL != VAX V7.3 oracle 404");
_Static_assert(SS$_ALIGN         == 1292,  "SS$_ALIGN != VAX V7.3 oracle 1292");
_Static_assert(SS$_DEVALRALLOC   == 1601,  "SS$_DEVALRALLOC != VAX V7.3 oracle 1601");
_Static_assert(SS$_LOWPREC       == 1873,  "SS$_LOWPREC != Alpha V8.4 oracle 1873");
_Static_assert(SS$_NOMOREPROC    == 2472,  "SS$_NOMOREPROC != VAX V7.3 oracle 2472");
_Static_assert(SS$_DUPIDENT      == 8748,  "SS$_DUPIDENT != VAX V7.3 oracle 8748");
_Static_assert(SS$_NOSUCHCPU     == 9028,  "SS$_NOSUCHCPU != VAX V7.3 oracle 9028");
_Static_assert(SS$_NOCALLPRIV    == 9284,  "SS$_NOCALLPRIV != VAX V7.3 oracle 9284");
_Static_assert(SS$_NOLOG         == 9332,  "SS$_NOLOG != VAX V7.3 oracle 9332");
_Static_assert(SS$_NOIMPERSONATE == 10284, "SS$_NOIMPERSONATE != VAX V7.3 oracle 10284");
_Static_assert(SS$_NOOPER        == 10388, "SS$_NOOPER != VAX V7.3 oracle 10388");
_Static_assert(SS$_EXITFORCED    == 11220, "SS$_EXITFORCED != Alpha V8.4 oracle 11220");
_Static_assert(SS$_USERDISABLED  == 11290, "SS$_USERDISABLED != VAX V7.3 oracle 11290");

/* All SS$_ error/warning values are even (low bit clear => not success). */
_Static_assert((SS$_ALIGN & 1) == 0, "SS$_ALIGN should not carry success severity");

/* ---- RMS journaling status codes (rmsdef.h) — ORACLE-PINNED ---- */
_Static_assert(RMS$_ACC_RUJ    == 115044, "RMS$_ACC_RUJ != VAX V7.3 oracle 115044");
_Static_assert(RMS$_JNLNOTAUTH == 115100, "RMS$_JNLNOTAUTH != VAX V7.3 oracle 115100");

/* ---- XABITM item-XAB codes (rms/xab.h) — ORACLE-PINNED ---- */
_Static_assert(XAB$C_ITM      == 36, "XAB$C_ITM != VAX V7.3 oracle 36");
_Static_assert(XAB$K_ITMLEN   == 32, "XAB$K_ITMLEN != VAX V7.3 oracle 32");
_Static_assert(XAB$K_SETMODE  == 2,  "XAB$K_SETMODE != VAX V7.3 oracle 2");
_Static_assert(XAB$K_SENSEMODE == 1, "XAB$K_SENSEMODE != VAX V7.3 oracle 1");

/* ---- CHP$_FLAGS access synonyms (chpdef.h) — ORACLE-PINNED ----
 * On real VMS CHP$M_OBSERVE aliases CHP$M_READ and CHP$M_ALTER aliases
 * CHP$M_WRITE; OVMX's READ/WRITE bits already match the oracle. */
_Static_assert(CHP$M_OBSERVE == CHP$M_READ,  "CHP$M_OBSERVE must alias CHP$M_READ");
_Static_assert(CHP$M_ALTER   == CHP$M_WRITE, "CHP$M_ALTER must alias CHP$M_WRITE");
_Static_assert(CHP$M_OBSERVE == 0x01, "CHP$M_OBSERVE != VAX V7.3 oracle 0x01");
_Static_assert(CHP$M_ALTER   == 0x02, "CHP$M_ALTER != VAX V7.3 oracle 0x02");

/* ---- Persona id constant (issdef.h) — ORACLE-PINNED ---- */
_Static_assert(ISS$C_ID_NATURAL == 1, "ISS$C_ID_NATURAL != VAX V7.3 oracle 1");

/* ======================================================================
 * OVMX-PRIVATE constants (design choices, NOT VMS-authentic values).
 * The oracle shows these families already diverge from VMS in OVMX, so
 * authentic values would collide with existing OVMX bits. These assertions
 * are drift guards over OVMX's own numbering, not VMS-conformance claims.
 * ====================================================================== */

/* SYI$_ CPU-inventory item codes (prcdef.h) — OVMX-private continuation. */
_Static_assert(SYI$_MAX_CPUS          == 0x0211, "SYI$_MAX_CPUS drifted from OVMX-private 0x0211");
_Static_assert(SYI$_ACTIVE_CPU_BITMAP == 0x0212, "SYI$_ACTIVE_CPU_BITMAP drifted from OVMX-private 0x0212");
_Static_assert(SYI$_AVAIL_CPU_BITMAP  == 0x0213, "SYI$_AVAIL_CPU_BITMAP drifted from OVMX-private 0x0213");
_Static_assert(SYI$_MAX_CPUS != SYI$_SCSSYSTEMID, "SYI$_ CPU codes must not collide with existing SYI$_ codes");

/* FAB$L_FOP mask bits (rms/fab.h) — OVMX-private FOP continuation. */
_Static_assert(FAB$M_ASY == 0x2000, "FAB$M_ASY drifted from OVMX-private 0x2000");
_Static_assert(FAB$M_RU  == 0x4000, "FAB$M_RU drifted from OVMX-private 0x4000");
_Static_assert(FAB$M_UFO == 0x8000, "FAB$M_UFO drifted from OVMX-private 0x8000");
/* Must not collide with the existing FOP bits in the same field. */
_Static_assert((FAB$M_ASY & (FAB$M_SQO | FAB$M_CTG | FAB$M_CIF)) == 0,
               "FAB$M_ASY collides with an existing FOP bit");
_Static_assert((FAB$M_RU  & FAB$M_ASY) == 0 && (FAB$M_UFO & FAB$M_RU) == 0,
               "new FOP bits must be mutually distinct");

/* OSS$M_ security-service flag (ossdef.h) — OVMX-private (real VMS uses 2,
 * which OVMX already assigned to OSS$M_WLOCK). */
_Static_assert(OSS$M_RELCTX == 0x04, "OSS$M_RELCTX drifted from OVMX-private 0x04");
_Static_assert((OSS$M_RELCTX & OSS$M_WLOCK) == 0, "OSS$M_RELCTX collides with OSS$M_WLOCK");

int main(void)
{
    /* All meaningful checks are compile-time. Exercise the boolean macros at
     * runtime too so the constants are unambiguously usable in expressions. */
    int t = TRUE, f = FALSE;
    if (t != 1 || f != 0) {
        fprintf(stderr, "FAIL: TRUE/FALSE not usable as runtime values\n");
        return 1;
    }
    printf("vms-f16 conformance constants: all compile-time assertions passed\n");
    return 0;
}
