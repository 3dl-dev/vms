/*
 * RMSDEF.H - VMS Record Management Services (RMS) Status Code Definitions
 *
 * OpenVMX compatibility layer - Defines the RMS$_ condition values
 * returned by RMS operations (file open, close, read, write, etc.).
 *
 * RMS status codes use the same 32-bit structure as system service
 * status codes (see STSDEF.H), with facility number 1 (RMS).
 *
 * RMS returns two classes of status codes:
 *   - Success codes (odd values): RMS$_NORMAL, RMS$_OK_xxx
 *   - Error codes (even values):  RMS$_FNF, RMS$_EOF, etc.
 *
 * The secondary status (STV field in FAB/RAB) provides additional
 * detail about the nature of the error.
 *
 * Reference: OpenVMS Record Management Services Reference Manual
 *            Guide to OpenVMS File Applications
 */

#ifndef __RMSDEF_H
#define __RMSDEF_H

#include <stdint.h>
#include "stsdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * RMS Facility number
 * ================================================================ */

#define RMS$_FACILITY       1       /* RMS facility number */

/* ================================================================
 * RMS Success status codes (severity 1 = success, bit 0 set)
 * ================================================================ */

#define RMS$_NORMAL         65537   /* Successful completion */
#define RMS$_SUC            65537   /* Alias for NORMAL */
#define RMS$_OK_ALK         98361   /* Record already locked */
#define RMS$_OK_DEL         98369   /* Deleted record successfully read */
#define RMS$_OK_RLK         98337   /* Record successfully read, record locked */
#define RMS$_OK_RRL         98345   /* Record successfully read, read-locked */
#define RMS$_OK_DUP         98321   /* Duplicate key detected, record stored */
#define RMS$_OK_LIM         98385   /* Retrieved record exceeds key limit */
#define RMS$_OK_NOP         98393   /* No operation (noop success) */
#define RMS$_OK_WAT         98401   /* Record locked, waiting */

/* ================================================================
 * RMS Error status codes - File errors
 * ================================================================ */

#define RMS$_ACC            114690  /* File access error (ACP) */
#define RMS$_CRE            114698  /* File create error */
#define RMS$_BKZ            99364   /* Bucket size error */
#define RMS$_BLN            99372   /* Block length invalid (bad BLN) */
#define RMS$_BSZ            98858   /* Bad byte size */
#define RMS$_CCR            99476   /* Cannot connect RAB */
#define RMS$_BUG            99380   /* Internal RMS bug check */
#define RMS$_CHG            99484   /* Key field changed (not allowed) */
#define RMS$_DUP            99564   /* Duplicate key value, not allowed */
#define RMS$_DEL            98914   /* Error deleting record */
#define RMS$_DIR            99532   /* Error in directory name */
#define RMS$_FAC            99604   /* File access (FAC) violation */
#define RMS$_IMX            99692   /* Index not initialized */
#define RMS$_IOP            99700   /* Illegal operation */
#define RMS$_RER            114932  /* File read error */
#define RMS$_EOF            98938   /* End of file */
#define RMS$_KEY            99732   /* Key value error / invalid key */
#define RMS$_MRN            99788   /* Record number exceeds maximum */
#define RMS$_FLK            98954   /* File locked by another user */
#define RMS$_ESS            99588   /* Expanded string area too short */
#define RMS$_EXT            114722  /* File extension error */
#define RMS$_FNF            98962   /* File not found */
#define RMS$_FAB            99596   /* Not a valid FAB */
#define RMS$_DNF            114762  /* Directory not found */
#define RMS$_RNL            98720   /* Record not locked */
#define RMS$_RLK            98986   /* Record locked */
#define RMS$_IFI            99684   /* Invalid internal file identifier */
#define RMS$_RNF            98994   /* Record not found */
#define RMS$_ISI            99716   /* Invalid internal stream identifier */
#define RMS$_REX            98978   /* Record already exists */
#define RMS$_PRV            98970   /* Insufficient privilege */
#define RMS$_FEX            98946   /* File already exists */
#define RMS$_KRF            99740   /* Invalid key reference */
#define RMS$_KSZ            99748   /* Invalid key size */
#define RMS$_RSZ            100004  /* Invalid record size */
#define RMS$_FNM            99628   /* File name error */
#define RMS$_SHR            100020  /* File sharing conflict */
#define RMS$_WER            114964  /* File write error */
#define RMS$_MKD            114738  /* Bad key definition (XAB) */
#define RMS$_NEF            99812   /* Not positioned to EOF */

/* ================================================================
 * Additional RMS error status codes
 * ================================================================ */

#define RMS$_ORG            99852   /* Invalid file organization */
#define RMS$_PLG            99868   /* File prologue error */
#define RMS$_RAB            99900   /* Not a valid RAB */
#define RMS$_RAT            99916   /* Invalid record attributes */
#define RMS$_RFM            99940   /* Invalid record format */
#define RMS$_RSS            99988   /* Invalid resultant string size */
#define RMS$_RTB            98728   /* Record too big for buffer */
#define RMS$_SEQ            100012  /* Record not sequential */
#define RMS$_SIZ            100028  /* Invalid size value */
#define RMS$_SYN            100052  /* Syntax error in filespec */
#define RMS$_TNS            98744   /* Terminator not seen (partial record) */
#define RMS$_TRE            100060  /* Index tree error */
#define RMS$_TYP            100068  /* Invalid file type */
#define RMS$_WCC            99050   /* Invalid wildcard context */
#define RMS$_DME            99540   /* Dynamic memory exhausted */

/* ================================================================
 * RMS codes returned on wildcard operations
 * ================================================================ */

#define RMS$_NMF            99018   /* No more files (wildcard exhausted) */

/* ================================================================
 * RMS informational/warning codes
 * ================================================================ */

#define RMS$_CRE_STM        98409   /* File created as stream */
#define RMS$_CREATED        67097   /* File was created */
#define RMS$_FILEPURGED     67193   /* Previous version purged */
#define RMS$_SUPERSEDE      67121   /* File superseded */
#define RMS$_OK_RRV         98865   /* Success, record in RRV */
#define RMS$_COD            99500   /* Invalid code value */
#define RMS$_CUR            99508   /* No current record */
#define RMS$_DAC            114706  /* File deaccess error */
#define RMS$_DAN            99516   /* Data area number error */
#define RMS$_IAN            99668   /* Index area number error */
#define RMS$_RAC            99908   /* Invalid record access */
#define RMS$_RPL            114948  /* Error reading prologue */
#define RMS$_WPL            114972  /* Error writing prologue */
#define RMS$_NAM            99804   /* Bad NAM block */

/* ================================================================
 * RMS status testing macros
 * ================================================================ */

/* ================================================================
 * RMS journaling / recovery-unit status codes (vms-f16).
 *
 * ORACLE-PINNED, 2026-08-13, on OpenVMS VAX V7.3 (lab-2 node VAX1) by
 * assembling the public $RMSDEF macro as GLOBAL symbols and reading
 * the defined longword out of the object GSD with ANALYZE/OBJECT/GSD
 * (documented tool output, Rule 8).  Well-known anchors verified in
 * the same dump: RMS$_NORMAL=65537, RMS$_EOF=98938, RMS$_FNF=98962.
 *
 * FIXED under vms-a7d (2026-08-13): the SAME oracle method was rerun on
 * lab-2 (node vaxlab-2/vax1, OpenVMS VAX V7.3) -- MACRO/OBJECT of
 * `$RMSDEF GLOBAL` then ANALYZE/OBJECT/GSD, ANALYZ V07-04.  267 RMS$_
 * global symbols were read out of the object GSD; 74 EXISTING values in
 * this header disagreed with the oracle (they had been synthesised as
 * arithmetic +8 sequences, never grounded) and were corrected in place
 * to the dumped longwords.  Examples: RMS$_ACC 98826->114690 (0x1C002),
 * RMS$_CRE 98834->114698 (0x1C00A), RMS$_RNF 98986->98994 (0x182B2),
 * RMS$_RTB 99108->98728 (0x181A8).  Wrong aliases were split to their
 * distinct oracle values (RMS$_KEY!=EOF, RMS$_DNF!=RNL, RMS$_SHR!=WER,
 * RMS$_RAB!=NAM, RMS$_PLG!=WPL).  Every RMS$_ error/success/info code in
 * this header is now oracle-pinned to that dump; success/failure parity
 * (bit 0) was preserved for all 74.  Two header symbols NOT present in
 * the V7.3 $RMSDEF dump -- RMS$_BSZ (98858) and RMS$_OK_RRV (98865) --
 * were left unchanged and remain UNGROUNDED (no oracle definition).
 * The pinned values are checked at compile time in
 * tests/libvms/test_conformance_constants.c.
 * ================================================================ */

#define RMS$_ACC_RUJ        115044  /* Recovery-unit journal access error */
#define RMS$_JNLNOTAUTH     115100  /* Journaling not authorized on the file */

/* Test if RMS status indicates success */
#define RMS$SUCCESS(sts)    ((sts) & 1)

/* Test if RMS status indicates failure */
#define RMS$FAILURE(sts)    (!((sts) & 1))

/* Match two RMS status codes ignoring severity */
#define RMS$MATCH(sts1, sts2) STS$MATCH(sts1, sts2)

#ifdef __cplusplus
}
#endif

#endif /* __RMSDEF_H */
