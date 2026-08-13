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

/* ---- RMS$_ status codes (rmsdef.h) — ORACLE-PINNED, vms-a7d ----
 * 74 values corrected 2026-08-13 from a MACRO/OBJECT + ANALYZE/OBJECT/GSD
 * dump of `$RMSDEF GLOBAL` on lab-2 (vaxlab-2/vax1, OpenVMS VAX V7.3,
 * ANALYZ V07-04).  Anchors NORMAL/EOF/FNF verified in the same dump.
 * These are architecture-invariant condition values. */
_Static_assert(RMS$_NORMAL    ==  65537, "RMS$_NORMAL != VAX V7.3 oracle 65537");
_Static_assert(RMS$_EOF       ==  98938, "RMS$_EOF != VAX V7.3 oracle 98938");
_Static_assert(RMS$_FNF       ==  98962, "RMS$_FNF != VAX V7.3 oracle 98962");
_Static_assert(RMS$_FACILITY  ==      1, "RMS$_FACILITY != VAX V7.3 oracle 1");
_Static_assert(RMS$_OK_ALK    ==  98361, "RMS$_OK_ALK != VAX V7.3 oracle 98361");
_Static_assert(RMS$_OK_DEL    ==  98369, "RMS$_OK_DEL != VAX V7.3 oracle 98369");
_Static_assert(RMS$_OK_RLK    ==  98337, "RMS$_OK_RLK != VAX V7.3 oracle 98337");
_Static_assert(RMS$_OK_RRL    ==  98345, "RMS$_OK_RRL != VAX V7.3 oracle 98345");
_Static_assert(RMS$_OK_DUP    ==  98321, "RMS$_OK_DUP != VAX V7.3 oracle 98321");
_Static_assert(RMS$_OK_LIM    ==  98385, "RMS$_OK_LIM != VAX V7.3 oracle 98385");
_Static_assert(RMS$_OK_NOP    ==  98393, "RMS$_OK_NOP != VAX V7.3 oracle 98393");
_Static_assert(RMS$_OK_WAT    ==  98401, "RMS$_OK_WAT != VAX V7.3 oracle 98401");
_Static_assert(RMS$_ACC       == 114690, "RMS$_ACC != VAX V7.3 oracle 114690");
_Static_assert(RMS$_CRE       == 114698, "RMS$_CRE != VAX V7.3 oracle 114698");
_Static_assert(RMS$_BKZ       ==  99364, "RMS$_BKZ != VAX V7.3 oracle 99364");
_Static_assert(RMS$_BLN       ==  99372, "RMS$_BLN != VAX V7.3 oracle 99372");
_Static_assert(RMS$_CCR       ==  99476, "RMS$_CCR != VAX V7.3 oracle 99476");
_Static_assert(RMS$_BUG       ==  99380, "RMS$_BUG != VAX V7.3 oracle 99380");
_Static_assert(RMS$_CHG       ==  99484, "RMS$_CHG != VAX V7.3 oracle 99484");
_Static_assert(RMS$_DUP       ==  99564, "RMS$_DUP != VAX V7.3 oracle 99564");
_Static_assert(RMS$_DEL       ==  98914, "RMS$_DEL != VAX V7.3 oracle 98914");
_Static_assert(RMS$_DIR       ==  99532, "RMS$_DIR != VAX V7.3 oracle 99532");
_Static_assert(RMS$_FAC       ==  99604, "RMS$_FAC != VAX V7.3 oracle 99604");
_Static_assert(RMS$_IMX       ==  99692, "RMS$_IMX != VAX V7.3 oracle 99692");
_Static_assert(RMS$_IOP       ==  99700, "RMS$_IOP != VAX V7.3 oracle 99700");
_Static_assert(RMS$_RER       == 114932, "RMS$_RER != VAX V7.3 oracle 114932");
_Static_assert(RMS$_KEY       ==  99732, "RMS$_KEY != VAX V7.3 oracle 99732");
_Static_assert(RMS$_MRN       ==  99788, "RMS$_MRN != VAX V7.3 oracle 99788");
_Static_assert(RMS$_FLK       ==  98954, "RMS$_FLK != VAX V7.3 oracle 98954");
_Static_assert(RMS$_ESS       ==  99588, "RMS$_ESS != VAX V7.3 oracle 99588");
_Static_assert(RMS$_EXT       == 114722, "RMS$_EXT != VAX V7.3 oracle 114722");
_Static_assert(RMS$_FAB       ==  99596, "RMS$_FAB != VAX V7.3 oracle 99596");
_Static_assert(RMS$_DNF       == 114762, "RMS$_DNF != VAX V7.3 oracle 114762");
_Static_assert(RMS$_RNL       ==  98720, "RMS$_RNL != VAX V7.3 oracle 98720");
_Static_assert(RMS$_RLK       ==  98986, "RMS$_RLK != VAX V7.3 oracle 98986");
_Static_assert(RMS$_IFI       ==  99684, "RMS$_IFI != VAX V7.3 oracle 99684");
_Static_assert(RMS$_RNF       ==  98994, "RMS$_RNF != VAX V7.3 oracle 98994");
_Static_assert(RMS$_ISI       ==  99716, "RMS$_ISI != VAX V7.3 oracle 99716");
_Static_assert(RMS$_REX       ==  98978, "RMS$_REX != VAX V7.3 oracle 98978");
_Static_assert(RMS$_PRV       ==  98970, "RMS$_PRV != VAX V7.3 oracle 98970");
_Static_assert(RMS$_FEX       ==  98946, "RMS$_FEX != VAX V7.3 oracle 98946");
_Static_assert(RMS$_KRF       ==  99740, "RMS$_KRF != VAX V7.3 oracle 99740");
_Static_assert(RMS$_KSZ       ==  99748, "RMS$_KSZ != VAX V7.3 oracle 99748");
_Static_assert(RMS$_RSZ       == 100004, "RMS$_RSZ != VAX V7.3 oracle 100004");
_Static_assert(RMS$_FNM       ==  99628, "RMS$_FNM != VAX V7.3 oracle 99628");
_Static_assert(RMS$_SHR       == 100020, "RMS$_SHR != VAX V7.3 oracle 100020");
_Static_assert(RMS$_WER       == 114964, "RMS$_WER != VAX V7.3 oracle 114964");
_Static_assert(RMS$_MKD       == 114738, "RMS$_MKD != VAX V7.3 oracle 114738");
_Static_assert(RMS$_NEF       ==  99812, "RMS$_NEF != VAX V7.3 oracle 99812");
_Static_assert(RMS$_ORG       ==  99852, "RMS$_ORG != VAX V7.3 oracle 99852");
_Static_assert(RMS$_PLG       ==  99868, "RMS$_PLG != VAX V7.3 oracle 99868");
_Static_assert(RMS$_RAB       ==  99900, "RMS$_RAB != VAX V7.3 oracle 99900");
_Static_assert(RMS$_RAT       ==  99916, "RMS$_RAT != VAX V7.3 oracle 99916");
_Static_assert(RMS$_RFM       ==  99940, "RMS$_RFM != VAX V7.3 oracle 99940");
_Static_assert(RMS$_RSS       ==  99988, "RMS$_RSS != VAX V7.3 oracle 99988");
_Static_assert(RMS$_RTB       ==  98728, "RMS$_RTB != VAX V7.3 oracle 98728");
_Static_assert(RMS$_SEQ       == 100012, "RMS$_SEQ != VAX V7.3 oracle 100012");
_Static_assert(RMS$_SIZ       == 100028, "RMS$_SIZ != VAX V7.3 oracle 100028");
_Static_assert(RMS$_SYN       == 100052, "RMS$_SYN != VAX V7.3 oracle 100052");
_Static_assert(RMS$_TNS       ==  98744, "RMS$_TNS != VAX V7.3 oracle 98744");
_Static_assert(RMS$_TRE       == 100060, "RMS$_TRE != VAX V7.3 oracle 100060");
_Static_assert(RMS$_TYP       == 100068, "RMS$_TYP != VAX V7.3 oracle 100068");
_Static_assert(RMS$_WCC       ==  99050, "RMS$_WCC != VAX V7.3 oracle 99050");
_Static_assert(RMS$_DME       ==  99540, "RMS$_DME != VAX V7.3 oracle 99540");
_Static_assert(RMS$_NMF       ==  99018, "RMS$_NMF != VAX V7.3 oracle 99018");
_Static_assert(RMS$_CRE_STM   ==  98409, "RMS$_CRE_STM != VAX V7.3 oracle 98409");
_Static_assert(RMS$_CREATED   ==  67097, "RMS$_CREATED != VAX V7.3 oracle 67097");
_Static_assert(RMS$_FILEPURGED ==  67193, "RMS$_FILEPURGED != VAX V7.3 oracle 67193");
_Static_assert(RMS$_SUPERSEDE ==  67121, "RMS$_SUPERSEDE != VAX V7.3 oracle 67121");
_Static_assert(RMS$_COD       ==  99500, "RMS$_COD != VAX V7.3 oracle 99500");
_Static_assert(RMS$_CUR       ==  99508, "RMS$_CUR != VAX V7.3 oracle 99508");
_Static_assert(RMS$_DAC       == 114706, "RMS$_DAC != VAX V7.3 oracle 114706");
_Static_assert(RMS$_DAN       ==  99516, "RMS$_DAN != VAX V7.3 oracle 99516");
_Static_assert(RMS$_IAN       ==  99668, "RMS$_IAN != VAX V7.3 oracle 99668");
_Static_assert(RMS$_RAC       ==  99908, "RMS$_RAC != VAX V7.3 oracle 99908");
_Static_assert(RMS$_RPL       == 114948, "RMS$_RPL != VAX V7.3 oracle 114948");
_Static_assert(RMS$_WPL       == 114972, "RMS$_WPL != VAX V7.3 oracle 114972");
_Static_assert(RMS$_NAM       ==  99804, "RMS$_NAM != VAX V7.3 oracle 99804");
_Static_assert(RMS$_SUC == RMS$_NORMAL, "RMS$_SUC must alias RMS$_NORMAL");
/* Two header symbols are NOT in the V7.3 $RMSDEF dump — left UNGROUNDED.
 * Assert OVMX's current values only as a drift guard, not as VMS-authentic. */
_Static_assert(RMS$_BSZ       ==  98858, "RMS$_BSZ drifted from OVMX-chosen 98858 (ungrounded: absent from V7.3 $RMSDEF)");
_Static_assert(RMS$_OK_RRV    ==  98865, "RMS$_OK_RRV drifted from OVMX-chosen 98865 (ungrounded: absent from V7.3 $RMSDEF)");

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
