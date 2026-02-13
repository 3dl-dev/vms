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
#define RMS$_OK_ALK         65545   /* Record already locked */
#define RMS$_OK_DEL         65553   /* Deleted record successfully read */
#define RMS$_OK_RLK         65561   /* Record successfully read, record locked */
#define RMS$_OK_RRL         65569   /* Record successfully read, read-locked */
#define RMS$_OK_DUP         65577   /* Duplicate key detected, record stored */
#define RMS$_OK_LIM         65585   /* Retrieved record exceeds key limit */
#define RMS$_OK_NOP         65593   /* No operation (noop success) */
#define RMS$_OK_WAT         65601   /* Record locked, waiting */

/* ================================================================
 * RMS Error status codes - File errors
 * ================================================================ */

#define RMS$_ACC            98826   /* File access error (ACP) */
#define RMS$_CRE            98834   /* File create error */
#define RMS$_BKZ            98842   /* Bucket size error */
#define RMS$_BLN            98850   /* Block length invalid (bad BLN) */
#define RMS$_BSZ            98858   /* Bad byte size */
#define RMS$_CCR            98866   /* Cannot connect RAB */
#define RMS$_BUG            98874   /* Internal RMS bug check */
#define RMS$_CHG            98882   /* Key field changed (not allowed) */
#define RMS$_DUP            98890   /* Duplicate key value, not allowed */
#define RMS$_DEL            98898   /* Error deleting record */
#define RMS$_DIR            98904   /* Error in directory name */
#define RMS$_FAC            98906   /* File access (FAC) violation */
#define RMS$_IMX            98914   /* Index not initialized */
#define RMS$_IOP            98922   /* Illegal operation */
#define RMS$_RER            98930   /* File read error */
#define RMS$_EOF            98938   /* End of file */
#define RMS$_KEY            98938   /* Key value error / invalid key */
#define RMS$_MRN            98946   /* Record number exceeds maximum */
#define RMS$_FLK            98948   /* File locked by another user */
#define RMS$_ESS            98954   /* Expanded string area too short */
#define RMS$_EXT            98956   /* File extension error */
#define RMS$_FNF            98962   /* File not found */
#define RMS$_FAB            98964   /* Not a valid FAB */
#define RMS$_DNF            98970   /* Directory not found */
#define RMS$_RNL            98970   /* Record not locked */
#define RMS$_RLK            98978   /* Record locked */
#define RMS$_IFI            98980   /* Invalid internal file identifier */
#define RMS$_RNF            98986   /* Record not found */
#define RMS$_ISI            98988   /* Invalid internal stream identifier */
#define RMS$_REX            98994   /* Record already exists */
#define RMS$_PRV            98996   /* Insufficient privilege */
#define RMS$_FEX            99002   /* File already exists */
#define RMS$_KRF            99004   /* Invalid key reference */
#define RMS$_KSZ            99006   /* Invalid key size */
#define RMS$_RSZ            99010   /* Invalid record size */
#define RMS$_FNM            99012   /* File name error */
#define RMS$_SHR            99018   /* File sharing conflict */
#define RMS$_WER            99018   /* File write error */
#define RMS$_MKD            99020   /* Bad key definition (XAB) */
#define RMS$_NEF            99028   /* Not positioned to EOF */

/* ================================================================
 * Additional RMS error status codes
 * ================================================================ */

#define RMS$_ORG            99044   /* Invalid file organization */
#define RMS$_PLG            99060   /* File prologue error */
#define RMS$_RAB            99068   /* Not a valid RAB */
#define RMS$_RAT            99076   /* Invalid record attributes */
#define RMS$_RFM            99084   /* Invalid record format */
#define RMS$_RSS            99100   /* Invalid resultant string size */
#define RMS$_RTB            99108   /* Record too big for buffer */
#define RMS$_SEQ            99116   /* Record not sequential */
#define RMS$_SIZ            99124   /* Invalid size value */
#define RMS$_SYN            99132   /* Syntax error in filespec */
#define RMS$_TNS            99140   /* Terminator not seen (partial record) */
#define RMS$_TRE            99148   /* Index tree error */
#define RMS$_TYP            99156   /* Invalid file type */
#define RMS$_WCC            99172   /* Invalid wildcard context */
#define RMS$_DME            99180   /* Dynamic memory exhausted */

/* ================================================================
 * RMS codes returned on wildcard operations
 * ================================================================ */

#define RMS$_NMF            99188   /* No more files (wildcard exhausted) */

/* ================================================================
 * RMS informational/warning codes
 * ================================================================ */

#define RMS$_CRE_STM        98833   /* File created as stream */
#define RMS$_CREATED        98841   /* File was created */
#define RMS$_FILEPURGED     98849   /* Previous version purged */
#define RMS$_SUPERSEDE      98857   /* File superseded */
#define RMS$_OK_RRV         98865   /* Success, record in RRV */
#define RMS$_COD            98896   /* Invalid code value */
#define RMS$_CUR            98908   /* No current record */
#define RMS$_DAC            98916   /* File deaccess error */
#define RMS$_DAN            98924   /* Data area number error */
#define RMS$_IAN            98972   /* Index area number error */
#define RMS$_RAC            98984   /* Invalid record access */
#define RMS$_RPL            99052   /* Error reading prologue */
#define RMS$_WPL            99060   /* Error writing prologue */
#define RMS$_NAM            99068   /* Bad NAM block */

/* ================================================================
 * RMS status testing macros
 * ================================================================ */

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
