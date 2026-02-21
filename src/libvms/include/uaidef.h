/*
 * UAIDEF.H - VMS User Authorization File (UAF) Item Code Definitions
 *
 * OpenVMX compatibility layer - Defines the UAI$_ item codes and
 * UAI$M_ flag mask constants used with sys$getuai (Get User
 * Authorization Information) and sys$setuai (Set User Authorization
 * Information).
 *
 * Item codes are passed in an item list (ILE3 or itm3 structs) to
 * request or modify specific UAF record fields.
 *
 * Data type key (in comments below):
 *   L  = Longword (32-bit unsigned integer)
 *   W  = Word (16-bit unsigned integer)
 *   B  = Byte (8-bit value)
 *   T  = Counted ASCII string (first byte is length, then data)
 *   Q  = Quadword (64-bit value, e.g., privilege masks, passwords)
 *
 * Reference: OpenVMS System Services Reference Manual (SYS$GETUAI,
 *            SYS$SETUAI)
 *            OpenVMS Guide to System Security
 *            OpenVMS Programming Concepts Manual, Chapter 10
 */

#ifndef __UAIDEF_H
#define __UAIDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * UAI$_ item codes for SYS$GETUAI / SYS$SETUAI
 * ================================================================ */

/* Identity and account */
#define UAI$_USERNAME       0x0001  /* Username (T, 12 chars) */
#define UAI$_UIC            0x0002  /* User Identification Code (L) */
#define UAI$_ACCOUNT        0x0003  /* Account name (T, 8 chars) */
#define UAI$_OWNER          0x0004  /* Owner name (T, 31 chars) */
#define UAI$_COMMENT        0x0005  /* Comment field (T, 63 chars) */

/* Login paths */
#define UAI$_DEFDEV         0x0010  /* Default device (T, counted, 31 chars) */
#define UAI$_DEFDIR         0x0011  /* Default directory (T, counted, 63 chars) */
#define UAI$_LGICMD         0x0012  /* Login command procedure (T, 63 chars) */
#define UAI$_CLITABLES      0x0013  /* CLI tables (T, 31 chars) */
#define UAI$_CLI            0x0014  /* Command line interpreter name (T, 31 chars) */
#define UAI$_LANGUAGE       0x0015  /* NLS language name (T, 31 chars) */

/* Flags */
#define UAI$_FLAGS          0x0020  /* Account flags (L) — see UAI$M_* below */

/* Password and authentication */
#define UAI$_PWD            0x0030  /* Primary password hash (Q, 8 bytes) */
#define UAI$_PWD2           0x0031  /* Secondary password hash (Q, 8 bytes) */
#define UAI$_SALT           0x0032  /* Password salt value (W, 2 bytes) */
#define UAI$_ENCRYPT        0x0033  /* Encryption algorithm (B, 1 byte) */
#define UAI$_ENCRYPT2       0x0034  /* Secondary encryption algorithm (B) */
#define UAI$_PWD_LENGTH     0x0035  /* Minimum password length (B) */
#define UAI$_PWD_LIFETIME   0x0036  /* Password lifetime (Q, VMS time delta) */
#define UAI$_PWD_DATE       0x0037  /* Date password was last changed (Q) */
#define UAI$_PWD2_DATE      0x0038  /* Date secondary password changed (Q) */

/* Privileges */
#define UAI$_PRIV           0x0040  /* Authorized privileges (Q, 8 bytes) */
#define UAI$_DEF_PRIV       0x0041  /* Default (enabled) privileges (Q, 8 bytes) */

/* Quotas and limits */
#define UAI$_ASTLM          0x0050  /* AST queue limit (L) */
#define UAI$_BIOLM          0x0051  /* Buffered I/O limit (L) */
#define UAI$_BYTLM          0x0052  /* Buffered I/O byte count limit (L) */
#define UAI$_CPUTIM         0x0053  /* CPU time limit (0=unlimited) (L) */
#define UAI$_DIOLM          0x0054  /* Direct I/O limit (L) */
#define UAI$_ENQLM          0x0055  /* Lock manager enqueue limit (L) */
#define UAI$_FILLM          0x0056  /* Open file limit (L) */
#define UAI$_JTQUOTA        0x0057  /* Job table quota (L) */
#define UAI$_MAXACCTJOBS    0x0058  /* Max jobs per account (L) */
#define UAI$_MAXDETACH      0x0059  /* Max detached processes (L) */
#define UAI$_MAXJOBS        0x005A  /* Max total jobs (L) */
#define UAI$_PGFLQUOTA      0x005B  /* Page file quota (L) */
#define UAI$_PRCCNT         0x005C  /* Subprocess count limit (L) */
#define UAI$_TQCNT          0x005D  /* Timer queue entry limit (L) */

/* Working set */
#define UAI$_DFWSCNT        0x0060  /* Default working set size (L) */
#define UAI$_WSEXTENT       0x0061  /* Working set extent (L) */
#define UAI$_WSQUOTA        0x0062  /* Working set quota (L) */

/* Process defaults */
#define UAI$_PRI            0x0070  /* Default base priority (B, 1 byte) */
#define UAI$_QUEPRI         0x0071  /* Maximum batch queue priority (B) */

/* Time restrictions */
#define UAI$_EXPIRATION     0x0080  /* Account expiration date/time (Q) */
#define UAI$_PRIMEDAYS      0x0081  /* Prime shift days (W, bitmask) */

/* Network access */
#define UAI$_NETWORK_ACCESS_P 0x0090 /* Network primary day access hours (Q) */
#define UAI$_NETWORK_ACCESS_S 0x0091 /* Network secondary day access hours (Q) */
#define UAI$_DIALUP_ACCESS_P  0x0092 /* Dialup primary day access hours (Q) */
#define UAI$_DIALUP_ACCESS_S  0x0093 /* Dialup secondary day access hours (Q) */
#define UAI$_LOCAL_ACCESS_P   0x0094 /* Local primary day access hours (Q) */
#define UAI$_LOCAL_ACCESS_S   0x0095 /* Local secondary day access hours (Q) */
#define UAI$_BATCH_ACCESS_P   0x0096 /* Batch primary day access hours (Q) */
#define UAI$_BATCH_ACCESS_S   0x0097 /* Batch secondary day access hours (Q) */
#define UAI$_REMOTE_ACCESS_P  0x0098 /* Remote primary day access hours (Q) */
#define UAI$_REMOTE_ACCESS_S  0x0099 /* Remote secondary day access hours (Q) */

/* Security and audit */
#define UAI$_AUDIT_FLAGS    0x00A0  /* Audit flags (Q) */

/* Login history */
#define UAI$_LSTLOGIN_I     0x00B0  /* Last interactive login date/time (Q) */
#define UAI$_LSTLOGIN_N     0x00B1  /* Last non-interactive login date/time (Q) */

/* ================================================================
 * UAI$M_ flag bit masks for UAI$_FLAGS item
 *
 * These bits control account access and behavior.  They are returned
 * and set as a longword via the UAI$_FLAGS item code.
 * ================================================================ */

#define UAI$M_DISCTLY       0x00000001  /* Bit 0:  Disregard captive flag */
#define UAI$M_DEFCLI        0x00000002  /* Bit 1:  Default CLI is forced */
#define UAI$M_LOCKPWD       0x00000004  /* Bit 2:  Password is locked (no change) */
#define UAI$M_NODISMAIL     0x00000008  /* Bit 3:  Do not disable mail */
#define UAI$M_DISMAIL       0x00000010  /* Bit 4:  Disable new mail notification */
#define UAI$M_NOMAIL        0x00000020  /* Bit 5:  No mail (deprecated; use DISMAIL) */
#define UAI$M_DISNEWMAIL    0x00000040  /* Bit 6:  Disable new mail notification */
#define UAI$M_CAPTIVE       0x00000080  /* Bit 7:  Captive account */
#define UAI$M_DISREPORT     0x00000100  /* Bit 8:  Disable last login reporting */
#define UAI$M_DISRECONNECT  0x00000200  /* Bit 9:  Disable reconnect on disconnect */
#define UAI$M_AUTOLOGIN     0x00000400  /* Bit 10: Automatic login allowed */
#define UAI$M_DISLOCAL      0x00000800  /* Bit 11: Local interactive login disabled */
#define UAI$M_DISDIALUP     0x00001000  /* Bit 12: Dialup login disabled */
#define UAI$M_DISNETWORK    0x00002000  /* Bit 13: Network login disabled */
#define UAI$M_DISACNT       0x00004000  /* Bit 14: Account disabled */
#define UAI$M_DISBATCH      0x00008000  /* Bit 15: Batch login disabled */
#define UAI$M_DISUSER       0x00010000  /* Bit 16: Interactive login disabled (alias) */
#define UAI$M_DISWELCOME    0x00020000  /* Bit 17: Suppress welcome message */
#define UAI$M_EXTAUTH       0x00040000  /* Bit 18: External authentication enabled */
#define UAI$M_MIGRATEPWD    0x00080000  /* Bit 19: Password migration in progress */
#define UAI$M_VMSAUTH       0x00100000  /* Bit 20: VMS authentication also required */
#define UAI$M_PWDMIX        0x00200000  /* Bit 21: Mixed-case passwords allowed */
#define UAI$M_GENERATE_PWD  0x00400000  /* Bit 22: Auto-generate password on login */
#define UAI$M_DISPWDDIC     0x00800000  /* Bit 23: Bypass password dictionary check */
#define UAI$M_DISPWDHIS     0x01000000  /* Bit 24: Bypass password history check */
#define UAI$M_DISPWDSYNCH   0x02000000  /* Bit 25: Disable password synchronization */
#define UAI$M_RESTRICTED    0x04000000  /* Bit 26: Restricted account (limited DCL) */
#define UAI$M_DISFORCE_PWD  0x08000000  /* Bit 27: Do not force password change */
#define UAI$M_PWD_EXPIRED   0x10000000  /* Bit 28: Password has expired */
#define UAI$M_PWD2_EXPIRED  0x20000000  /* Bit 29: Secondary password has expired */
#define UAI$M_AUDIT         0x40000000  /* Bit 30: Audit all logins */
#define UAI$M_DISIMAGE      0x80000000  /* Bit 31: Disable image activation */

/* ================================================================
 * UAI$_ password encryption algorithm codes (UAI$_ENCRYPT field)
 * ================================================================ */

#define UAI$C_AD_II         1   /* AUTODIN II (default VMS hash) */
#define UAI$C_PURDY         2   /* Purdy polynomial */
#define UAI$C_PURDY_V       3   /* Purdy with visible characters */
#define UAI$C_PURDY_S       4   /* Purdy, susceptible (older) */
#define UAI$C_CUST          5   /* Customer-defined algorithm */

#ifdef __cplusplus
}
#endif

#endif /* __UAIDEF_H */
