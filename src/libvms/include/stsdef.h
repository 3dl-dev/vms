/*
 * STSDEF.H - VMS Status Value Structure Definitions
 *
 * OpenVMX compatibility layer - Defines the layout and manipulation
 * macros for VMS condition values (status codes).
 *
 * A VMS status value is a 32-bit longword with the following layout:
 *
 *   31  29 28  27  16 15    3  2  0
 *   +----+---+------+--+------+---+
 *   |Rsv |Inh|Cust| Facility |Sp| Msg No | Sev |
 *   +----+---+------+--+------+---+
 *
 * Bits  0-2:  Severity (STS$V_SEVERITY)
 * Bits  3-15: Message number (STS$V_MSG_NO)
 * Bit   15:   Facility-specific flag (STS$V_FAC_SP)
 * Bits 16-27: Facility number (STS$V_FAC_NO)
 * Bit   27:   Customer-defined flag (STS$V_CUST_DEF)
 * Bit   28:   Inhibit message flag (STS$V_INHIB_MSG)
 * Bits 29-31: Reserved
 *
 * Bit 0 is the success/failure indicator:
 *   odd  (bit 0 = 1) = success or informational
 *   even (bit 0 = 0) = warning, error, or fatal
 *
 * Reference: OpenVMS Programming Concepts Manual, Chapter 8
 *            OpenVMS System Services Reference Manual
 */

#ifndef __STSDEF_H
#define __STSDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Severity level constants
 *
 * The low 3 bits of a status value encode the severity.
 * Odd values (1, 3) indicate success; even values (0, 2, 4)
 * indicate failure.  This is the basis for the success-test
 * macro ($VMS_STATUS_SUCCESS).
 */
#define STS$K_WARNING  0   /* Warning */
#define STS$K_SUCCESS  1   /* Success */
#define STS$K_ERROR    2   /* Error */
#define STS$K_INFO     3   /* Informational */
#define STS$K_SEVERE   4   /* Severe (fatal) error */

/*
 * Bit position constants (shift counts)
 */
#define STS$V_SEVERITY   0   /* Severity field starts at bit 0 */
#define STS$V_MSG_NO     3   /* Message number starts at bit 3 */
#define STS$V_FAC_SP     15  /* Facility-specific flag at bit 15 */
#define STS$V_FAC_NO     16  /* Facility number starts at bit 16 */
#define STS$V_CUST_DEF   27  /* Customer-defined flag at bit 27 */
#define STS$V_INHIB_MSG  28  /* Inhibit message flag at bit 28 */

/*
 * Field mask constants
 *
 * Each mask covers the full field when ANDed with the status value.
 */
#define STS$M_SEVERITY   0x00000007  /* Bits 0-2: severity */
#define STS$M_MSG_NO     0x0000FFF8  /* Bits 3-15: message number */
#define STS$M_FAC_SP     0x00008000  /* Bit 15: facility-specific */
#define STS$M_FAC_NO     0x0FFF0000  /* Bits 16-27: facility number */
#define STS$M_CUST_DEF   0x08000000  /* Bit 27: customer-defined */
#define STS$M_INHIB_MSG  0x10000000  /* Bit 28: inhibit message */
#define STS$M_COND_ID    0x0FFFFFF8  /* Bits 3-27: condition identification */
#define STS$M_SUCCESS    0x00000001  /* Bit 0: success indicator */
#define STS$M_CONTROL    0xF0000000  /* Bits 28-31: control bits */

/*
 * Width constants (number of bits in each field)
 */
#define STS$S_SEVERITY   3   /* Severity is 3 bits wide */
#define STS$S_MSG_NO     13  /* Message number is 13 bits wide */
#define STS$S_FAC_NO     12  /* Facility number is 12 bits wide */

/*
 * Well-known facility numbers
 */
#define FACILITY_SYSTEM   0   /* SYSTEM facility */
#define FACILITY_RMS      1   /* RMS facility */
#define FACILITY_CLI      3   /* CLI facility */
#define FACILITY_LIB      21  /* LIB$ facility */
#define FACILITY_STR      36  /* STR$ facility */
#define FACILITY_MTH      22  /* MTH$ facility */

/*
 * Status testing and extraction macros
 */

/**
 * $VMS_STATUS_SUCCESS - Test if a status value indicates success
 *
 * Returns non-zero (true) if the status value represents success.
 * In VMS, a status is successful if bit 0 is set (odd severity).
 * Severity values 1 (success) and 3 (informational) are considered
 * successful.
 */
#define $VMS_STATUS_SUCCESS(sts) ((sts) & STS$M_SUCCESS)

/**
 * $VMS_STATUS_SEVERITY - Extract severity from status value
 */
#define $VMS_STATUS_SEVERITY(sts) ((sts) & STS$M_SEVERITY)

/**
 * $VMS_STATUS_CODE - Extract the message number (as used in ssdef.h)
 *
 * Note: In the traditional VMS definition, $VMS_STATUS_CODE constructs
 * a status code from components.  Here we also provide it as an extractor
 * for the message number field, consistent with common usage in ported code.
 */
#define $VMS_STATUS_MSG_NO(sts) (((sts) & STS$M_MSG_NO) >> STS$V_MSG_NO)

/**
 * $VMS_STATUS_FAC_NO - Extract the facility number
 */
#define $VMS_STATUS_FAC_NO(sts) (((sts) & STS$M_FAC_NO) >> STS$V_FAC_NO)

/**
 * $VMS_STATUS_FAC_SP - Test the facility-specific flag
 */
#define $VMS_STATUS_FAC_SP(sts) (((sts) & STS$M_FAC_SP) >> STS$V_FAC_SP)

/**
 * $VMS_STATUS_CUST_DEF - Test the customer-defined flag
 */
#define $VMS_STATUS_CUST_DEF(sts) (((sts) & STS$M_CUST_DEF) >> STS$V_CUST_DEF)

/**
 * $VMS_STATUS_INHIB_MSG - Test the inhibit-message flag
 */
#define $VMS_STATUS_INHIB_MSG(sts) (((sts) & STS$M_INHIB_MSG) >> STS$V_INHIB_MSG)

/**
 * $VMS_STATUS_CODE - Construct a status code from components
 *
 * @param fac     Facility number (0-4095)
 * @param facsp   Facility-specific flag (0 or 1)
 * @param msgno   Message number (0-8191)
 * @param sev     Severity (0-7)
 */
#define $VMS_STATUS_CODE(fac, facsp, msgno, sev) \
    ((((uint32_t)(fac) << STS$V_FAC_NO) & STS$M_FAC_NO) | \
     (((uint32_t)(facsp) << STS$V_FAC_SP) & STS$M_FAC_SP) | \
     (((uint32_t)(msgno) << STS$V_MSG_NO) & STS$M_MSG_NO) | \
     ((uint32_t)(sev) & STS$M_SEVERITY))

/**
 * STS$MATCH - Compare two status values ignoring severity
 *
 * Returns non-zero if the condition identifications match,
 * regardless of severity.  This is used to test which specific
 * condition occurred without caring about the exact severity.
 */
#define STS$MATCH(sts1, sts2) \
    (((sts1) & STS$M_COND_ID) == ((sts2) & STS$M_COND_ID))

/**
 * STS$VALUE - Construct a status value from all components
 *
 * @param sev      Severity (0-7)
 * @param fac      Facility number (0-4095)
 * @param msg      Message number (0-8191)
 * @param fac_sp   Facility-specific flag (0 or 1)
 * @param cust     Customer-defined flag (0 or 1)
 */
#define STS$VALUE(sev, fac, msg, fac_sp, cust) \
    ( ((uint32_t)(sev)    & 0x7)   << STS$V_SEVERITY  | \
      ((uint32_t)(msg)    & 0x1FFF)<< STS$V_MSG_NO    | \
      ((uint32_t)(fac_sp) & 0x1)   << STS$V_FAC_SP    | \
      ((uint32_t)(fac)    & 0xFFF) << STS$V_FAC_NO    | \
      ((uint32_t)(cust)   & 0x1)   << STS$V_CUST_DEF )

/*
 * Convenience macros matching common VMS usage patterns
 */
#define VMSCOND_SUCCESS(sts)  $VMS_STATUS_SUCCESS(sts)
#define VMSCOND_SEVERITY(sts) $VMS_STATUS_SEVERITY(sts)
#define VMSCOND_WARNING(sts)  ($VMS_STATUS_SEVERITY(sts) == STS$K_WARNING)
#define VMSCOND_ERROR(sts)    ($VMS_STATUS_SEVERITY(sts) == STS$K_ERROR)
#define VMSCOND_SEVERE(sts)   ($VMS_STATUS_SEVERITY(sts) == STS$K_SEVERE)
#define VMSCOND_INFO(sts)     ($VMS_STATUS_SEVERITY(sts) == STS$K_INFO)

#ifdef __cplusplus
}
#endif

#endif /* __STSDEF_H */
