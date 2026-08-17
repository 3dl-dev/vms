/*
 * SSDEF.H - VMS System Service Status Code Definitions
 *
 * OpenVMX compatibility layer - Defines the SS$_ condition values
 * returned by VMS system services.
 *
 * Status values are 32-bit longwords with the structure defined
 * in STSDEF.H:
 *   Bits  0-2:  Severity (0=warning, 1=success, 2=error, 3=info, 4=severe)
 *   Bits  3-15: Message number
 *   Bits 16-27: Facility number (0 = SYSTEM)
 *   Bit   28:   Customer bit
 *   Bits 29-31: Reserved
 *
 * System service status codes use facility number 0 (SYSTEM).
 * The actual numeric values below match the real OpenVMS definitions.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS System Messages and Recovery Procedures Reference
 */

#ifndef __SSDEF_H
#define __SSDEF_H

#include <stdint.h>
#include "stsdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * DEC C boolean convention (TRUE / FALSE)
 *
 * TRUE and FALSE are not ISO C keywords; the Eight-Cubed VMS C corpus
 * (and much DEC C source) assumes them ambient.  OpenVMS does NOT
 * publish a single authoritative header home for them -- they are the
 * universal DEC C / K&R boolean convention TRUE == 1, FALSE == 0
 * (documented throughout the VSI DEC C RTL Reference).  OVMX therefore
 * defines them here (an OVMX compatibility choice, per Rule 8) in the
 * header that every corpus consumer already includes, guarded so a
 * translation unit that defines its own copy still wins.
 * ================================================================ */

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif

/* ================================================================
 * Success status codes (severity = 1, bit 0 set)
 * ================================================================ */

#define SS$_NORMAL          1       /* Normal successful completion */
/* ORACLE-PINNED (vms-68c, 2026-07-30), docs/oracle/vax73-event-flags.md.
 * SS$_WASCLR REALLY IS SS$_NORMAL -- this is VMS, not an OVMX placeholder
 * and not a collision to be resolved:
 *     $EQU  SS$_NORMAL  1
 *     $EQU  SS$_WASCLR  1      (both, from $SSDEF in STARLET.MLB)
 *     F$MESSAGE(1) -> %SYSTEM-S-NORMAL, normal successful completion
 * There is exactly one message at value 1, so nothing renders as "WASCLR".
 * A caller of $SETEF/$CLREF/$READEF distinguishes the two outcomes by
 * testing against SS$_WASSET (9); "not WASSET, and success" IS was-clear.
 * vms-68c was filed on the premise that this alias was the defect. It is
 * not -- the defect was src/kernel/vms_internal.h's SS__WASCLR 5, which the
 * same oracle shows is not a status at all (F$MESSAGE(5) = %NONAME-?-NOMSG).
 * Do not "disambiguate" these by inventing a distinct value. */
#define SS$_WASCLR          1       /* Previous state was clear (== SS$_NORMAL on VMS) */
#define SS$_WASSET          9       /* Previous state was set */
#define SS$_BUFFEROVF       4       /* Buffer overflow (warning, sev=0) */

/* ================================================================
 * Error/failure status codes
 *
 * Values match the real VMS system message file.  The severity
 * is encoded in bits 0-2 of each value.
 * ================================================================ */

#define SS$_ACCVIO          12      /* Access violation */
#define SS$_BADPARAM        20      /* Bad parameter value */
#define SS$_EXQUOTA         28      /* Exceeded quota */
#define SS$_NOPRIV          36      /* No privilege for attempted operation */
#define SS$_ABORT           44      /* Abort */

/* ================================================================
 * Warning status codes (severity = 0)
 * ================================================================ */

#define SS$_IVTIME          388     /* Invalid time */
/* ORACLE-PINNED (vms-8019, see the block above SS$_IVLOGNAM below).
 * Real severity is F (148 & 7 == 4), not W -- it sits in this section
 * only because the section split predates the pinning. */
#define SS$_DUPLNAM         148     /* Duplicate name (%SYSTEM-F-DUPLNAM) */
#define SS$_NOLOGNAM        444     /* No logical name match */
/* ORACLE-PINNED (vms-2b8). MEASURED on the reference lab OpenVMS VAX
 * V7.3 node VAX1, 2026-07-30 (docs/oracle/vax73-privileges.md §1):
 *   $ WRITE SYS$OUTPUT "1664="+F$MESSAGE(1664)
 *   1664=%SYSTEM-W-NOTALLPRIV, not all requested privileges authorized
 * This was 532, which the SAME oracle session disproves:
 *   $ WRITE SYS$OUTPUT "532="+F$MESSAGE(532)
 *   532=%SYSTEM-F-RESULTOVF, resultant string overflow
 * Severity is W, matching the partial-success condition it reports. */
#define SS$_NOTALLPRIV      1664    /* Not all requested privileges authorized (%SYSTEM-W-NOTALLPRIV) */
#define SS$_IVIDENT         548     /* Invalid identifier */
/* ORACLE-PINNED (vms-8019) -- see the block above SS$_IVLOGNAM.
 * 564 is SS$_UNASEFC; this collision was created by pinning UNASEFC,
 * so it is that change's blast radius, not vms-c90's. */
#define SS$_IVSECFLG        364     /* Invalid process or global section flags (%SYSTEM-F-IVSECFLG) */

/* ================================================================
 * Error status codes (severity = 2)
 * ================================================================ */

#define SS$_INSFMEM         292     /* Insufficient dynamic memory */
#define SS$_TIMEOUT         556     /* Device timeout */
/* ORACLE-PINNED (vms-9fc, 2026-07-30) on reference lab node VAX1, OpenVMS
 * VAX V7.3, by the same two documented-tool observations used above:
 *     $EQU  SS$_ILLIOFUNC   244        (LIBRARY/EXTRACT=$SSDEF ... STARLET.MLB)
 *     F$MESSAGE(244) -> %SYSTEM-F-ILLIOFUNC, illegal I/O function code
 * The previous value here, 580, is a DIFFERENT condition on the oracle:
 *     F$MESSAGE(580) -> %SYSTEM-F-VASFULL, virtual address space is full
 * so every sys$qio that rejected an unimplemented function code was
 * reporting an address-space exhaustion.
 *
 * Most consumers name the symbol (src/libvms/status.c,
 * src/libvms/syssvc/sys_qio.c) and so follow this value automatically.
 * ONE DID NOT: DCL's F$MESSAGE table in src/vmsdcl/dcl_lexical.c is a
 * number->message table and hard-coded 580/'E'/ILLIOFUNC, so after this
 * correction F$MESSAGE could not name the status sys$qio returns and
 * still rendered "illegal I/O function" for VASFULL. Both rows are
 * corrected there against the same oracle run; a status whose number no
 * user-visible message table can name is a half-applied correction.
 * SS$_BUGCHECK 676 below was pinned by the same run and was already correct
 * ($EQU SS$_BUGCHECK 676; F$MESSAGE(676) -> %SYSTEM-F-BUGCHECK, internal
 * consistency failure). */
#define SS$_ILLIOFUNC       244     /* Illegal I/O function (%SYSTEM-F-ILLIOFUNC) */
#define SS$_NOMORENODE      588     /* No more cluster nodes (VMS: 0x24C) */
/* ================================================================
 * ORACLE-PINNED VALUES (vms-8019, 2026-07-30)
 *
 * Pinned on the reference lab node VAX1, OpenVMS VAX V7.3, by two
 * independent documented-tool observations:
 *
 *   LIBRARY/EXTRACT=$SSDEF/OUTPUT=SYS$SCRATCH:SSDEF.MAR
 *       SYS$LIBRARY:STARLET.MLB
 *   SEARCH SYS$SCRATCH:SSDEF.MAR "<symbol>"
 *       $EQU  SS$_DUPLNAM    148
 *       $EQU  SS$_IVLOGNAM   340
 *       $EQU  SS$_IVSECFLG   364
 *       $EQU  SS$_UNASEFC    564
 *       $EQU  SS$_VOLINV     596
 *       $EQU  SS$_POWERFAIL  868
 *       $EQU  SS$_SYNCH      1673
 *       $EQU  SS$_NONEXPR    2280
 *
 *   F$MESSAGE(148)  -> %SYSTEM-F-DUPLNAM,   duplicate name
 *   F$MESSAGE(340)  -> %SYSTEM-F-IVLOGNAM,  invalid logical name
 *   F$MESSAGE(364)  -> %SYSTEM-F-IVSECFLG,  invalid process or global section flags
 *   F$MESSAGE(564)  -> %SYSTEM-F-UNASEFC,   unassociated event flag cluster
 *   F$MESSAGE(596)  -> %SYSTEM-F-VOLINV,    volume is not software enabled
 *   F$MESSAGE(868)  -> %SYSTEM-F-POWERFAIL, power failure occurred
 *   F$MESSAGE(1673) -> %SYSTEM-S-SYNCH,     synchronous successful completion
 *   F$MESSAGE(2280) -> %SYSTEM-W-NONEXPR,   nonexistent process
 *
 * This retires the old note that "596 is taken by SS$_IVLOGNAM" and the
 * displaced SS$_POWERFAIL 598 that the note justified: 596 is VOLINV,
 * and 598 is VOLINV re-severitied, not a distinct condition at all.
 *
 * SS$_SYNCH, SS$_UNASEFC and SS$_IVSECFLG are pinned here NOT because
 * the process table uses them, but because pinning POWERFAIL/NONEXPR/
 * UNASEFC made their previous placeholder values ALIAS a pinned one
 * (868, 2280, 564 respectively). Two distinct VMS conditions cannot
 * share a status value, so a header that lets them is asserting a
 * contradiction -- and `status == SS$_SYNCH` would have matched
 * SS$_POWERFAIL. That is this change's own blast radius, not vms-c90's.
 *
 * NOTE the section headings below are NOT reliable severity labels --
 * several pinned values sit under a heading their real severity
 * contradicts (SS$_DUPLNAM is -F- but sits under "warning"). Reconciling
 * the headings, and the rest of the unpinned constants, is vms-c90.
 * ================================================================ */
#define SS$_IVLOGNAM        340     /* Invalid logical name (%SYSTEM-F-IVLOGNAM) */
#define SS$_VOLINV          596     /* Volume is not software enabled (%SYSTEM-F-VOLINV) */
#define SS$_POWERFAIL       868     /* Power failure occurred (%SYSTEM-F-POWERFAIL) */
/* ORACLE-PINNED (vms-2b8). MEASURED on OpenVMS VAX V7.3 node VAX1,
 * 2026-07-30 (docs/oracle/vax73-privileges.md §1):
 *   $ WRITE SYS$OUTPUT "532="+F$MESSAGE(532)
 *   532=%SYSTEM-F-RESULTOVF, resultant string overflow
 * Was 1364. Corrected here because leaving it would contradict the
 * SS$_NOTALLPRIV pin above, which vacated 532 in the same session.
 * Every in-tree consumer uses the SYMBOL, not the literal (vmsfs,
 * vmslnm, libvms/status.c), so the value change is transparent. */
#define SS$_RESULTOVF       532     /* Resultant string overflow (%SYSTEM-F-RESULTOVF) */
#define SS$_CANCEL          2096    /* I/O operation canceled */
#define SS$_ENDOFFILE       2160    /* End of file */
#define SS$_NOSUCHDEV       2680    /* No such device */
/* SS$_NOMOREDEV: needed as the sys$device_scan wildcard-scan-exhausted
 * terminator (see starlet.h). PROVENANCE: 0x0A58/2648, sourced this
 * session from a GCC-for-Alpha OpenVMS toolkit SSDEF.H mirror
 * (vsm.com.au/ftp/KITS/GCC-FOR-ALPHA/INCLUDE/VMS/SSDEF.H). NOT
 * independently cross-verified against a second lineage this session
 * (a spot-check against the Nankervis/ODS2 ssdef.h - github.com/simh/
 * simtools - found that source disagrees with this file's *existing*
 * SS$_NOSUCHDEV value, 2312 vs 2680, so multi-source drift is a live
 * risk here, not a hypothetical one). Flagged for operator sign-off
 * per docs/../memory vms-purity-guardrail; do not treat as authoritative
 * until confirmed. Tracked in vms-fb3 findings.
 */
#define SS$_NOMOREDEV       2648
#define SS$_DEVMOUNT        2684    /* Device already mounted */
#define SS$_DEVNOTMOUNT     2688    /* Device not mounted */
#define SS$_NOSUCHFILE      2696    /* No such file */

/* ================================================================
 * Additional commonly-used status codes
 * ================================================================ */

#define SS$_ITEMNOTFOUND    35820   /* Item not found */
#define SS$_BUGCHECK        676     /* Internal consistency failure */
#define SS$_FILALRACC       2736    /* File already accessed */
#define SS$_DEVOFFLINE      2692    /* Device offline */
#define SS$_DEVINACT        2704    /* Device inactive */
#define SS$_IVCHAN          602     /* Invalid channel */
#define SS$_IVDEVNAM        608     /* Invalid device name */
#define SS$_IVSSRQ          620     /* Invalid system service request */
#define SS$_SSFAIL          636     /* System service failure */
#define SS$_NOTRAN          2700    /* No translation for logical name */

/* ================================================================
 * Process-related status codes
 * ================================================================ */

/* ORACLE-PINNED (vms-8019) -- see the block above SS$_IVLOGNAM. */
#define SS$_NONEXPR         2280    /* Nonexistent process (%SYSTEM-W-NONEXPR) */
#define SS$_SUSPENDED       2584    /* Process suspended */
#define SS$_INCOMPAT        2632    /* Incompatible attributes */
#define SS$_NOSLOT          2732    /* No PCB slot available */

/* ================================================================
 * Condition handling status codes
 * ================================================================ */

#define SS$_CONTROLC        1617    /* Ctrl-C interrupt */
#define SS$_RESIGNAL        2328    /* Resignal condition */
#define SS$_UNWIND          2204    /* Unwind in progress */
#define SS$_CONTINUE        1       /* Continue execution (same as SS$_NORMAL) */

/* ================================================================
 * Success/informational status codes
 * ================================================================ */

#define SS$_CREATED         836     /* Object created */
#define SS$_SUPERSEDE       844     /* Object superseded */
#define SS$_CONCEALED       852     /* Concealed device */
#define SS$_REMOTE          860     /* Remote node */
/* ORACLE-PINNED (vms-8019) -- see the block above SS$_IVLOGNAM.
 * 868 is SS$_POWERFAIL; SS$_SYNCH is 1673, and it really is a success
 * status (1673 & 7 == 1 == STS$K_SUCCESS), so it belongs in this
 * section on its own merits. */
#define SS$_SYNCH           1673    /* Synchronous successful completion (%SYSTEM-S-SYNCH) */
#define SS$_OPINCOMPL       2552    /* Operation incomplete */

/* ================================================================
 * Lock manager status codes
 * ================================================================ */

/* Real OpenVMS $SSDEF condition values (VMS compatibility is source-of-truth).
 * Provenance: the Nankervis ODS2 ssdef.h lineage, redistributed via
 * vsm.com.au (GCC-for-Alpha kit) and github.com/ztmr/FreeVMS; verified
 * 2026-07-26. Single-lineage for the lock-specific codes (VSI/HPE manuals
 * publish the names but no numbers); severity-bit structure checks pass and
 * SS$_VALNOTVALID matches the VSI manual's "returned as a warning" wording.
 * Not independently confirmed against an official VSI $SSDEF extract. */
#define SS$_NOTQUEUED       2488    /* Not queued (0x9B8) */
#define SS$_DEADLOCK        3594    /* Deadlock detected (0xE0A) */
#define SS$_VALNOTVALID     2544    /* Value block not valid (0x9F0) */
#define SS$_PARNOTGRANT     716     /* Parent lock not granted (UNVERIFIED — not
                                     * in the researched set; see vms-b27 sweep) */
#define SS$_CVTUNGRANT      8508    /* Convert ungrantable (0x213C) */
#define SS$_IVLOCKID        8484    /* Invalid lock ID (0x2124) */
#define SS$_SUBLOCKS        8492    /* Sublocks still held (0x212C) */

/* ================================================================
 * Quota and resource status codes
 * ================================================================ */

#define SS$_EXENQLM         10820   /* Exceeded enqueue limit (0x2A44) — authentic,
                                     * per the ssdef.h lineage above. Neighbours
                                     * EXASTLM/EXBYTLM below are UNVERIFIED (see
                                     * the broader ssdef fidelity sweep). */
/* SS$_EXASTLM's value below is UNCHANGED (2756) but its provenance is a
 * DECLINE TO PIN, not a confirmation -- record this so the next reader does
 * not mistake "value didn't move" for "value was verified" (vms-cd41,
 * vms-2e5). The kernel's own SS__EXASTLM (src/kernel/vms_internal.h) is a
 * DIFFERENT number (56/0x38). src/kernel/vms_ast.c's vms_ioctl_dclast
 * REALLY DOES set that raw status when the AST quota is exceeded -- this is
 * a live kernel condition, not a hypothetical one -- but nothing translates
 * it to this public constant yet: src/libvmssys/vms_kif.c:vms_kif_dclast
 * returns the raw kernel value unmapped, and it is OVMX-UNWIRED (vms-as1,
 * declared in vms_kif.h) -- no product path calls it. sys_lock.c's
 * kstat_to_ss() is the model for the translation this will eventually need;
 * no equivalent exists yet for the AST family.
 * docs/api-system-services.md was checked as a candidate oracle for 2756
 * and REJECTED: on the same page (docs/api-system-services.md ~line 2498),
 * it lists SS$_DEADLOCK as 708 and SS$_EXENQLM as 2748 -- both of which
 * disagree with THIS file's own already oracle-pinned values for those two
 * names (SS$_DEADLOCK 3594 above, SS$_EXENQLM 10820 immediately above this
 * comment). A source shown wrong on two neighbours it disagrees with is not
 * good corroboration for the one entry (EXASTLM) where it happens to agree
 * with the number already here -- agreement with an unverified value proves
 * nothing. Per Rule 10, do not self-certify: 2756 stays UNVERIFIED until
 * confirmed from ~/vax/cluster (LIBRARY/EXTRACT=$SSDEF from STARLET.MLB) or
 * a public OpenVMS doc source that does NOT already disagree with a pinned
 * neighbour. Do not "fix" this by inventing a different number either --
 * that is the same self-certification error in the other direction. */
#define SS$_EXASTLM         2756    /* Exceeded AST limit -- UNVERIFIED, see
                                     * the block above (vms-cd41) */
#define SS$_EXBYTLM         2764    /* Exceeded byte count limit */

/* ================================================================
 * Privilege and security status codes
 * ================================================================ */

#define SS$_NOCMKRNL        2212    /* No CMKRNL privilege */
#define SS$_NOCMEXEC        2216    /* No CMEXEC privilege */
#define SS$_NOSYSNAM        2220    /* No SYSNAM privilege */
#define SS$_NOGRACELOGIN    2224    /* No grace login */
#define SS$_INVLOGIN        2228    /* Invalid login */
#define SS$_NOSUCHID        2580    /* No such user identifier */

/* ================================================================
 * Timer and AST status codes
 * ================================================================ */

#define SS$_ASTFLT          2244    /* AST fault */
/* ORACLE-PINNED (vms-68c, 2026-07-30) on reference lab node VAX1, OpenVMS
 * VAX V7.3, by the two documented-tool observations used throughout this
 * header (full transcript: docs/oracle/vax73-event-flags.md):
 *     $EQU  SS$_ILLEFC  236              (LIBRARY/EXTRACT=$SSDEF ... STARLET.MLB)
 *     F$MESSAGE(236) -> %SYSTEM-F-ILLEFC, illegal event flag cluster
 * The previous value here, 2260, is a DIFFERENT condition on the oracle:
 *     F$MESSAGE(2260) -> %SYSTEM-F-IDXFILEFULL, index file is full
 * so every $SETEF/$CLREF/$READEF that rejected an out-of-range event flag
 * number was reporting a full index file. src/kernel/vms_internal.h carried
 * a THIRD value for the same symbol (44 = %SYSTEM-F-ABORT); it is corrected
 * to 236 in the same change, so the two sides of /dev/vms now agree.
 *
 * Every in-tree consumer names the symbol (src/libvms/syssvc/sys_event.c,
 * src/vmsprocess/event_flags.c, src/libvms/status.c), so they follow this
 * value automatically. DCL's F$MESSAGE table in src/vmsdcl/dcl_lexical.c is
 * a number->message table and named neither ILLEFC nor UNASEFC at all; both
 * rows are added there against this same oracle run, because a status whose
 * number no user-visible message table can name is a half-applied
 * correction (the lesson of the SS$_ILLIOFUNC pin above). */
#define SS$_ILLEFC          236     /* Illegal event flag cluster (%SYSTEM-F-ILLEFC) */
/* ORACLE-PINNED (vms-8019) -- see the block above SS$_IVLOGNAM.
 * 2280 is SS$_NONEXPR. */
#define SS$_UNASEFC         564     /* Unassociated event flag cluster (%SYSTEM-F-UNASEFC) */

/* ================================================================
 * I/O-related status codes
 * ================================================================ */

#define SS$_ENDOFTAPE       2164    /* End of tape */
#define SS$_DATACHECK       2168    /* Data check error */
#define SS$_PARITY          2172    /* Parity error */
#define SS$_NOREADER        2176    /* No reader on mailbox */
#define SS$_NOWRITER        2180    /* No writer on mailbox */
#define SS$_NOMSG           2184    /* No message */

/* ================================================================
 * CLI-related status codes
 * ================================================================ */

#define SS$_IVVERB          2284    /* Invalid verb */
#define SS$_IVQUAL          2288    /* Invalid qualifier */
#define SS$_IVKEYW          2292    /* Invalid keyword */

/* ================================================================
 * Miscellaneous status codes
 * ================================================================ */

#define SS$_UNSUPPORTED     2296    /* Unsupported operation */
#define SS$_ACCVIO_RO       2340    /* Read-only access violation */
#define SS$_PAGOWNVIO       2344    /* Page owner violation */
#define SS$_NOSOLICIT       4268    /* No solicitation */
#define SS$_FILNOTACC       2744    /* File not accessed */
#define SS$_IVMODE          2300    /* Invalid access mode */
#define SS$_CHANINTLK       2304    /* Channel interlock */
#define SS$_MSGNOTFND       2308    /* Message not found */

/* Additional status codes */
#define SS$_FILACCERR       2312    /* File access error */
/*
 * SS$_DEVALLOC / SS$_DEVNOTALLOC.
 *
 * PROVENANCE: measured on the ~/vax OpenVMS VAX V7.3 lab (node VAX2,
 * 30-JUL-2026) by asking VMS's own message facility for the text of
 * each condition value -- `WRITE SYS$OUTPUT F$MESSAGE(n)` -- and
 * scanning for the name. VMS answered:
 *     2112  %SYSTEM-W-DEVALLOC, device already allocated to another user
 *     2116  %SYSTEM-F-DEVALLOC, device already allocated to another user
 *     2136  %SYSTEM-W-DEVNOTALLOC, device not allocated
 *     2140  %SYSTEM-F-DEVNOTALLOC, device not allocated
 * and the behaviour was confirmed end to end: `ALLOCATE OPA0:` issued
 * from a second (detached) process while the interactive job held the
 * console printed exactly
 *     %SYSTEM-W-DEVALLOC, device already allocated to another user
 * and a second `DEALLOCATE` of an already-deallocated device printed
 *     %SYSTEM-W-DEVNOTALLOC, device not allocated
 * (docs/oracle/vax73-terminal-device.md sections 7-9). The warning
 * form is the one $SSDEF carries, so that is the value used here.
 *
 * The previous value on this line, 2316, was wrong: the same probe
 * shows 2316 is %SYSTEM-F-NOSUCHDEV. That measurement also disagrees
 * with several OTHER values in this file (see the note on
 * SS$_NOSUCHDEV above); correcting the rest has a blast radius across
 * the kernel module and its tests and is tracked separately, not done
 * here.
 *
 * SS$_DEVALLOC already had two consumers when this value was
 * corrected -- src/vmsdcl/dcl_cmd_misc.c and src/vmsfs/vmsfs_device.c.
 * Both name the symbol rather than the number, so neither breaks.
 * Noted because an earlier version of this comment said the constant
 * had no other consumer, which was simply false. Separately, and NOT
 * fixed here because it is out of this change's scope:
 * vmsfs_device.c returns SS$_DEVALLOC for a FULL DEVICE TABLE, which
 * is the wrong condition entirely -- carried in vms-d0b's findings.
 */
#define SS$_DEVALLOC        2112    /* Device already allocated to another user */
#define SS$_DEVNOTALLOC     2136    /* Device not allocated */
#define SS$_IVLOGTAB        2320    /* Invalid logical name table */
#define SS$_NOLOGTAB        2324    /* No such logical name table */

/* ================================================================
 * Additional SYSTEM-facility condition values (vms-f16).
 *
 * ORACLE-PINNED, 2026-08-13, by assembling the public $SSDEF macro
 * (STARLET.MLB) as a module of GLOBAL symbols and reading the exact
 * defined longword out of the object's GSD with ANALYZE/OBJECT/GSD --
 * documented tool output, not disassembly (Rule 8).  Anchors
 * SS$_NORMAL=1 and SS$_ACCVIO=12 verified against the same dump.
 *
 * All values below except SS$_EXITFORCED and SS$_LOWPREC come from
 * OpenVMS VAX V7.3 (lab-2 node VAX1).  SS$_EXITFORCED and SS$_LOWPREC
 * do not exist in VAX V7.3 $SSDEF; their values come from OpenVMS
 * Alpha V8.4 (lab-Alpha node ALPHA1) -- SYSTEM-facility condition
 * values are architecture-invariant (SS$_ACCVIO=12 on both).
 * ================================================================ */

#define SS$_LKWSETFUL       404     /* Locked working set is full */
#define SS$_ALIGN           1292    /* Alignment fault */
#define SS$_DEVALRALLOC     1601    /* Device already allocated */
#define SS$_LOWPREC         1873    /* Low precision (Alpha V8.4 oracle) */
#define SS$_NOMOREPROC      2472    /* No more processes (end of $GETJPI wildcard) */
/*
 * SS$_NOMOREFILES (%X0930 == 2352) -- ORACLE-PINNED (vms-a0b, 2026-08-17).
 * MEASURED on the reference lab OpenVMS VAX V7.3 node VAX1 by assembling
 * `.LONG SS$_NOMOREFILES` after `$SSDEF` and reading the resolved absolute
 * value from the MACRO/LIST listing + symbol table:
 *   SS$_NOMOREFILES = 00000930   (2352.)
 * Cross-checked in the same run: SS$_NOMOREDEV = 00000A58 (2648) -- which
 * matches this header's existing SS$_NOMOREDEV exactly, confirming the method.
 * The Files-11 ODS-2 ACP's IO$_ACPCONTROL wildcard directory search
 * ($SEARCH primitive) returns it when the wildcard context is exhausted
 * (VSI OpenVMS I/O User's Reference Manual, "ACP-QIO Interface").
 */
#define SS$_NOMOREFILES     2352    /* No more files (wildcard $SEARCH exhausted) */
#define SS$_DUPIDENT        8748    /* Duplicate identifier */
#define SS$_NOSUCHCPU       9028    /* No such CPU */
#define SS$_NOCALLPRIV      9284    /* No privilege for calling access mode */
#define SS$_NOLOG           9332    /* Logging is not enabled */
#define SS$_NOIMPERSONATE   10284   /* No impersonate privilege */
#define SS$_NOOPER          10388   /* No operator privilege */
#define SS$_EXITFORCED      11220   /* Forced exit occurred (Alpha V8.4 oracle) */
#define SS$_USERDISABLED    11290   /* User account is disabled */

/* ================================================================
 * Status testing macros
 *
 * These are provided here for convenience since many VMS programs
 * include only SSDEF.H without STSDEF.H.
 * ================================================================ */

#ifndef $VMS_STATUS_SUCCESS
#define $VMS_STATUS_SUCCESS(code)   ((code) & 1)
#endif

#ifndef $VMS_STATUS_SEVERITY
#define $VMS_STATUS_SEVERITY(code)  ((code) & 7)
#endif

#ifndef $VMS_STATUS_FAC_NO
#define $VMS_STATUS_FAC_NO(code)    (((code) >> 16) & 0xFFF)
#endif

#ifndef $VMS_STATUS_CODE
#define $VMS_STATUS_CODE(code)      (((code) >> 3) & 0x1FFF)
#endif

#ifndef $VMS_STATUS_FAC_SP
#define $VMS_STATUS_FAC_SP(code)    (((code) >> 15) & 1)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __SSDEF_H */
