/*
 * test_lock_flag_bits.c — vms-982
 *
 * Pins every LCK$M_ / LCK$K_ constant in <lckdef.h> to the AUTHENTIC OpenVMS
 * $LCKDEF layout, so the nine wrong bits this header used to carry (several
 * SWAPPED with each other) can never silently return. $ENQ/$DEQ behaviour keys
 * off these flags, so DLM correctness sits directly on top of them.
 *
 * GROUNDING (clean-room Rule 8): the expected values below were pinned by TWO
 * independent documented-tool methods on the OpenVMS VAX V7.3 reference oracle
 * (lab-2 vaxlab-7), 2026-08-10. $LCKDEF is architecture-independent, so a VAX
 * oracle is authoritative for these bits.
 *
 *   Method A — LIBRARIAN extract of the definition macro from the OS macro lib:
 *     $ LIBRARY/EXTRACT=$LCKDEF/OUTPUT=SYS$SCRATCH:LCKDEF.MAR SYS$LIBRARY:STARLET.MLB
 *     $ TYPE SYS$SCRATCH:LCKDEF.MAR
 *       -> $EQU LCK$M_VALBLK 1  ... NOQUEUE 4  SYNCSTS 8  ... PROTECT 256
 *          NODLCKWT 512  EXPEDITE 2048  QUECVT 4096  BYPASS 8192
 *          (DEQ field) DEQALL 1  CANCEL 2  INVVALBLK 4
 *
 *   Method B — MACRO-32 assembler symbol table (independent of LIBRARIAN):
 *     $ CREATE SYS$SCRATCH:LCKT.MAR  [ "<TAB>$LCKDEF GLOBAL" / "<TAB>.END" ]
 *     $ MACRO/LIST=SYS$SCRATCH:LCKT.LIS SYS$SCRATCH:LCKT.MAR
 *     $ SEARCH SYS$SCRATCH:LCKT.LIS "LCK$M_"
 *       -> LCK$M_NOQUEUE=00000004  LCK$M_SYNCSTS=00000008  LCK$M_PROTECT=00000100
 *          LCK$M_NODLCKWT=00000200  LCK$M_EXPEDITE=00000800  LCK$M_QUECVT=00001000
 *          LCK$M_BYPASS=00002000  LCK$M_DEQALL=00000001  LCK$M_INVVALBLK=00000004 ...
 *
 * Both methods agreed exactly on all 14 $ENQ flags, 3 $DEQ flags, and 6 modes.
 * The assertions are compile-time (_Static_assert): drift fails the build.
 */

#include <stdio.h>
#include <lckdef.h>
#include <starlet.h>   /* second in-tree copy — must agree bit-for-bit */

/* ---- $ENQ/$ENQW flag bits (oracle: SYS$LIBRARY:STARLET.MLB $LCKDEF) ---- */
_Static_assert(LCK$M_VALBLK    == 0x0001, "LCK$M_VALBLK != oracle 0x0001");
_Static_assert(LCK$M_CONVERT   == 0x0002, "LCK$M_CONVERT != oracle 0x0002");
_Static_assert(LCK$M_NOQUEUE   == 0x0004, "LCK$M_NOQUEUE != oracle 0x0004");  /* was 0x0008 */
_Static_assert(LCK$M_SYNCSTS   == 0x0008, "LCK$M_SYNCSTS != oracle 0x0008");  /* was 0x0400 */
_Static_assert(LCK$M_SYSTEM    == 0x0010, "LCK$M_SYSTEM != oracle 0x0010");
_Static_assert(LCK$M_NOQUOTA   == 0x0020, "LCK$M_NOQUOTA != oracle 0x0020");
_Static_assert(LCK$M_CVTSYS    == 0x0040, "LCK$M_CVTSYS != oracle 0x0040");
_Static_assert(LCK$M_RECOVER   == 0x0080, "LCK$M_RECOVER != oracle 0x0080");
_Static_assert(LCK$M_PROTECT   == 0x0100, "LCK$M_PROTECT != oracle 0x0100");  /* was 0x0080 */
_Static_assert(LCK$M_NODLCKWT  == 0x0200, "LCK$M_NODLCKWT != oracle 0x0200"); /* was 0x0004 */
_Static_assert(LCK$M_NODLCKBLK == 0x0400, "LCK$M_NODLCKBLK != oracle 0x0400");
_Static_assert(LCK$M_EXPEDITE  == 0x0800, "LCK$M_EXPEDITE != oracle 0x0800"); /* was 0x0040 */
_Static_assert(LCK$M_QUECVT    == 0x1000, "LCK$M_QUECVT != oracle 0x1000");   /* slot was LCK$M_CLUSTER (bogus) */
_Static_assert(LCK$M_BYPASS    == 0x2000, "LCK$M_BYPASS != oracle 0x2000");   /* slot was LCK$M_RQJOBIDFL (bogus) */

/* ---- $DEQ flag bits: SEPARATE field, restarts at bit 0 (legitimately
 *      aliases the low $ENQ bits — this is authentic $LCKDEF) ---- */
_Static_assert(LCK$M_DEQALL    == 0x0001, "LCK$M_DEQALL != oracle 0x0001");    /* was 0x0100 */
_Static_assert(LCK$M_CANCEL    == 0x0002, "LCK$M_CANCEL != oracle 0x0002");
_Static_assert(LCK$M_INVVALBLK == 0x0004, "LCK$M_INVVALBLK != oracle 0x0004"); /* was 0x0200 */

/* ---- Lock grant modes (oracle: same $LCKDEF extract) ---- */
_Static_assert(LCK$K_NLMODE == 0, "LCK$K_NLMODE != 0");
_Static_assert(LCK$K_CRMODE == 1, "LCK$K_CRMODE != 1");
_Static_assert(LCK$K_CWMODE == 2, "LCK$K_CWMODE != 2");
_Static_assert(LCK$K_PRMODE == 3, "LCK$K_PRMODE != 3");
_Static_assert(LCK$K_PWMODE == 4, "LCK$K_PWMODE != 4");
_Static_assert(LCK$K_EXMODE == 5, "LCK$K_EXMODE != 5");

/* ---- Cross-header consistency (vms-6d3): <lckdef.h> and <starlet.h> are two
 *      copies of the same $LCKDEF; they must not disagree. Because both are
 *      included above, a *differing* redefinition would already have failed to
 *      compile; these assertions make the contract explicit and self-documenting
 *      for the shared $ENQ flag surface. ---- */
_Static_assert(LCK$M_NOQUEUE  == 0x0004 && LCK$M_SYNCSTS == 0x0008 &&
               LCK$M_NODLCKWT == 0x0200 && LCK$M_EXPEDITE == 0x0800 &&
               LCK$M_PROTECT  == 0x0100 && LCK$M_QUECVT   == 0x1000 &&
               LCK$M_BYPASS   == 0x2000,
               "lckdef.h and starlet.h disagree on the $ENQ flag layout");

int main(void)
{
    printf("All LCK$M_/LCK$K_ constants match the OpenVMS VAX V7.3 oracle "
           "$LCKDEF (LIBRARIAN + MACRO-32, lab-2 vaxlab-7 2026-08-10).\n");
    return 0;
}
