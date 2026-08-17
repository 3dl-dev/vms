/*
 * sysuaf.h - SYSUAF (System User Authorization File) library
 *
 * Shared interface for looking up users and authenticating passwords
 * against SYS$SYSTEM:SYSUAF.DAT. Used by vms_login, vms_ssh_auth,
 * and vmssshd.
 *
 * The library is usable without any VMS runtime being initialized —
 * it is pure file parsing and SHA256 hashing.
 */

#ifndef SYSUAF_H
#define SYSUAF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ovmx_layout.h"
#include "rms/xab.h"

#define SYSUAF_PATH VMS_SYSUAF_PATH

/* =========================================================================
 * THE SYSUAF.DAT TEXT FORMAT — ONE DEFINITION, ONE READER, ONE WRITER
 * (vms-9b7)
 *
 * WHAT THIS REPLACES, so it is never put back: five independent hand-rolled
 * parsers of one file format, with THREE different line-buffer sizes and TWO
 * different writer format strings.
 *
 *   src/ovmx_init/ovmx_init.c  sysuaf_split()   char line[512]   DELETED
 *   src/ovmx_init/ovmx_init.c  sysuaf_field()   char line[512]   DELETED
 *   src/libvms/rtl/sysuaf.c    sysuaf_scan()    char line[1024]  -> this
 *   src/libvms/syssvc/sys_uai.c parse_uaf_line  512              -> this
 *   tools/vms_authorize.c      load_sysuaf()    1024             -> this
 *
 * MEASURED, on a real QEMU boot of the unfixed tree (three boots, each with
 * a control): a SYSTEM row long enough that its SIXTH '|' falls past byte
 * 511 is read by the 512-byte readers as a record with only five fields.
 * PID 1's establish_system_identity() then reported
 *
 *     %OVMX-F-EXECINIT, no SYSTEM record in SYS$SYSTEM:SYSUAF.DAT
 *
 * and powered the machine off, while the 1024-byte readers accepted the same
 * row without complaint. That is a writer and a reader disagreeing about one
 * file, and the disagreement is fatal by design (Rule 10) -- so the format
 * gets exactly one definition and every accessor is derived from it.
 *
 * SYSUAF_LINE_MAX is the ONE limit. It bounds the writer (an over-length
 * record is REFUSED, loudly, by sysuaf_format_record() -- never silently
 * clipped) and it bounds the reader (an over-length line is REPORTED by
 * sysuaf_read_line() -- never silently truncated into a short record).
 * ========================================================================= */

/* The record separator. Chosen over ':' because VMS device names contain
   colons and DEFAULT_DIR is a filespec. */
#define SYSUAF_SEP_CHAR      '|'
#define SYSUAF_SEP_STR       "|"

/* Field order and count. Rows carrying fewer than SYSUAF_MIN_FIELDS are
   malformed; FLAGS, PRIVILEGES and LGICMD may be absent on a legacy row.
   LGICMD (field 8, vms-e48) is a trailing OPTIONAL field: a row that omits it
   parses exactly as before (empty LGICMD), and the writer omits it when empty,
   so pre-vms-e48 SYSUAF.DAT rows round-trip byte-identically. */
#define SYSUAF_FIELD_COUNT   8
#define SYSUAF_MIN_FIELDS    5

enum {
    SYSUAF_F_USERNAME   = 0,
    SYSUAF_F_PWHASH     = 1,
    SYSUAF_F_UIC_GROUP  = 2,
    SYSUAF_F_UIC_MEMBER = 3,
    SYSUAF_F_DEFDIR     = 4,
    SYSUAF_F_FLAGS      = 5,
    SYSUAF_F_PRIVILEGES = 6,
    SYSUAF_F_LGICMD     = 7
};

/*
 * The login command file run at login when the account's LGICMD field is
 * empty (vms-e48). VSI OpenVMS System Manager's Manual, AUTHORIZE /LGICMD:
 * "If you omit this qualifier, the OpenVMS operating system uses the default,
 * SYS$LOGIN:LOGIN.COM." SYS$LOGIN is the per-user login logical this same
 * item establishes, so the default resolves through it to the user's own
 * home -- one definition, used by LOGINOUT (tools/vms_login.c) and by DCL's
 * login-mode fallback (src/vmsdcl/dcl_main.c).
 */
#define SYSUAF_DEFAULT_LGICMD  "SYS$LOGIN:LOGIN.COM"

/*
 * THE UIC FIELDS ARE OCTAL (vms-e60). Derivation -- from the oracle, not
 * chosen -- is in the block comment on sysuaf_parse_line() in
 * src/libvms/rtl/sysuaf.c. Every read, write, display and /UIC=[g,m] parse
 * in the tree goes through this constant so they cannot drift apart again.
 */
#define SYSUAF_UIC_RADIX     8

/* The one line limit, shared by the writer's refusal and the reader's
   truncation report. Includes the terminating newline. */
#define SYSUAF_LINE_MAX      1024

/* -------------------------------------------------------------------------
 * In-memory (parsed) SYSUAF record.
 *
 * Legacy representation produced by sysuaf_lookup() from the current
 * pipe-delimited text SYSUAF.DAT. Fields are text/string-oriented. This is
 * the interface consumed today by vms_login, vms_ssh_auth, and vmssshd.
 * ------------------------------------------------------------------------- */
typedef struct {
    char     username[64];
    char     password_hash[128];
    uint32_t uic_group;
    uint32_t uic_member;
    char     default_dir[256];
    char     flags[64];
    char     privileges[256];
    char     lgicmd[256];       /* login command file (SYSUAF LGICMD); empty
                                   means the SYSUAF_DEFAULT_LGICMD default */
} sysuaf_record_t;

/*
 * Parse one SYSUAF line into a record. 'line' is MODIFIED IN PLACE.
 * Returns  1 if a record was parsed,
 *          0 if the line is a comment or blank (not an error),
 *         -1 if the line is malformed (fewer than SYSUAF_MIN_FIELDS fields).
 */
int sysuaf_parse_line(char *line, sysuaf_record_t *rec);

/*
 * Render one record as the SYSUAF line that represents it, WITHOUT the
 * trailing newline.
 *
 * Returns the length written on success, or -1 if the record does not fit in
 * SYSUAF_LINE_MAX (counting the newline a writer will append). AN OVER-LENGTH
 * RECORD IS REFUSED, NEVER CLIPPED: a reader that silently truncates is how
 * the SYSTEM record went missing and the boot halted, and clipping here would
 * simply move that same silent data loss one process earlier.
 */
int sysuaf_format_record(const sysuaf_record_t *rec, char *out, size_t outsz);

/*
 * Read one line, reporting truncation instead of hiding it.
 *
 * Returns 1 when a line was read, 0 at end of file. When the line did not fit
 * in 'bufsz', *too_long is set to 1, the REST OF THE LINE IS CONSUMED (so the
 * caller's next read starts at a real record boundary and never at a
 * fragment), and the caller decides what to do -- which is never "carry on
 * with the prefix".
 */
int sysuaf_read_line(FILE *fp, char *buf, size_t bufsz, int *too_long);

/* FLAGS is stored in the file as a comma-separated list of UAI flag NAMES
   (what a system manager types at AUTHORIZE's /FLAGS=), and exposed through
   $GETUAI/$SETUAI as the UAI$M_* longword. These two functions are the only
   conversion between the two, so the file format has one answer and the API
   has one answer and neither invents the other's. An unrecognized name is
   ignored; an empty string is mask 0 and round-trips back to empty. */
uint32_t sysuaf_flags_to_mask(const char *flags);
void     sysuaf_mask_to_flags(uint32_t mask, char *out, size_t outsz);


/* =========================================================================
 * THE REAL BINARY $UAFDEF SYSUAF RECORD — vms-f88 / epic vms-d0c
 *
 * Real OpenVMS stores users in SYS$SYSTEM:SYSUAF.DAT as an RMS PROLOG-3
 * INDEXED file of fixed-length BINARY records ($UAFDEF) — NOT a flat text
 * file, and NOT the ASCII+SHA-256 stand-in this record replaces.
 * sysuaf_rms_record_t is that on-disk record; the indexed file that carries
 * it is authored/read through the genuine Files-11 Prolog-3 engine
 * (src/vmsrms/rms_prolog3.{c,h}) over the executive ACP window (or the rms_io
 * POSIX backend when /dev/vms is absent), with primary key = 32-byte USERNAME
 * (key of reference 0) and secondary key = the UIC longword (key 1). The small
 * create/put/get-by-username/get-by-uic API lives in src/vmsrms/sysuaf_rms.h.
 *
 * ORACLE GROUNDING (docs/oracle/vax73-alpha84-uafdef.md, vms-db8): the record
 * is 644 bytes on BOTH VAX V7.3 and Alpha V8.4 (no architecture divergence),
 * and the fields marked [PIN] below sit at the byte offset the oracle located
 * by controlled edit (DUMP/RECORDS diff — clean-room, CLAUDE.md Rule 8):
 *
 *   [PIN] UAF$T_USERNAME   @0x004  32   primary key, blank-padded, upcased
 *   [PIN] UAF$L_UIC        @0x024   4   (group<<16)|member, LE longword
 *   [PIN] owner identifier @0x02C   8   (key 3)
 *   [PIN] UAF$Q_PWD        @0x154   8   hashed-password quadword
 *   [PIN] UAF$W_SALT       @0x166   2   per-account salt word
 *   [PIN] UAF$B_ENCRYPT    @0x168   1   algorithm (0x03 = UAI$C_PURDY_S)
 *   [PIN] UAF$B_PWD_LENGTH @0x16A   1   minimum password length
 *   [PIN] UAF$Q_PWD2       @0x16C   8   secondary password quadword
 *
 * Where the oracle does NOT publish a field's byte offset (the account string
 * area, the password-change date, the flags/priv/quota region), OVMX defines
 * its own offset and LABELS it [OVMX] per Rule 8 — those are NOT presented as
 * VMS-authentic sub-offsets. The [PIN] offsets above are exact and asserted.
 *
 * SUBSTRATE-AGNOSTIC ON-DISK ENCODING (vms-5f0 thin-seam): every multi-byte
 * on-disk field is a fixed-width BYTE ARRAY read/written through the
 * p3_le16/le32/le64 accessors — never a native long/size_t/pointer. The whole
 * struct is uint8_t/char members (alignment 1) so it has NO implicit padding
 * and is byte-identical on x86_64/aarch64 LP64 and VAX ILP32. There is no
 * substrate #ifdef. Field-access helpers (UIC, password quadword/salt/encrypt)
 * live in src/vmsrms/sysuaf_rms.h.
 *
 * ⚠ THE PASSWORD IS NOT COMPUTED HERE. This record STORES the password
 * quadword + salt + algorithm byte AS BYTES (the record FIELD only). Computing
 * the Purdy hash from (password, username, salt) is the NEXT rung (vms-631e);
 * the seam is marked on uaf$q_pwd below. Nothing here hashes a password, and
 * nothing stores a SHA-256 (the facade this record replaces).
 * ========================================================================= */

#define SYSUAF_USERNAME_LEN    32   /* UAF$T_USERNAME width, primary key [PIN] */
#define SYSUAF_UAF_RECORD_SIZE 644  /* $UAFDEF record length [PIN, both arches]*/

/* [PIN] byte offsets the oracle located by controlled DUMP/RECORDS edit. */
#define UAF$K_USERNAME_OFF   0x004
#define UAF$K_UIC_OFF        0x024
#define UAF$K_OWNER_OFF      0x02C
#define UAF$K_PWD_OFF        0x154
#define UAF$K_SALT_OFF       0x166
#define UAF$K_ENCRYPT_OFF    0x168
#define UAF$K_PWD_LENGTH_OFF 0x16A
#define UAF$K_PWD2_OFF       0x16C

/* UAF$B_ENCRYPT algorithm byte values (public $UAIDEF). */
#define UAI$C_AD_II    0
#define UAI$C_PURDY    1
#define UAI$C_PURDY_V  2
#define UAI$C_PURDY_S  3   /* [PIN] the modern default the oracle observed */

typedef struct {
    /* -- record head -- */
    uint8_t  uaf$b_rtype;         /* @0x000 [PIN] record type == 1            */
    uint8_t  uaf$b_version;       /* @0x001 [PIN] record version == 1         */
    uint8_t  uaf$w_reserved0[2];  /* @0x002 [PIN] reserved word               */
    char     uaf$t_username[32];  /* @0x004 [PIN] primary key, blank-padded   */
    uint8_t  uaf$l_uic[4];        /* @0x024 [PIN] (grp<<16)|mem, LE longword   */
    uint8_t  uaf$l_uic_ext_hi[4]; /* @0x028 [OVMX] high half of the 8-byte     */
                                  /*        extended user identifier (key 2)   */
    uint8_t  uaf$q_owner_id[8];   /* @0x02C [PIN] owner identifier (key 3)     */
    /* -- account string area ([OVMX] sub-offsets; oracle: not load-bearing) --*/
    char     uaf$t_account[32];   /* @0x034 [OVMX] accounting string           */
    char     uaf$t_owner_name[32];/* @0x054 [OVMX] account owner name          */
    char     uaf$t_defdev[32];    /* @0x074 [OVMX] default device              */
    char     uaf$t_defdir[64];    /* @0x094 [OVMX] default directory           */
    char     uaf$t_lgicmd[64];    /* @0x0D4 [OVMX] login command file          */
    char     uaf$t_defcli[32];    /* @0x114 [OVMX] CLI (e.g. "DCL")            */
    char     uaf$t_clitables[32]; /* @0x134 [OVMX] command tables             */
    /* -- password area (offsets [PIN]) -- */
    /* Purdy hash filled by vms-631e; this rung stores the quadword AS BYTES.  */
    uint8_t  uaf$q_pwd[8];        /* @0x154 [PIN] password hash quadword       */
    uint8_t  uaf$q_pwd_date[8];   /* @0x15C [OVMX] password change date        */
    uint8_t  uaf$w_reserved1[2];  /* @0x164 [OVMX] pad to the salt word        */
    uint8_t  uaf$w_salt[2];       /* @0x166 [PIN] per-account salt word        */
    uint8_t  uaf$b_encrypt;       /* @0x168 [PIN] algorithm (UAI$C_PURDY_S=3)  */
    uint8_t  uaf$b_reserved2;     /* @0x169 [OVMX] pad                         */
    uint8_t  uaf$b_pwd_length;    /* @0x16A [PIN] minimum password length      */
    uint8_t  uaf$b_reserved3;     /* @0x16B [OVMX] pad                         */
    uint8_t  uaf$q_pwd2[8];       /* @0x16C [PIN] secondary password quadword  */
    /* -- flags / privileges ([OVMX] offsets) -- */
    uint8_t  uaf$l_flags[4];      /* @0x174 [OVMX] UAF$M_* flags longword      */
    uint8_t  uaf$q_priv[8];       /* @0x178 [OVMX] authorized privilege mask   */
    uint8_t  uaf$q_def_priv[8];   /* @0x180 [OVMX] default privilege mask      */
    uint8_t  uaf$r_reserved4[120];/* @0x188 [OVMX] reserved up to quota block  */
    /* -- quota block ([OVMX]; oracle §4 correlates values, not sub-offsets) --*/
    uint8_t  uaf$r_quota[132];    /* @0x200 [OVMX] quota region -> 0x284       */
} sysuaf_rms_record_t;            /* total: 644 bytes ($UAFDEF) */

/* Compile-time layout lock: 644-byte record with NO implicit padding, and
   every [PIN] field at the exact oracle offset. Adding/reordering a field is a
   design change (run the vms-d0c cascade) and must keep the [PIN] offsets. */
_Static_assert(sizeof(sysuaf_rms_record_t) == SYSUAF_UAF_RECORD_SIZE,
               "sysuaf_rms_record_t must be exactly 644 bytes ($UAFDEF)");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$t_username) == UAF$K_USERNAME_OFF,
               "UAF$T_USERNAME must be at 0x04 (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$l_uic) == UAF$K_UIC_OFF,
               "UAF$L_UIC must be at 0x24 (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$q_owner_id) == UAF$K_OWNER_OFF,
               "owner identifier must be at 0x2C (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$q_pwd) == UAF$K_PWD_OFF,
               "UAF$Q_PWD must be at 0x154 (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$w_salt) == UAF$K_SALT_OFF,
               "UAF$W_SALT must be at 0x166 (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$b_encrypt) == UAF$K_ENCRYPT_OFF,
               "UAF$B_ENCRYPT must be at 0x168 (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$b_pwd_length) == UAF$K_PWD_LENGTH_OFF,
               "UAF$B_PWD_LENGTH must be at 0x16A (oracle [PIN])");
_Static_assert(offsetof(sysuaf_rms_record_t, uaf$q_pwd2) == UAF$K_PWD2_OFF,
               "UAF$Q_PWD2 must be at 0x16C (oracle [PIN])");

/* Primary key geometry (key of reference 0): the 32-byte username at [PIN]
   offset 0x04. UIC secondary key (key of reference 1) is the LE longword at
   [PIN] offset 0x24. Both are consumed by src/vmsrms/sysuaf_rms.c when it
   authors the Prolog-3 indexed file (p3_create_params seg0_pos/seg0_siz). */
#define SYSUAF_KEY_USERNAME_POS  UAF$K_USERNAME_OFF
#define SYSUAF_KEY_USERNAME_SIZ  SYSUAF_USERNAME_LEN
#define SYSUAF_KEY_UIC_POS       UAF$K_UIC_OFF
#define SYSUAF_KEY_UIC_SIZ       4

/* Look up a user in sysuaf.dat. Returns 0 on success, -1 if not found. */
int sysuaf_lookup(const char *username, sysuaf_record_t *rec);

/* Look up the account holding UIC 'uic' ((group << 16) | member).
   Returns 0 on success, -1 if no account holds it (vms-2f8: the rights
   database derives UIC identifiers from SYSUAF rather than keeping a second
   copy of every account's UIC that is free to disagree with this one). */
int sysuaf_lookup_by_uic(uint32_t uic, sysuaf_record_t *rec);

/* Authenticate: returns 1 if password matches, 0 if not.
   An empty/unset hash NEVER authenticates -- returns 0 for every password,
   including the empty string (vms-08f; see the Rule 10 disposition in
   sysuaf.c). OpenVMS has no state where an absent password field is a
   green light; the one documented passwordless state (UAI$M_AUTOLOGIN)
   is an explicit per-account flag OVMX does not implement, so an unset
   hash must mean "cannot authenticate", not "no password required". */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password);

/*
 * LOGIN-FLAG ENFORCEMENT (vms-c8fa), separate from the password check above.
 * Authenticating proves the credential; these prove the account may log in
 * and how the session is constrained. Both read the parsed FLAGS field
 * through the one converter (sysuaf_flags_to_mask), never a second parse.
 *
 * sysuaf_interactive_login_permitted: 0 if a DISABLING flag (DISUSER or
 *   DISACNT) forbids login, else 1. A NULL record fails closed (returns 0).
 *   A correct password on a disabled account MUST still be refused.
 * sysuaf_account_captive: 1 if the account is CAPTIVE (confined to its login
 *   command procedure, no escape to the "$" DCL prompt), else 0.
 * See the doc comments in src/libvms/rtl/sysuaf.c for the public-doc
 * (OpenVMS Guide to System Security) grounding of each flag's behavior.
 */
int sysuaf_interactive_login_permitted(const sysuaf_record_t *rec);
int sysuaf_account_captive(const sysuaf_record_t *rec);

/* Parse VMS privilege string (e.g. "TMPMBX,NETMBX,OPER") into bitmask */
uint64_t sysuaf_parse_privileges(const char *priv_string);

/*
 * Resolve the login command file for an account (vms-e48): the SYSUAF LGICMD
 * field when it is set, otherwise SYSUAF_DEFAULT_LGICMD. This is the ONE place
 * the "field empty -> documented default" rule is applied, so LOGINOUT
 * (tools/vms_login.c) and any other caller cannot disagree about the default.
 * The result is a VMS filespec (e.g. "DKA100:[SMITH]LOGIN.COM" or
 * "SYS$LOGIN:LOGIN.COM"); the caller translates it through the logical-name /
 * device tables like any other filespec. 'out' is always NUL-terminated.
 */
void sysuaf_login_command_file(const sysuaf_record_t *rec,
                               char *out, size_t outsz);

/*
 * Rewrite ONE existing row of SYSUAF_PATH in place with the record given
 * (the row matched is the one whose username equals rec->username,
 * case-insensitive). Every OTHER row -- parsed or not -- is copied through
 * VERBATIM, mirroring sys$setuai's targeted rewrite (src/libvms/syssvc/
 * sys_uai.c): a caller updating one account has no business touching, or
 * being defeated by, a row it does not understand.
 *
 * ONE WRITER (vms-9b7): the replaced line comes from sysuaf_format_record(),
 * the SAME function AUTHORIZE (tools/vms_authorize.c) and $SETUAI call --
 * this is a second CALLER of the one writer, never a second format.
 *
 * Returns 0 on success. Returns -1, leaving the file on disk UNCHANGED, if:
 *   - SYSUAF_PATH cannot be opened for read or the temp file cannot be
 *     created,
 *   - an existing row is longer than SYSUAF_LINE_MAX and cannot be safely
 *     carried forward (copying its prefix would corrupt that account,
 *     dropping it would delete it -- the same case sysuaf_read_line()
 *     reports at read time everywhere else in this file),
 *   - rec does not fit in SYSUAF_LINE_MAX (sysuaf_format_record() refuses),
 *   - or rec->username matches no row in the file.
 */
int sysuaf_write_record(const sysuaf_record_t *rec);

#endif /* SYSUAF_H */
