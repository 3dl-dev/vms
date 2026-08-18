/*
 * rightslist_rms.h - RIGHTSLIST as a genuine binary RMS Prolog-3 INDEXED file
 * (vms-3e0, epic vms-d0c). The real $RDBDEF identifier record (48 bytes,
 * oracle-pinned offsets) authored and read through the Files-11 Prolog-3 engine
 * (rms_prolog3.{c,h}) -- primary key = identifier VALUE longword (key of
 * reference 0), secondary key = identifier NAME (key of reference 1, resolved
 * via SIDR). This is the binary sibling of the SYSUAF rung (sysuaf_rms.{c,h});
 * it SUPERSEDES the ASCII colon/pipe-delimited RIGHTSLIST.DAT facade, storing
 * and reading the binary $RDBDEF record only.
 *
 * ⚠ KEY-OF-REFERENCE NUMBERING vs the oracle. The oracle (docs/oracle/
 * vax73-alpha84-rdbdef.md §1) measured THREE keys on a real RIGHTSLIST:
 *     key 0 = IDENTIFIER value (bin4 @0x00)  <- PRIMARY, matched here
 *     key 1 = HOLDER          (string @0x08) <- the holder-relationship key
 *     key 2 = NAME            (string @0x10)
 * This rung lands the PRIMARY (value, key 0 -- oracle-exact) plus the NAME key,
 * and DEFERS the HOLDER key (key 1) and the 16-byte holder records as a
 * labelled seam (see rdb_holder_record_t below and rightslist_rms.c). Because
 * HOLDER is deferred, NAME is added here as the next secondary (key of
 * reference 1); on a real VMS RIGHTSLIST NAME is key of reference 2. Closing
 * the holder seam (adding the HOLDER key at krf 1, shifting NAME to krf 2) is
 * what makes an OVMX-written RIGHTSLIST mountable by a real VMS AUTHORIZE.
 *
 * SUBSTRATE-AGNOSTIC (vms-5f0 thin-seam). The API takes an already-opened
 * rms_file_t handle (rms_io.h): an ACP channel window when /dev/vms is present
 * (Rule 9 / INV-6), or the rms_io POSIX backend (rms_io_posix_wrap) on the
 * executive-absent host defer / netbsd-vax cross. There is NO substrate #ifdef
 * here and no /vms path -- the handle decides the substrate, the record bytes
 * are identical either way. The caller owns the handle; rightslist_rms_close
 * frees only the bound Prolog-3 context.
 *
 * The live consumer (src/libvms/rtl/rightslist.c ASCII reader) is NOT flipped
 * onto this engine here -- that is C1 (vms-d92). This rung adds the real binary
 * path ALONGSIDE the ASCII path.
 *
 * Oracle grounding: docs/oracle/vax73-alpha84-rdbdef.md (record layout + keys),
 * docs/oracle/vax73-rights-database.md (the shipped identifier values),
 * docs/oracle/vax73-alpha84-rms-prolog3.md (the indexed envelope).
 */
#ifndef RIGHTSLIST_RMS_H
#define RIGHTSLIST_RMS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rms_prolog3.h"   /* the Prolog-3 engine + p3_le/p3_put_le accessors  */

/* =========================================================================
 * THE REAL BINARY $RDBDEF RECORDS -- oracle-pinned (vax73-alpha84-rdbdef.md)
 *
 * RIGHTSLIST stores ONE physical record shape with three keys; two logical
 * kinds are distinguished by whether the HOLDER field (@0x08) is zero:
 *   - identifier-DEFINITION record: 48 bytes, HOLDER == 0, carries the NAME.
 *   - HOLDER record:                16 bytes, HOLDER != 0, no NAME.
 * The record is 48/16 bytes on BOTH VAX V7.3 and Alpha V8.4 (oracle §5, no
 * architecture divergence -- the identifier value stays a longword and the
 * holder stays 8 bytes on the 64-bit machine). [PIN] marks a byte offset taken
 * verbatim from the oracle DUMP/RECORDS; [OVMX] marks an OVMX design choice
 * where the oracle does not publish a sub-field.
 * ========================================================================= */

#define RDB$K_IDENT_RECORD_SIZE   48   /* [PIN] identifier record length 0x30 */
#define RDB$K_HOLDER_RECORD_SIZE  16   /* [PIN] holder record length     0x10 */
#define RDB$K_NAME_LEN            32   /* [PIN] NAME field width (key seg len) */
#define RDB$K_MAX_NAME_CHARS      31   /* VMS identifier name <= 31 chars      */

/* [PIN] byte offsets from the oracle DUMP/RECORDS (little-endian fields). */
#define RDB$K_IDENTIFIER_OFF  0x00  /* identifier value longword (key 0)       */
#define RDB$K_ATTRIB_OFF      0x04  /* $KGBDEF attribute flags longword        */
#define RDB$K_HOLDER_OFF      0x08  /* holder field (key 1); 0 in a def record */
#define RDB$K_NAME_OFF        0x10  /* identifier name, blank-padded (key 2)   */

/* ---- identifier-DEFINITION record: 48 bytes, HOLDER == 0 ---- */
typedef struct {
    uint8_t rdb$l_identifier[4]; /* @0x00 [PIN] value (key 0), LE longword     */
    uint8_t rdb$l_attributes[4]; /* @0x04 [PIN] $KGBDEF attribute flags        */
    uint8_t rdb$q_holder[8];     /* @0x08 [PIN] holder (0 for a def record)    */
    char    rdb$t_name[32];      /* @0x10 [PIN] name, upcased, blank-padded    */
} rdb_identifier_record_t;       /* total: 48 bytes ($RDBDEF identifier) */

_Static_assert(sizeof(rdb_identifier_record_t) == RDB$K_IDENT_RECORD_SIZE,
               "rdb_identifier_record_t must be exactly 48 bytes ($RDBDEF)");
_Static_assert(offsetof(rdb_identifier_record_t, rdb$l_identifier) == RDB$K_IDENTIFIER_OFF,
               "RDB$L_IDENTIFIER must be at 0x00 (oracle [PIN])");
_Static_assert(offsetof(rdb_identifier_record_t, rdb$l_attributes) == RDB$K_ATTRIB_OFF,
               "attribute flags must be at 0x04 (oracle [PIN])");
_Static_assert(offsetof(rdb_identifier_record_t, rdb$q_holder) == RDB$K_HOLDER_OFF,
               "HOLDER must be at 0x08 (oracle [PIN])");
_Static_assert(offsetof(rdb_identifier_record_t, rdb$t_name) == RDB$K_NAME_OFF,
               "RDB$T_NAME must be at 0x10 (oracle [PIN])");

/* ---- HOLDER record: 16 bytes, HOLDER != 0 (the DEFERRED seam) ----
 * Created on real VMS by GRANT/IDENTIFIER (oracle §2b): {value, grant-attr,
 * holder UIC}, no NAME. The struct is defined (byte-exact at the oracle
 * offsets, _Static_asserted) so the layout is nailed down now, but this rung
 * does NOT store or query holder records -- the identifier<->holder mapping,
 * the HOLDER key (oracle key 1), and holder-relationship queries
 * ("what does this UIC hold?" / "who holds this identifier?") are the labelled
 * follow-on seam. See rightslist_rms.c. */
typedef struct {
    uint8_t rdb$l_identifier[4]; /* @0x00 [PIN] which identifier is held       */
    uint8_t rdb$l_attributes[4]; /* @0x04 [PIN] grant attribute flags          */
    uint8_t rdb$q_holder[8];     /* @0x08 [PIN] holder UIC as 8 bytes (key 1)  */
} rdb_holder_record_t;           /* total: 16 bytes ($RDBDEF holder) */

_Static_assert(sizeof(rdb_holder_record_t) == RDB$K_HOLDER_RECORD_SIZE,
               "rdb_holder_record_t must be exactly 16 bytes ($RDBDEF holder)");
_Static_assert(offsetof(rdb_holder_record_t, rdb$l_identifier) == RDB$K_IDENTIFIER_OFF,
               "holder RDB$L_IDENTIFIER must be at 0x00 (oracle [PIN])");
_Static_assert(offsetof(rdb_holder_record_t, rdb$l_attributes) == RDB$K_ATTRIB_OFF,
               "holder grant flags must be at 0x04 (oracle [PIN])");
_Static_assert(offsetof(rdb_holder_record_t, rdb$q_holder) == RDB$K_HOLDER_OFF,
               "HOLDER UIC must be at 0x08 (oracle [PIN])");

/* =========================================================================
 * IDENTIFIER-VALUE encoding (oracle §3). Bit 31 marks a non-UIC identifier.
 *   UIC identifier   : bit31 == 0, value == (group << 16) | member
 *   environmental    : bit31 == 1, value == 0x8000_00xx
 *   facility/general : bit31 == 1, value == 0x8001_xxxx, 0x96EE_xxxx, ...
 * ========================================================================= */
#define RDB$M_NONUIC   0x80000000u   /* bit 31 set => environmental/general */

/* value for a UIC identifier [g,m] (bit 31 clear). */
static inline uint32_t rdb_uic_value(uint16_t group, uint16_t member)
{
    return ((uint32_t)group << 16) | (uint32_t)member;
}
/* value for an environmental/general identifier (bit 31 set). */
static inline uint32_t rdb_nonuic_value(uint32_t low)
{
    return RDB$M_NONUIC | (low & 0x7fffffffu);
}
/* 1 if `value` is a non-UIC (environmental/general) identifier. */
static inline int rdb_value_is_nonuic(uint32_t value)
{
    return (value & RDB$M_NONUIC) != 0u;
}

/* =========================================================================
 * $KGBDEF ATTRIBUTE BITS -- ON-DISK $RDBDEF encoding.
 *
 * ⚠⚠ SOURCE-OF-TRUTH CONFLICT (flag, do not silently reconcile -- Rule 10).
 * The oracle (§2a, §6) MEASURED the on-disk attribute longword: identifier
 * OVMXRES, created /ATTRIBUTES=RESOURCE, dumped 0x00000001, and
 * SHOW/IDENTIFIER printed "RESOURCE" -- so in the ON-DISK $RDBDEF record
 *     RESOURCE == bit 0 (0x01)  [PIN]
 *     DYNAMIC  == bit 1 (0x02)  [PIN]
 * The EXISTING API-mask header src/libvms/include/kgbdef.h -- which documents
 * the mask sys$find_held/sys$find_holder RETURN -- disagrees: it has
 * KGB$M_RESOURCE = 0x04 (bit 2) and KGB$M_DYNAMIC = 0x01 (bit 0), matching the
 * PUBLISHED $KGBDEF symbol values. These are two different surfaces (on-disk
 * record vs runtime API mask) OR one of them is wrong; the oracle DUMP is the
 * authority for THIS file's on-disk bytes, so the RDB$V_ constants below follow
 * the measured on-disk encoding and are deliberately NOT the KGB$M_ masks.
 * REPORTED to the conductor to file an rd item reconciling the two surfaces.
 *
 * Only RESOURCE(0)/DYNAMIC(1) are oracle-measured. The oracle (§6) explicitly
 * did NOT exercise the higher bits, and warns against a guessed decode. Because
 * the published $KGBDEF (RESOURCE=bit2) conflicts with the measured bit0, the
 * higher bits CANNOT be grounded without a fresh oracle measurement -- so they
 * are marked [SEAM: UNVERIFIED] and must not be trusted until re-measured.
 * ========================================================================= */
#define RDB$V_RESOURCE       0u   /* [PIN] resource identifier                */
#define RDB$M_RESOURCE       (1u << RDB$V_RESOURCE)
#define RDB$V_DYNAMIC        1u   /* [PIN] dynamic (grantable) identifier     */
#define RDB$M_DYNAMIC        (1u << RDB$V_DYNAMIC)
/* [SEAM: UNVERIFIED on-disk bit -- see conflict note above; re-measure] */
#define RDB$V_HOLDER_HIDDEN  2u   /* [SEAM] holders hidden (bit unconfirmed)  */
#define RDB$M_HOLDER_HIDDEN  (1u << RDB$V_HOLDER_HIDDEN)
#define RDB$V_NAME_HIDDEN    3u   /* [SEAM] name hidden (bit unconfirmed)     */
#define RDB$M_NAME_HIDDEN    (1u << RDB$V_NAME_HIDDEN)
#define RDB$V_NOACCESS       4u   /* [SEAM] subsystem no-access (unconfirmed) */
#define RDB$M_NOACCESS       (1u << RDB$V_NOACCESS)

/* =========================================================================
 * RECORD FIELD ACCESSORS (binary, fixed-width little-endian -- substrate
 * agnostic; every field is a byte array read/written via p3_le/p3_put_le).
 * ========================================================================= */

static inline void rdb_ident_set_value(rdb_identifier_record_t *r, uint32_t v)
{
    p3_put_le32(r->rdb$l_identifier, v);
}
static inline uint32_t rdb_ident_value(const rdb_identifier_record_t *r)
{
    return p3_le32(r->rdb$l_identifier);
}
static inline void rdb_ident_set_attributes(rdb_identifier_record_t *r, uint32_t a)
{
    p3_put_le32(r->rdb$l_attributes, a);
}
static inline uint32_t rdb_ident_attributes(const rdb_identifier_record_t *r)
{
    return p3_le32(r->rdb$l_attributes);
}

/* Set the NAME primary field: upcased (VMS folds identifier case), blank-padded
 * to 32 at [PIN] offset 0x10. Also used to build the secondary search key so a
 * lookup matches the stored record. */
void rdb_ident_set_name(rdb_identifier_record_t *r, const char *name);

/* Trim the 32-byte blank-padded RDB$T_NAME into a NUL-terminated C string.
 * `out` must hold RDB$K_NAME_LEN + 1 bytes. */
void rdb_ident_name(const rdb_identifier_record_t *r, char *out);

/* =========================================================================
 * RIGHTSLIST indexed-file API (over rms_prolog3, the genuine Files-11 engine)
 * ========================================================================= */

/* Key of reference in the RIGHTSLIST indexed file (see the numbering note at
 * the top of this header re: the deferred HOLDER key). */
#define RIGHTSLIST_KRF_VALUE  0u   /* primary: identifier value @0x00 (oracle key 0) */
#define RIGHTSLIST_KRF_NAME   1u   /* secondary: identifier name @0x10 (oracle key 2) */

/* Bound RIGHTSLIST indexed file: a borrowed rms_file_t handle plus the
 * Prolog-3 context authored/bound over it. */
typedef struct rightslist_rms_file {
    rms_file_t *f;     /* borrowed handle (ACP window or POSIX wrap) -- NOT owned */
    p3_ctx_t   *ctx;   /* bound/created Prolog-3 context (owned; freed on close)  */
} rightslist_rms_file_t;

/* Author a fresh, EMPTY RIGHTSLIST indexed file over the (writable) handle `f`:
 * a Prolog-3 image with the primary identifier-VALUE key (unique) and the
 * secondary identifier-NAME key (unique). On success *rf is a bound context
 * ready for rightslist_put_identifier. Returns RMS$_CREATED, or an RMS$_ error
 * (RMS$_FAB for a NULL argument; the engine's RMS$_WPL/RMS$_KEY/RMS$_PLG/
 * RMS$_DME otherwise). */
uint32_t rightslist_rms_create(rms_file_t *f, rightslist_rms_file_t *rf);

/* Bind an EXISTING RIGHTSLIST indexed file on the handle `f` (parses the
 * prologue; expects the 2-key VALUE+NAME image rightslist_rms_create authors).
 * Returns RMS$_NORMAL, RMS$_FAB (NULL arg), or RMS$_PLG on a bad/foreign
 * prologue. */
uint32_t rightslist_rms_open(rms_file_t *f, rightslist_rms_file_t *rf);

/* Write one $RDBDEF identifier-DEFINITION record (48 bytes): $PUT into the
 * primary VALUE key; the engine maintains the NAME SIDR automatically from the
 * record's embedded name field. HOLDER must be zero (a definition record);
 * this rung does not store holder records. Returns RMS$_NORMAL, RMS$_DUP
 * (value already present), RMS$_FAB, or an engine I/O status. */
uint32_t rightslist_put_identifier(rightslist_rms_file_t *rf,
                                   const rdb_identifier_record_t *rec);

/* Read the identifier record for `name` by the NAME key (upcased/blank-padded
 * to 32 internally). Returns RMS$_NORMAL, RMS$_RNF (no such identifier),
 * RMS$_FAB. */
uint32_t rightslist_get_by_name(rightslist_rms_file_t *rf, const char *name,
                                rdb_identifier_record_t *out);

/* Read the identifier record for `value` by the PRIMARY VALUE key. This is the
 * value->name resolution path (read the record, then rdb_ident_name).
 * Returns RMS$_NORMAL, RMS$_RNF (no such value), RMS$_FAB. */
uint32_t rightslist_get_by_value(rightslist_rms_file_t *rf, uint32_t value,
                                 rdb_identifier_record_t *out);

/* Free the bound Prolog-3 context. Does NOT close/unwrap the handle `f`
 * (the caller owns it). Safe on a zeroed / partially-created rf. */
void rightslist_rms_close(rightslist_rms_file_t *rf);

#endif /* RIGHTSLIST_RMS_H */
