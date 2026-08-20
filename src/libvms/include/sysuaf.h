/*
 * sysuaf.h - SYSUAF (System User Authorization File) library
 *
 * Shared interface for looking up users and authenticating passwords against
 * SYS$SYSTEM:SYSUAF.DAT. Used by vms_login, vmssshd, MAIL, DCL SET PASSWORD,
 * $GETUAI/$SETUAI and PROVISION.
 *
 * ATOMIC FLIP (vms-d92, epic vms-d0c). The runtime SYSUAF is now the GENUINE
 * binary $UAFDEF RMS Prolog-3 indexed file (sysuaf_rms_record_t below), read
 * and written through the binary engine (src/vmsrms/sysuaf_rms.c) over the
 * Files-11 ACP, and passwords are the real VMS Purdy one-way hash
 * (UAI$C_PURDY_S, src/libvms/rtl/purdy.c). The ASCII pipe-delimited +
 * SHA-256-hex facade this file used to define -- one text format with five
 * hand-rolled parsers -- is RETIRED. There is no ASCII SYSUAF and no SHA-256
 * on any path.
 *
 * `sysuaf_record_t` remains the in-memory presentation shape the consumers
 * read (username / UIC / default directory / flag + privilege NAMES), but it is
 * now a DERIVED VIEW over the binary record it carries in `raw`: sysuaf_lookup
 * fills the text fields FROM the $UAFDEF record, and authentication verifies the
 * typed password against `raw`'s Purdy quadword -- never against a text hash.
 */

#ifndef SYSUAF_H
#define SYSUAF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ovmx_layout.h"
#include "rms/xab.h"
#include "uaidef.h"   /* $UAIDEF: UAI$C_* algorithm codes + UAI$M_* flags */

#define SYSUAF_PATH VMS_SYSUAF_PATH

/*
 * THE UIC FIELDS ARE OCTAL (vms-e60). VMS writes UICs in octal; every /UIC=[g,m]
 * parse and UIC display in the tree reads this radix so they cannot drift.
 * (Still consumed by AUTHORIZE's /UIC parse and next-member allocation.)
 */
#define SYSUAF_UIC_RADIX     8

/*
 * The login command file run at login when the account's LGICMD field is
 * empty (vms-e48). VSI OpenVMS System Manager's Manual, AUTHORIZE /LGICMD:
 * "If you omit this qualifier, the OpenVMS operating system uses the default,
 * SYS$LOGIN:LOGIN.COM."
 */
#define SYSUAF_DEFAULT_LGICMD  "SYS$LOGIN:LOGIN.COM"

/* =========================================================================
 * THE REAL BINARY $UAFDEF SYSUAF RECORD — vms-f88 / epic vms-d0c
 *
 * Real OpenVMS stores users in SYS$SYSTEM:SYSUAF.DAT as an RMS PROLOG-3
 * INDEXED file of fixed-length BINARY records ($UAFDEF) — NOT a flat text
 * file. sysuaf_rms_record_t is that on-disk record; the indexed file that
 * carries it is authored/read through the genuine Files-11 Prolog-3 engine
 * (src/vmsrms/rms_prolog3.{c,h}) over the executive ACP window (or the rms_io
 * POSIX backend when /dev/vms is absent), with primary key = 32-byte USERNAME
 * (key of reference 0) and secondary key = the UIC longword (key 1). The small
 * create/put/get-by-username/get-by-uic API lives in src/vmsrms/sysuaf_rms.h;
 * the runtime read/enumerate/store entry points live in src/vmsrms/sysuaf_live.h.
 *
 * ORACLE GROUNDING (docs/oracle/vax73-alpha84-uafdef.md, vms-db8): the record
 * is 644 bytes on BOTH VAX V7.3 and Alpha V8.4 (no architecture divergence),
 * and the fields marked [PIN] below sit at the byte offset the oracle located
 * by controlled edit (DUMP/RECORDS diff — clean-room, CLAUDE.md Rule 8):
 *
 *   [PIN] UAF$T_USERNAME   @0x004  32   primary key, blank-padded, upcased
 *   [PIN] UAF$L_UIC        @0x024   4   (group<<16)|member, LE longword
 *   [PIN] owner identifier @0x02C   8   (key 3)
 *   [PIN] UAF$Q_PWD        @0x154   8   hashed-password quadword (Purdy)
 *   [PIN] UAF$W_SALT       @0x166   2   per-account salt word
 *   [PIN] UAF$B_ENCRYPT    @0x168   1   algorithm byte (UAI$C_PURDY_S)
 *   [PIN] UAF$B_PWD_LENGTH @0x16A   1   minimum password length
 *   [PIN] UAF$Q_PWD2       @0x16C   8   secondary password quadword
 *
 * Where the oracle does NOT publish a field's byte offset (the account string
 * area, the password-change date, the flags/priv/quota region), OVMX defines
 * its own offset and LABELS it [OVMX] per Rule 8. The [PIN] offsets are exact.
 *
 * SUBSTRATE-AGNOSTIC (vms-5f0 thin-seam): every multi-byte on-disk field is a
 * fixed-width BYTE ARRAY read/written through the p3_le16/le32/le64 accessors
 * — never a native long/size_t/pointer. The struct is uint8_t/char (alignment
 * 1) so it has NO implicit padding and is byte-identical on x86_64/aarch64 LP64
 * and VAX ILP32.
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

/*
 * UAF$B_ENCRYPT algorithm byte values come from $UAIDEF (uaidef.h, included
 * above) so there is ONE definition of UAI$C_PURDY_S across the writer
 * (sysuaf_rms.c / mksysuaf) and the reader (sysuaf.c authenticate) -- a split
 * definition would let the two disagree and silently break every login.
 *
 * SOURCE-OF-TRUTH: RESOLVED (vms-722). The oracle DUMP of a real on-disk
 * SYSUAF measured UAF$B_ENCRYPT == 0x03 on both VAX 7.3 and Alpha 8.4, and the
 * public $UAIDEF enumerates AD_II=0, PURDY=1, PURDY_V=2, PURDY_S=3 -- oracle
 * and $UAIDEF AGREE at 3. An earlier uaidef.h was off-by-one (AD_II 1, PURDY 2,
 * PURDY_V 3, PURDY_S 4), so OVMX had been stamping 4 on disk and rejecting the
 * authentic byte 3; uaidef.h is now corrected to the real $UAIDEF and is the
 * single source, so writer (sysuaf_rms.c / mksysuaf) and reader (sysuaf.c
 * authenticate) both agree on UAI$C_PURDY_S == 3.
 */

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
    uint8_t  uaf$q_pwd[8];        /* @0x154 [PIN] Purdy password hash quadword */
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

/* Compile-time layout lock: 644-byte record, NO implicit padding, every [PIN]
   field at the exact oracle offset. */
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

/* Primary key = 32-byte username @0x04; secondary key = UIC longword @0x24. */
#define SYSUAF_KEY_USERNAME_POS  UAF$K_USERNAME_OFF
#define SYSUAF_KEY_USERNAME_SIZ  SYSUAF_USERNAME_LEN
#define SYSUAF_KEY_UIC_POS       UAF$K_UIC_OFF
#define SYSUAF_KEY_UIC_SIZ       4

/* -------------------------------------------------------------------------
 * In-memory (parsed) SYSUAF record -- a DERIVED VIEW over the binary record.
 *
 * sysuaf_lookup() fills the text fields FROM the 644-byte $UAFDEF record it
 * reads, and keeps that record in `raw` so authentication verifies the typed
 * password against the real Purdy quadword. There is no password_hash field:
 * the credential lives in `raw` (uaf$q_pwd / uaf$w_salt / uaf$b_encrypt), not
 * in a text hash.
 * ------------------------------------------------------------------------- */
typedef struct {
    char     username[64];
    uint32_t uic_group;
    uint32_t uic_member;
    char     default_dir[256];
    char     flags[64];          /* UAI flag NAMES, from raw.uaf$l_flags       */
    char     privileges[256];    /* privilege NAMES, from raw.uaf$q_priv       */
    char     lgicmd[256];        /* login command file; empty => the default   */
    sysuaf_rms_record_t raw;     /* the on-disk $UAFDEF record (auth material)  */
} sysuaf_record_t;

/* FLAGS <-> mask: names in the file, a UAI$M_* longword in the API. These two
   are the only conversion between the two representations. */
uint32_t sysuaf_flags_to_mask(const char *flags);
void     sysuaf_mask_to_flags(uint32_t mask, char *out, size_t outsz);

/* Privilege mask -> comma-separated NAME string (inverse of
   parse_privilege_string / sysuaf_parse_privileges). Used to render the view's
   `privileges` field from the binary record's uaf$q_priv mask. */
void     sysuaf_format_privileges(uint64_t mask, char *out, size_t outsz);

/* ---- binary <-> view mapping (atomic flip) ------------------------------- */

/* Fill the text view fields FROM the binary $UAFDEF record `raw` and copy `raw`
 * into rec->raw. This is how sysuaf_lookup presents a record. */
void sysuaf_raw_to_view(const sysuaf_rms_record_t *raw, sysuaf_record_t *rec);

/* Sync the text view fields (username/uic/default_dir/flags/privileges/lgicmd)
 * INTO rec->raw's binary fields, PRESERVING the password area (uaf$q_pwd/salt/
 * encrypt -- set those with sysuaf_set_password). Used by AUTHORIZE / the seed
 * to build a record, and by $SETUAI after editing view fields. */
void sysuaf_view_to_raw(sysuaf_record_t *rec);

/* Compute the genuine Purdy (UAI$C_PURDY_S) hash of `password` for rec's
 * username and store it, a fresh salt, the algorithm byte and pwd length into
 * rec->raw's password area. rec->raw.uaf$t_username must be set first
 * (sysuaf_view_to_raw or sysuaf_record_set_username). Returns 0 on success. */
int  sysuaf_set_password(sysuaf_record_t *rec, const char *password);

/* As sysuaf_set_password but with an EXPLICIT salt word, for a reproducible
 * seed (mksysuaf) where a random salt would make the shipped SYSUAF differ
 * byte-for-byte between builds. Verification uses the stored salt either way. */
int  sysuaf_set_password_salt(sysuaf_record_t *rec, const char *password,
                              uint16_t salt);

/* Upcase/blank-pad `username` into rec->raw.uaf$t_username and rec->username. */
void sysuaf_record_set_username(sysuaf_record_t *rec, const char *username);

/* ---- lookup / authenticate ----------------------------------------------- */

/* Look up a user by name in the binary SYSUAF. Returns 0 on success (rec filled
   from the $UAFDEF record), -1 if not found or the binary engine is absent from
   this image (fail-honest -- an image that does not link LIBVMSRMS, or has no
   /dev/vms, gets -1, never a fabricated record). Case-insensitive. */
int sysuaf_lookup(const char *username, sysuaf_record_t *rec);

/* Look up the account holding UIC 'uic' ((group << 16) | member) by the
   secondary UIC key. Returns 0 on success, -1 if none holds it / engine absent.*/
int sysuaf_lookup_by_uic(uint32_t uic, sysuaf_record_t *rec);

/* Authenticate: 1 if `password` Purdy-verifies against rec->raw's stored
   quadword, else 0. A record whose UAF$B_ENCRYPT is not UAI$C_PURDY_S (an
   account with no Purdy password on file) authenticates NOTHING -- every
   password, right or wrong, is refused (the binary equivalent of the retired
   "empty hash => cannot authenticate" rule, vms-08f). NO SHA-256. */
int sysuaf_authenticate(const sysuaf_record_t *rec, const char *password);

/*
 * LOGIN-FLAG ENFORCEMENT (vms-c8fa), separate from the password check.
 * sysuaf_interactive_login_permitted: 0 if a DISABLING flag (DISUSER/DISACNT)
 *   forbids login, else 1. NULL fails closed.
 * sysuaf_account_captive: 1 if CAPTIVE, else 0.
 * Both read the view's FLAGS names through sysuaf_flags_to_mask.
 */
int sysuaf_interactive_login_permitted(const sysuaf_record_t *rec);
int sysuaf_account_captive(const sysuaf_record_t *rec);

/* Parse VMS privilege string (e.g. "TMPMBX,NETMBX,OPER") into bitmask. */
uint64_t sysuaf_parse_privileges(const char *priv_string);

/*
 * Resolve the login command file for an account (vms-e48): the SYSUAF LGICMD
 * field when set, otherwise SYSUAF_DEFAULT_LGICMD. 'out' is NUL-terminated.
 */
void sysuaf_login_command_file(const sysuaf_record_t *rec,
                               char *out, size_t outsz);

/*
 * Persist rec into the binary SYSUAF: in-place $UPDATE if the account exists,
 * else $PUT. Writes rec->raw through the binary engine over the ACP (or the
 * POSIX defer when /dev/vms is absent). Returns 0 on success, -1 on failure or
 * when the binary engine is absent from this image (fail-honest). Callers that
 * edited view fields must call sysuaf_view_to_raw(rec) first; callers that
 * changed the password use sysuaf_set_password(rec, ...).
 */
int sysuaf_write_record(const sysuaf_record_t *rec);

#endif /* SYSUAF_H */
