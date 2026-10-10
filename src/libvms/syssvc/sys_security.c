/*
 * sys_security.c - Security System Services
 *
 * Implements VMS-style protection checking based on UIC (User
 * Identification Code) and SOGW protection masks.
 *
 * UIC format: [group,member] packed into uint32_t
 *   High 16 bits = group number (mapped from Linux GID)
 *   Low 16 bits  = member number (mapped from Linux UID)
 *
 * Protection mask: 16 bits in SOGW order (PINNED -- see
 * src/libvms/include/ovmx_fileprot.h for the citation and for why this
 * is the shared definition; this file used to carry its own MIRROR-IMAGE
 * shifts and bit values that disagreed with vmsfs_protect.c, vms-f81):
 *   Bits  3-0:  System access (RWED)
 *   Bits  7-4:  Owner access (RWED)
 *   Bits 11-8:  Group access (RWED)
 *   Bits 15-12: World access (RWED)
 *
 * Each 4-bit nibble: bit0=Read, bit1=Write, bit2=Execute, bit3=Delete
 * A SET bit means access is DENIED (VMS convention: protection bits
 * deny access, the opposite of Unix permission bits).
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-PARTIAL: sys$asctoid (vms-44a) -- exec: RIGHTSLIST.DAT is read over the
 *     executive's Files-11 ACP (RMS-over-ACP, LIBVMSRMS), and the logicals that
 *     locate it are the executive's.
 * OVMX-LOCAL: sys$asctoid -- the name-to-value lookup in the rights-database
 *     reader (rtl/rightslist.c) runs in this process; the identifier's ATTRIBUTE
 *     flags come from the identifier's record when LIBVMSRMS is bound.
 * OVMX-PARTIAL: sys$grantid (vms-7d5a) -- exec: the process rights list is the
 *     executive's (VMS_IOCTL_RIGHTS); it checks CMKRNL and answers WASCLR/WASSET.
 * OVMX-LOCAL: sys$grantid -- a name is resolved here through $ASCTOID; a target
 *     named by process name is refused SS$_UNSUPPORTED.
 * OVMX-PARTIAL: sys$revokid (vms-7d5a) -- exec: the same executive rights list.
 * OVMX-LOCAL: sys$revokid -- as sys$grantid.
 * OVMX-PARTIAL: sys$parse_acl (vms-d404) -- exec: identifier names are
 *     looked up in RIGHTSLIST.DAT read over the executive ACP ($ASCTOID).
 * OVMX-LOCAL: sys$parse_acl -- the ACE text is parsed into the binary ACE
 *     (docs/oracle/vax73-acl.md layout) in this process.
 * OVMX-PARTIAL: sys$format_acl (vms-d404) -- exec: identifier values are named
 *     from RIGHTSLIST.DAT read over the executive ACP ($IDTOASC).
 * OVMX-LOCAL: sys$format_acl -- the text is composed in this process; width,
 *     terminator and indent are not applied (one ACE, one line).
 * OVMX-PARTIAL: sys$idtoasc (vms-44a) -- exec: the same ACP read of RIGHTSLIST.DAT.
 * OVMX-LOCAL: sys$idtoasc -- the value-to-name lookup runs in this process; a
 *     wildcard context (ctx) is refused, one identifier is looked up.
 * OVMX-PARTIAL: sys$check_privilegew (vms-44a) -- exec: the privilege mask the
 *     request is checked against is the executive's own current mask for this
 *     process (vms_kif_getjpi_self); this process never supplies it.
 * OVMX-LOCAL: sys$check_privilegew -- the mask comparison and the bit-to-SS$_NOxxx
 *     mapping (10244 + 8*bit, the oracle's SS$ numbering) run in this process;
 *     the audit item list is read but, as on a system with auditing disabled,
 *     nothing is logged.
 * OVMX-PARTIAL: sys$audit_eventw (vms-44a) -- exec: the AUDIT privilege it
 *     demands is read from the executive's own current mask for this process
 *     (vms_kif_getjpi_self); a caller without it gets SS$_NOAUDIT, which the V7.3
 *     lab prints as "operation requires AUDIT privilege".
 * OVMX-LOCAL: sys$audit_eventw -- the item-list validation and the decision that
 *     the event is not audited run in this process. OVMX has no audit server and
 *     no audit journal (docs/compat/facilities/audit.yaml: audit$security_journal
 *     absent), so no event class is ever enabled and an unforced event is
 *     genuinely not audited (SS$_NORMAL, nothing written). A caller that FORCES
 *     a record (NSA$M_NOEVTCHECK or NSA$M_MANDATORY) is refused SS$_UNSUPPORTED
 *     rather than told a record was written.
 * OVMX-PARTIAL: sys$create_uid (vms-44a) -- exec: the node field is the system's
 *     SCSNODE name, read through $GETSYI (the same as the lab OpenVMS Alpha V8.4
 *     node, whose uids carry "ALPHA1" there).
 * OVMX-LOCAL: sys$create_uid -- the uid is assembled in this process as an OSF DCE
 *     uuid version 1: the clock (sys$gettim), a per-process random clock sequence
 *     (sys$get_entropy) and that node field. Uniqueness is the uuid argument (time +
 *     sequence + node), not an executive-issued number. A node with no readable
 *     SCSNODE falls back to a random multicast-bit node (RFC 4122 4.5).
 * OVMX-PARTIAL: sys$chkpro (vms-d404) -- exec: with no user profile the subject
 *     is the calling process as the executive holds it (UIC, current
 *     privileges, the rights list $GRANTID changes).
 * OVMX-LOCAL: sys$chkpro -- the decision (protection code, ACL, privileges) is
 *     computed in this process from the item list, as $CHKPRO computes it for
 *     its caller; it enforces nothing by itself. Object profiles (objpro) are
 *     refused SS$_UNSUPPORTED; output items are not filled.
 * OVMX-PARTIAL: sys$create_user_profile (vms-d404) -- exec: SYSUAF.DAT and
 *     RIGHTSLIST.DAT are read over the executive ACP ($GETUAI, $FIND_HELD).
 * OVMX-LOCAL: sys$create_user_profile -- the profile is assembled in this
 *     process; its layout is this implementation's own (opaque to callers).
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include "starlet.h"
#include "rightslist.h"
#include "lib$routines.h"
#include "ssdef.h"
#include "ovmx_secparam.h"
#include "ovmx_fileprot.h"
#include "vms_kif.h"
#include "prvdef.h"
#include "chpdef.h"
#include "iledef.h"
#include "uaidef.h"
#include "rmsdef.h"
#include "descrip.h"
#include "nsadef.h"
#include "prcdef.h"      /* SYI$_SCSNODE */

/*
 * Protection access type flags and category offsets -- aliased onto the
 * single pinned encoding in ovmx_fileprot.h (vms-f81) so this file and
 * vmsfs_protect.c cannot drift apart again. Kept as PROT$M_/PROT$V_ names
 * because that is what sys$chkpro's own parameter documentation below
 * and its callers already spell them.
 */
#define PROT$M_READ    VMS_PROT_R
#define PROT$M_WRITE   VMS_PROT_W
#define PROT$M_EXECUTE VMS_PROT_E
#define PROT$M_DELETE  VMS_PROT_D

#define PROT$V_SYSTEM  VMS_PROT_SYSTEM_SHIFT
#define PROT$V_OWNER   VMS_PROT_OWNER_SHIFT
#define PROT$V_GROUP   VMS_PROT_GROUP_SHIFT
#define PROT$V_WORLD   VMS_PROT_WORLD_SHIFT

/*
 * OVMX_MAXSYSGROUP -- the SYSGEN parameter that decides which UIC groups
 * get the SYSTEM protection category. Defined ONCE, in
 * src/libvms/include/ovmx_secparam.h (see that file for the two-source
 * pin and the provenance of the value) -- not here, and not re-declared
 * by any test. vms-2b8 round 4 pinned the value in two independent
 * places (this file's comment and tests/libvms/test_protection.c's own
 * #define) that happened to agree; round 5 collapsed them into this one
 * header specifically so they cannot drift apart the next time either
 * side is edited without the other.
 *
 * THE BOUNDARY IS PROVEN BY MUTATION, NOT JUST STATED, through THREE
 * independent checks in tests/libvms/test_protection.c (see its own
 * comments): a `_Static_assert` pins the NUMBER at compile time; a
 * group == OVMX_MAXSYSGROUP case pins the <= vs < OPERATOR at runtime;
 * a hardcoded group-9 case independently pins the boundary is not above
 * 8. Changing this header's value to anything but 8 fails the build via
 * the static_assert before any of the runtime cases get a chance to run;
 * changing sys_security.c's comparison operator instead (leaving the
 * value at 8) is what the runtime cases catch.
 */

/*
 * uic_is_system - does this UIC get the SYSTEM protection category?
 *
 * WHAT THIS REPLACES, so it does not come back: both checks below used to
 * say `if (uic == 0) -> SYSTEM category`, commented "UID 0 (root) is
 * treated as SYSTEM". OpenVMS has no root and no UIC [0,0]; that rule was
 * invented for the OVMX platform and, while every VMS session on OVMX ran
 * as Linux root, it was also inert -- caller_uic 0 equalled the owner_uic 0
 * of every root-created file, so the owner branch would have answered the
 * same. The moment LOGINOUT started dropping to the authenticated user's
 * credentials (vms-2b8), the SYSTEM account's real UIC [1,4] stopped
 * matching it and fell through to the WORLD nibble on every file in the VMS
 * tree -- OVMX denying what VMS grants.
 *
 * The documented VMS rule is a group comparison, not an equality test: the
 * SYSTEM category covers every UIC whose GROUP number is less than or equal
 * to MAXSYSGROUP (OpenVMS Guide to System Security, "System" access
 * category). Group 0 is not a valid VMS UIC group at all, so root's [0,0]
 * is covered incidentally by 0 <= 8 rather than by a rule of its own.
 *
 * The privilege terms (SYSPRV, GRPPRV, BYPASS, READALL) are $CHKPRO's, in
 * chk_decide() below; this is only the UIC-group rule.
 */
static int uic_is_system(uint32_t uic)
{
    return ((uic >> 16) & 0xFFFFu) <= OVMX_MAXSYSGROUP;
}

/*
 * get_uic - Get the current process UIC: the executive's, from this process's
 * PCB (rd vms-ac48). The substrate uid/gid carry no VMS meaning. 0 (no UIC)
 * when the executive cannot answer.
 */
static uint32_t get_uic(void) {
    struct vms_procinfo pi;
    memset(&pi, 0, sizeof pi);
    return (vms_kif_getjpi_self(&pi) & 1) ? pi.uic : 0;
}

/*
 * vms$get_uic - Public accessor for the current UIC.
 */
uint32_t vms$get_uic(void) {
    return get_uic();
}
/* ======================================================================
 * $CHKPRO / $CREATE_USER_PROFILE (vms-d404).
 *
 *   sys$chkpro(itmlst, objpro, subjpro)
 *   sys$create_user_profile(usrnam, itmlst, flags, usrpro, usrprolen, contxt)
 *
 * Grounded on OpenVMS VAX V7.3 (docs/oracle/semantics/chkpro/vax73.txt, the
 * spec tools/oracle/semantic/specs/chkpro.py); the ACL rules are those of
 * docs/oracle/vax73-acl.md, which the executive ACP applies to files. The
 * decision, in order:
 *
 *   1. BYPASS grants.
 *   2. The first identifier ACE (not DEFAULT) whose identifiers the subject all
 *      holds decides; if it grants every wanted bit, access is granted.
 *   3. READALL grants, but only with CHP$M_USEREADALL and no matching ACE.
 *   4. Otherwise access is granted when every wanted bit is left un-denied by
 *      some category the subject is in: system (UIC group <= MAXSYSGROUP,
 *      SYSPRV, or GRPPRV for an object of its own group), owner, and -- only
 *      when no ACE matched -- group and world. The protection code denies only
 *      READ/WRITE/EXECUTE/DELETE; CONTROL and the higher bits are left to the
 *      ACL (no ACE matching: granted).
 *
 * The subject is the user profile `subjpro` describes, or else the calling
 * process (its UIC, current privileges and rights list are the executive's);
 * CHP$_UIC, CHP$_PRIV and CHP$_RIGHTS replace the corresponding part. A
 * profile is this implementation's own opaque block: UIC, privileges and the
 * identifiers the user holds in RIGHTSLIST.DAT.
 * ====================================================================== */
#define CHKPRO_RIGHTS_MAX 64
#define USRPRO_MAGIC      0x5055564Fu      /* "OVUP" */

struct ovmx_usrpro {
    uint32_t magic;
    uint32_t uic;
    uint64_t privs;
    uint32_t nrights;
    uint32_t rights[CHKPRO_RIGHTS_MAX];
};
#define USRPRO_LEN(n) ((uint32_t)(offsetof(struct ovmx_usrpro, rights) + 4u * (n)))

struct chk_subject {
    uint32_t uic;
    uint64_t privs;
    uint32_t nrights;
    uint32_t rights[CHKPRO_RIGHTS_MAX];
};

static uint32_t chk_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Does the subject hold identifier `id`? `*` is held by all; a UIC identifier
 * names the subject's UIC, either half possibly the wildcard ([*,m] group
 * 0x3FFF, [g,*] member 0xFFFF); a general identifier is in its rights list. */
static int chk_holds(const struct chk_subject *s, uint32_t id)
{
    uint32_t i;

    if (id == 0xFFFFFFFFu)
        return 1;
    if (!(id & 0x80000000u)) {
        uint32_t g = (id >> 16) & 0x3FFFu, m = id & 0xFFFFu;
        return (g == 0x3FFFu || g == ((s->uic >> 16) & 0xFFFFu)) &&
               (m == 0xFFFFu || m == (s->uic & 0xFFFFu));
    }
    for (i = 0; i < s->nrights && i < CHKPRO_RIGHTS_MAX; i++)
        if (s->rights[i] == id)
            return 1;
    return 0;
}

/* The first matching identifier ACE: 1 and its access mask, or 0. */
static int chk_acl_match(const struct chk_subject *s, const uint8_t *acl, size_t len,
                         uint32_t *access)
{
    size_t pos = 0;

    while (acl && pos + 4u <= len) {
        const uint8_t *ace = acl + pos;
        unsigned sz = ace[0];
        uint16_t flags = (uint16_t)(ace[2] | (ace[3] << 8));

        if (sz < 4u || pos + sz > len)
            break;
        if (ace[1] == 1u /* ACE$C_KEYID */ && sz >= 12u && !(flags & 0x0100u /* DEFAULT */)) {
            unsigned k, nid = (sz - 8u) / 4u;
            int all = 1;

            for (k = 0; k < nid && all; k++)
                all = chk_holds(s, chk_rd32(ace + 8u + 4u * k));
            if (all) {
                *access = chk_rd32(ace + 4u);
                return 1;
            }
        }
        pos += sz;
    }
    return 0;
}

static int chk_decide(const struct chk_subject *s, uint32_t want, uint32_t flags,
                      uint32_t owner, uint16_t prot, const uint8_t *acl, size_t acl_len)
{
    uint32_t ace_access = 0, denied, acc_group = (s->uic >> 16) & 0xFFFFu;
    int matched, is_system;

    if (s->privs & PRV$M_BYPASS)
        return 1;
    matched = chk_acl_match(s, acl, acl_len, &ace_access);
    if (matched && (ace_access & want) == want)
        return 1;
    if (!matched && (s->privs & PRV$M_READALL) && (flags & CHP$M_USEREADALL))
        return 1;
    is_system = uic_is_system(s->uic) || (s->privs & PRV$M_SYSPRV) ||
                ((s->privs & PRV$M_GRPPRV) && acc_group == ((owner >> 16) & 0xFFFFu));
    denied = want;
#define CHK_ALLOW(nib) (denied &= ~(want & ~((uint32_t)(nib) & 0xFu)))
    if (is_system)
        CHK_ALLOW(prot >> PROT$V_SYSTEM);
    if (s->uic == owner)
        CHK_ALLOW(prot >> PROT$V_OWNER);
    if (!matched) {
        if (acc_group == ((owner >> 16) & 0xFFFFu))
            CHK_ALLOW(prot >> PROT$V_GROUP);
        CHK_ALLOW(prot >> PROT$V_WORLD);
    }
#undef CHK_ALLOW
    return denied == 0;
}

/* The calling process as subject: the executive's UIC, current privileges and
 * rights list. No executive -> SS$_NOSUCHDEV (never a guessed identity). */
static uint32_t chk_self(struct chk_subject *s, int need_uic, int need_priv, int need_rights)
{
    if (need_uic || need_priv) {
        struct vms_procinfo info;
        uint32_t st;

        memset(&info, 0, sizeof(info));
        st = vms_kif_getjpi_self(&info);
        if (!(st & 1))
            return st;
        if (need_uic)
            s->uic = info.uic;
        if (need_priv)
            s->privs = info.cur_privs;
    }
    if (need_rights) {
        uint32_t n = 0, st = vms_kif_rights_list(0, s->rights, NULL, CHKPRO_RIGHTS_MAX, &n);
        if (!(st & 1))
            return st;
        s->nrights = n < CHKPRO_RIGHTS_MAX ? n : CHKPRO_RIGHTS_MAX;
    }
    return SS$_NORMAL;
}

uint32_t sys$chkpro(void *itmlst, void *objpro, void *subjpro)
{
    const ILE3 *it = (const ILE3 *)itmlst;
    struct chk_subject s;
    uint32_t want = 0, flags = 0, owner = 0, st;
    uint16_t prot = 0;
    const uint8_t *acl = NULL;
    size_t acl_len = 0;
    int have_uic = 0, have_priv = 0, have_rights = 0;

    if (!itmlst)
        return SS$_ACCVIO;
    if (objpro)
        return SS$_UNSUPPORTED;             /* object profiles: not provided */
    memset(&s, 0, sizeof(s));
    if (subjpro) {
        const struct dsc$descriptor_s *d = (const struct dsc$descriptor_s *)subjpro;
        const struct ovmx_usrpro *up = (const struct ovmx_usrpro *)d->dsc$a_pointer;

        if (!up || d->dsc$w_length < USRPRO_LEN(0) || up->magic != USRPRO_MAGIC ||
            up->nrights > CHKPRO_RIGHTS_MAX || d->dsc$w_length < USRPRO_LEN(up->nrights))
            return SS$_BADPARAM;
        s.uic = up->uic;
        s.privs = up->privs;
        s.nrights = up->nrights;
        memcpy(s.rights, up->rights, 4u * up->nrights);
        have_uic = have_priv = have_rights = 1;
    }
    for (; it->ile3$w_length != 0 || it->ile3$w_code != 0; it++) {
        const uint8_t *b = (const uint8_t *)it->ile3$ps_bufaddr;
        unsigned len = it->ile3$w_length;

        switch (it->ile3$w_code) {
        case CHP$_ACCESS: if (b && len >= 4) want = chk_rd32(b); break;
        case CHP$_FLAGS:  if (b && len >= 4) flags = chk_rd32(b); break;
        case CHP$_OWNER:  if (b && len >= 4) owner = chk_rd32(b); break;
        case CHP$_PROT:   if (b && len >= 2) prot = (uint16_t)(b[0] | (b[1] << 8)); break;
        case CHP$_ACL:    acl = b; acl_len = b ? len : 0; break;
        case CHP$_UIC:
            if (b && len >= 4) { s.uic = chk_rd32(b); have_uic = 1; }
            break;
        case CHP$_PRIV:
            if (b && len >= 8) {
                s.privs = (uint64_t)chk_rd32(b) | ((uint64_t)chk_rd32(b + 4) << 32);
                have_priv = 1;
            }
            break;
        case CHP$_RIGHTS: {
            unsigned k;
            s.nrights = 0;
            for (k = 0; b && k + 8u <= len && s.nrights < CHKPRO_RIGHTS_MAX; k += 8u)
                s.rights[s.nrights++] = chk_rd32(b + k);
            have_rights = 1;
            break;
        }
        default:
            break;                          /* output and audit items: not filled */
        }
    }
    if (!have_uic || !have_priv || !have_rights) {
        st = chk_self(&s, !have_uic, !have_priv, !have_rights);
        if (!(st & 1))
            return st;
    }
    return chk_decide(&s, want, flags, owner, prot, acl, acl_len) ? SS$_NORMAL : SS$_NOPRIV;
}

/* The rights database's holder walk (LIBVMSRMS, weak like the name lookups). */
uint32_t sys$find_held(const uint32_t *holder, uint32_t *id, uint32_t *attrib, uint32_t *contxt);
uint32_t sys$finish_rdb(uint32_t *contxt);
#pragma weak sys$find_held
#pragma weak sys$finish_rdb

uint32_t sys$create_user_profile(const struct dsc$descriptor_s *usrnam, void *itmlst,
                                 uint32_t flags, void *usrpro, uint32_t *usrprolen,
                                 uint32_t *contxt)
{
    struct ovmx_usrpro up;
    uint32_t uic = 0, st, need;
    uint64_t privs = 0;
    uint16_t uiclen = 0, prvlen = 0;
    ILE3 uai[3];

    (void)itmlst; (void)contxt;
    if (!usrprolen)
        return SS$_ACCVIO;
    if (!usrnam || !usrnam->dsc$a_pointer || usrnam->dsc$w_length == 0)
        return RMS$_RNF;                    /* oracle CUP.EMPTYNAME */
    memset(uai, 0, sizeof(uai));
    uai[0].ile3$w_length = 4; uai[0].ile3$w_code = UAI$_UIC;
    uai[0].ile3$ps_bufaddr = &uic; uai[0].ile3$ps_retlen_addr = &uiclen;
    uai[1].ile3$w_length = 8;
    uai[1].ile3$w_code = (flags & CHP$M_DEFPRIV) ? UAI$_DEF_PRIV : UAI$_PRIV;
    uai[1].ile3$ps_bufaddr = &privs; uai[1].ile3$ps_retlen_addr = &prvlen;
    st = sys$getuai(0, NULL, (struct dsc$descriptor_s *)usrnam, uai, NULL, NULL, 0);
    if (!(st & 1))
        return (st == SS$_NOSUCHID) ? RMS$_RNF : st;   /* no such user: CUP.NOSUCH */

    memset(&up, 0, sizeof(up));
    up.magic = USRPRO_MAGIC;
    up.uic = uic;
    up.privs = privs;
    if (!sys$find_held || !sys$finish_rdb)
        return SS$_UNSUPPORTED;             /* no rights-database reader in this image */
    {
        uint32_t holder[2] = { uic, 0 }, ctx = 0, id, at;

        while (up.nrights < CHKPRO_RIGHTS_MAX &&
               (sys$find_held(holder, &id, &at, &ctx) & 1))
            up.rights[up.nrights++] = id;
        (void)sys$finish_rdb(&ctx);
    }
    need = USRPRO_LEN(up.nrights);
    if (!usrpro) {
        *usrprolen = need;                  /* the length the caller must provide */
        return SS$_NORMAL;
    }
    if (*usrprolen < need)
        return SS$_BUFFEROVF;
    memcpy(usrpro, &up, need);
    *usrprolen = need;
    return SS$_NORMAL;
}

/*
 * vms$check_access -- DELETED AS A DECISION POINT (vms-2b8, operator ruling
 * 2026-07-31, Rule 10 applied to an internal interface).
 *
 * This used to be a second, parallel implementation of the same SOGW
 * category logic as sys$chkpro above, called only from
 * src/vmsrms/rms_core.c's rms_check_protection() to pre-check an RMS
 * $OPEN/$CREATE/$ERASE before touching the filesystem.
 *
 * IT COULD NOT ENFORCE ANYTHING. Rebuilding the bootable image with this
 * function returning 1 UNCONDITIONALLY changed nothing about the failure
 * it was thought to gate, because DCL's COPY/TYPE/DELETE call fopen()/
 * unlink() directly and never enter RMS at all -- so the one path a real
 * user takes was never reaching this check in the first place. Where RMS
 * IS the path, the real enforcer is the executive: the bootable runtime's
 * own file protection is decided by src/kernel/vmsfs/vmsfs_blkdev.c
 * through the Linux DAC bits vmsfs derives from the VMS protection mask
 * (measured: chmod 0777 over the entire [SYS0] tree at the Linux layer did
 * NOT let GUEST write SYS$SYSTEM: on the real runtime). This function was
 * a userspace SECOND OPINION computed from the same st_mode the kernel
 * module already turns into the real decision, and it could only ever be
 * WRONG in the same direction: a false denial, never a false grant, because
 * it has no notion of SYSPRV/BYPASS/READALL/GRPPRV (see uic_is_system()'s
 * own comment) and the executive's decision runs regardless of what this
 * one returned.
 *
 * A second decision point that cannot enforce and can only add false
 * denials is exactly where a future agent would "add SYSPRV support" and
 * ship the reported-but-unenforced state Rule 10 forbids -- so it is
 * deleted rather than fixed. The privilege overrides this function was
 * missing belong in vmsfs.ko, where the mask already lives (tracked
 * separately: vms-f15/vms-36d). rms_check_protection() and its callers in
 * src/vmsrms/rms_core.c are deleted with it; RMS's $OPEN/$CREATE/$ERASE now
 * let the real open()/unlink() -- and the executive behind it -- be the
 * only enforcer, exactly as the DCL path already does.
 */

/*
 * $ASCTOID - convert an identifier name to its binary value.
 *
 *   sys$asctoid(name, &id, &attrib)
 *
 * SS$_NOSUCHID when the rights database has no such identifier (or cannot be
 * read -- never a built-in table, rtl/rightslist.c).
 */
/* An identifier's attributes from its RIGHTSLIST definition record (LIBVMSRMS,
 * weak like the name lookups, vms-7d5a). */
uint32_t ovmx_rightslist_attributes(uint32_t value, uint32_t *attrib);
#pragma weak ovmx_rightslist_attributes

uint32_t sys$asctoid(const struct dsc$descriptor_s *name, uint32_t *id,
                     uint32_t *attrib)
{
    if (!name || !name->dsc$a_pointer || name->dsc$w_length == 0 || !id)
        return SS$_BADPARAM;
    char buf[64];
    size_t n = name->dsc$w_length;
    while (n > 0 && name->dsc$a_pointer[n - 1] == ' ')
        n--;                                    /* a descriptor may be blank-padded */
    if (n == 0 || n >= sizeof(buf))
        return SS$_IVIDENT;
    memcpy(buf, name->dsc$a_pointer, n);
    buf[n] = '\0';
    uint32_t v = 0;
    if (rightslist_name_to_value(buf, &v) != 0)
        return SS$_NOSUCHID;
    *id = v;
    if (attrib) {
        *attrib = 0;
        if (ovmx_rightslist_attributes)
            (void)ovmx_rightslist_attributes(v, attrib);   /* the identifier's own */
    }
    return SS$_NORMAL;
}

/*
 * $IDTOASC - convert a binary identifier to its name.
 *
 *   sys$idtoasc(id, &namlen, nambuf, &resid, &attrib, &ctx)
 */
uint32_t sys$idtoasc(uint32_t id, uint16_t *namlen, struct dsc$descriptor_s *nambuf,
                     uint32_t *resid, uint32_t *attrib, uint32_t *ctx)
{
    if (ctx && *ctx != 0)
        return SS$_BADPARAM;                    /* wildcard continuation not supported */
    char buf[64];
    if (rightslist_value_to_name(id, buf, sizeof(buf)) != 0)
        return SS$_NOSUCHID;
    size_t n = strlen(buf);
    if (nambuf && nambuf->dsc$a_pointer) {
        uint16_t l = (uint16_t)n;
        (void)lib$scopy_r_dx(&l, buf, nambuf);
    }
    if (namlen)
        *namlen = (uint16_t)n;
    if (resid)
        *resid = id;
    if (attrib) {
        *attrib = 0;
        if (ovmx_rightslist_attributes)
            (void)ovmx_rightslist_attributes(id, attrib);
    }
    return SS$_NORMAL;
}

/*
 * sys$check_privilegew - test the caller's current privileges.
 *
 * privnam points at a 64-bit privilege mask (the form the corpus uses, flags 0).
 * Every requested bit must be held in the executive's current mask; the first
 * missing bit (lowest first) yields the real per-privilege code
 * SS$_NOCMKRNL + 8*bit (oracle SS$ numbering, V7.3 SSDEF), SS$_NOPRIV beyond the
 * named range. The audit status block, when given, receives SS$_NORMAL (auditing
 * of the check is disabled; nothing is written to an audit log).
 */
uint32_t (sys$check_privilegew)(uint32_t efn, const void *privnam, uint32_t bitnum,
                                uint32_t flags, const void *itmlst, uint32_t *audsts,
                                void *astadr, uint64_t astprm)
{
    struct vms_procinfo self;
    uint64_t req, missing;
    uint32_t status = SS$_NORMAL;

    (void)efn; (void)bitnum; (void)flags; (void)itmlst; (void)astadr; (void)astprm;
    if (!privnam)
        return SS$_ACCVIO;
    memcpy(&req, privnam, sizeof(req));
    memset(&self, 0, sizeof(self));
    if (!(vms_kif_getjpi_self(&self) & 1))
        return SS$_NOSUCHDEV;   /* no executive: fail honestly, never fake */
    missing = req & ~self.cur_privs;
    if (missing) {
        unsigned bit = (unsigned)__builtin_ctzll(missing);
        status = bit <= 38 ? SS$_NOCMKRNL + 8u * bit : SS$_NOPRIV;
    }
    if (audsts)
        *audsts = SS$_NORMAL;
    return status;
}

/*
 * sys$audit_eventw - request that a security event be audited.
 *
 * AUDIT privilege required (SS$_NOAUDIT otherwise), then the NSA$_ item list is
 * walked: an NSA$_EVENT_TYPE item is mandatory (SS$_BADPARAM). The state of the
 * system is that no auditing is enabled (there is no audit server or journal), so
 * an event that is only audited when its class is enabled is not audited and the
 * service returns SS$_NORMAL having logged nothing -- true of a VMS system with
 * auditing disabled too. A caller that forces a record cannot be given one:
 * SS$_UNSUPPORTED, never a success that wrote nothing.
 */
uint32_t (sys$audit_eventw)(uint32_t efn, uint32_t flags, const void *itmlst,
                            void *audsts, void *astadr, uint64_t astprm)
{
    struct vms_procinfo self;
    const struct item_list_3 *it;
    int have_type = 0;

    (void)efn; (void)audsts; (void)astadr; (void)astprm;
    memset(&self, 0, sizeof(self));
    if (!(vms_kif_getjpi_self(&self) & 1))
        return SS$_NOSUCHDEV;   /* no executive: fail honestly, never fake */
    if (!(self.cur_privs & PRV$M_AUDIT))
        return SS$_NOAUDIT;
    if (!itmlst)
        return SS$_BADPARAM;
    for (it = (const struct item_list_3 *)itmlst; it->buflen || it->item_code; it++)
        if (it->item_code == NSA$_EVENT_TYPE)
            have_type = 1;
    if (!have_type)
        return SS$_BADPARAM;
    if (flags & (NSA$M_NOEVTCHECK | NSA$M_MANDATORY))
        return SS$_UNSUPPORTED;
    return SS$_NORMAL;
}

/*
 * sys$create_uid - create a universal identifier (a 128-bit unique value).
 *
 * Observed on the lab OpenVMS Alpha V8.4 node (a MACRO-32 program calling the
 * service twice and printing the four longwords): an OSF DCE uuid, version 1 --
 *   time_low[4] time_mid[2] time_hi_and_version[2]  the 100 ns count since
 *       15-OCT-1582 (the VMS system time + 100840 days), version nibble 1;
 *   clock_seq_hi_and_reserved[1] clock_seq_low[1]   variant 10xx, 14-bit sequence,
 *       constant between the two calls;
 *   node[6]  the SCSNODE name ("ALPHA1" there), not an IEEE address;
 * and the timestamp advances by exactly 1 between back-to-back calls. This does
 * the same: SCSNODE (blank-padded to 6) for the node, the clock for the time,
 * bumped by one tick if it has not advanced.
 */
#include <pthread.h>

#define UID_VMS_TO_UUID_EPOCH 0x0135886AC7960000ull  /* 100ns: 15-OCT-1582 -> 17-NOV-1858 = 100840 days */

uint32_t (sys$create_uid)(void *uid)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_time;
    static uint16_t clock_seq;
    static uint8_t  node[6];
    static int      seeded;
    static int      node_from_scsnode; (void)node_from_scsnode;
    uint64_t now;
    uint8_t out[16];

    if (!uid)
        return SS$_ACCVIO;

    if (!(sys$gettim(&now) & 1))
        return SS$_BADPARAM;

    pthread_mutex_lock(&lock);
    if (!seeded) {
        uint8_t r[8];
        if (!(sys$get_entropy(r, sizeof(r)) & 1)) {
            pthread_mutex_unlock(&lock);
            return SS$_INSFMEM;      /* no entropy: refuse rather than mint a guessable uid */
        }
        clock_seq = (uint16_t)(((r[0] << 8) | r[1]) & 0x3FFF);
        memcpy(node, r + 2, 6);
        node[0] |= 0x01;             /* fallback: multicast bit, not an IEEE address */
        {
            /* The node field is the SCSNODE name, blank-padded to six characters. */
            char scs[16];
            uint16_t sl = 0;
            struct item_list_3 il[2];
            memset(scs, 0, sizeof scs);
            il[0].buflen = 6; il[0].item_code = SYI$_SCSNODE;
            il[0].bufaddr = scs; il[0].retlen = &sl;
            il[1].buflen = 0; il[1].item_code = 0; il[1].bufaddr = NULL; il[1].retlen = NULL;
            if ((sys$getsyiw(0, NULL, NULL, il, NULL, NULL, 0) & 1) && sl > 0) {
                memset(node, ' ', sizeof node);
                memcpy(node, scs, sl < 6 ? sl : 6);
                node_from_scsnode = 1;
            }
        }
        seeded = 1;
    }
    uint64_t t = now + UID_VMS_TO_UUID_EPOCH;
    if (t <= last_time)
        t = last_time + 1;
    last_time = t;
    pthread_mutex_unlock(&lock);

    out[0] = (uint8_t)(t);          out[1] = (uint8_t)(t >> 8);
    out[2] = (uint8_t)(t >> 16);    out[3] = (uint8_t)(t >> 24);
    out[4] = (uint8_t)(t >> 32);    out[5] = (uint8_t)(t >> 40);
    out[6] = (uint8_t)(t >> 48);
    out[7] = (uint8_t)(((t >> 56) & 0x0F) | 0x10);          /* version 1 */
    out[8] = (uint8_t)(((clock_seq >> 8) & 0x3F) | 0x80);   /* RFC 4122 variant */
    out[9] = (uint8_t)clock_seq;
    memcpy(out + 10, node, 6);
    memcpy(uid, out, sizeof(out));
    return SS$_NORMAL;
}

/* ======================================================================
 * $PARSE_ACL / $FORMAT_ACL -- an access control entry's text form <-> its
 * binary form (vms-d404). Grounded on OpenVMS VAX V7.3, docs/oracle/
 * vax73-acl.md: ACE$B_SIZE, ACE$B_TYPE, ACE$W_FLAGS, ACE$L_ACCESS, then
 * identifiers (ACE$C_KEYID) or four S/O/G/W deny longwords (ACE$C_DIRDEF).
 * ====================================================================== */

#define ACE_KEYID         1u
#define ACE_DIRDEF        9u
#define ACE_UIC_WILD_G    0x3FFFu
#define ACE_UIC_WILD_M    0xFFFFu
#define ACE_ID_ANY        0xFFFFFFFFu   /* IDENTIFIER=* */
#define ACE_MAX_IDS       16

static const char *const ace_access_names[] = { "READ", "WRITE", "EXECUTE", "DELETE", "CONTROL" };
static const char *const ace_option_names[] = { "DEFAULT", "PROTECTED", "HIDDEN", "NOPROPAGATE" };

static void ace_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t ace_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ace_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

struct ace_lex { const char *s; size_t n, i; };

static void ace_ws(struct ace_lex *l)
{
    while (l->i < l->n && (l->s[l->i] == ' ' || l->s[l->i] == '\t'))
        l->i++;
}

static int ace_ch(struct ace_lex *l, char c)
{
    ace_ws(l);
    if (l->i < l->n && l->s[l->i] == c) { l->i++; return 1; }
    return 0;
}

/* A keyword token (letters, digits, $, _) upcased into buf. */
static size_t ace_word(struct ace_lex *l, char *buf, size_t cap)
{
    size_t k = 0;
    ace_ws(l);
    while (l->i < l->n && k + 1 < cap) {
        char c = l->s[l->i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '$' || c == '_'))
            break;
        buf[k++] = (char)ace_up(c);
        l->i++;
    }
    buf[k] = '\0';
    return k;
}

/* `w` is a (possibly abbreviated, at least one letter) form of `full`. */
static int ace_abbrev(const char *w, const char *full)
{
    size_t n = strlen(w);
    return n > 0 && n <= strlen(full) && strncmp(w, full, n) == 0;
}

static uint32_t ace_lookup_name(const char *name, uint32_t *id)
{
    struct dsc$descriptor_s d = { (uint16_t)strlen(name), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)name };
    uint32_t attr = 0;
    return sys$asctoid(&d, id, &attr);
}

/* Octal number or '*' inside a UIC; *wild set for '*'. */
static int ace_uic_part(struct ace_lex *l, uint32_t *v, int *wild)
{
    uint32_t x = 0;
    size_t d = 0;
    ace_ws(l);
    *wild = 0;
    if (l->i < l->n && l->s[l->i] == '*') { l->i++; *wild = 1; return 1; }
    while (l->i < l->n && l->s[l->i] >= '0' && l->s[l->i] <= '7') {
        x = x * 8u + (uint32_t)(l->s[l->i] - '0');
        l->i++; d++;
    }
    *v = x;
    return d > 0;
}

/* One identifier: `*`, `[g,m]` (octal or `*`), `[name]` (a UIC identifier) or a
 * name. Returns SS$_NORMAL, SS$_IVACL or the rights lookup's SS$_NOSUCHID. */
static uint32_t ace_parse_id(struct ace_lex *l, uint32_t *id)
{
    char w[64];
    ace_ws(l);
    if (ace_ch(l, '*')) { *id = ACE_ID_ANY; return SS$_NORMAL; }
    if (ace_ch(l, '[') || ace_ch(l, '<')) {
        size_t save = l->i;
        uint32_t g = 0, m = 0;
        int gw, mw;
        if (ace_uic_part(l, &g, &gw) && ace_ch(l, ',')) {
            if (!ace_uic_part(l, &m, &mw) || !(ace_ch(l, ']') || ace_ch(l, '>')))
                return SS$_IVACL;
            if ((!gw && g > 037777u) || (!mw && m > 0177777u))
                return SS$_IVACL;
            *id = ((gw ? ACE_UIC_WILD_G : g) << 16) | (mw ? ACE_UIC_WILD_M : m);
            return SS$_NORMAL;
        }
        l->i = save;
        if (!ace_word(l, w, sizeof w) || !(ace_ch(l, ']') || ace_ch(l, '>')))
            return SS$_IVACL;
        {
            uint32_t st = ace_lookup_name(w, id);
            if (!(st & 1))
                return st;
            return (*id & 0x80000000u) ? SS$_IVACL : SS$_NORMAL;   /* [name] names a UIC */
        }
    }
    if (!ace_word(l, w, sizeof w))
        return SS$_IVACL;
    return ace_lookup_name(w, id);
}

/* A '+'-joined keyword list into a bit mask (NONE = 0). */
static uint32_t ace_parse_bits(struct ace_lex *l, const char *const *names, unsigned nn,
                               uint32_t *mask)
{
    char w[32];
    *mask = 0;
    do {
        unsigned k;
        int hit = 0;
        if (!ace_word(l, w, sizeof w))
            return SS$_IVACL;
        if (ace_abbrev(w, "NONE")) { hit = 1; }
        for (k = 0; k < nn && !hit; k++)
            if (ace_abbrev(w, names[k])) { *mask |= 1u << k; hit = 1; }
        if (!hit)
            return SS$_IVACL;
    } while (ace_ch(l, '+'));
    return SS$_NORMAL;
}

/* "S:RWED" style category: the letters granted, as a deny longword (CONTROL is
 * always denied in a DEFAULT_PROTECTION ACE, as observed). */
static uint32_t ace_parse_prot(struct ace_lex *l, uint32_t *deny)
{
    uint32_t allow = 0;
    ace_ws(l);
    while (l->i < l->n) {
        int c = ace_up(l->s[l->i]);
        if (c == 'R') allow |= 1u; else if (c == 'W') allow |= 2u;
        else if (c == 'E') allow |= 4u; else if (c == 'D') allow |= 8u;
        else break;
        l->i++;
    }
    *deny = (~allow & 0xFu) | 0x10u;
    return SS$_NORMAL;
}

uint32_t sys$parse_acl(const struct dsc$descriptor_s *aclstr, struct dsc$descriptor_s *aclent,
                       uint16_t *errpos, void *accnam, uint32_t acmode)
{
    struct ace_lex l;
    uint8_t ace[8 + 4 * ACE_MAX_IDS];
    char w[64];
    uint32_t st, flags = 0, access = 0;
    size_t size, mark = 0;

    (void)accnam; (void)acmode;
    if (!aclstr || !aclstr->dsc$a_pointer || !aclent || !aclent->dsc$a_pointer)
        return SS$_BADPARAM;
    l.s = aclstr->dsc$a_pointer; l.n = aclstr->dsc$w_length; l.i = 0;
    memset(ace, 0, sizeof ace);

    st = SS$_IVACL;
    if (!ace_ch(&l, '('))
        goto fail;
    ace_ws(&l);
    mark = l.i;                                 /* errpos: start of the failing clause */
    if (!ace_word(&l, w, sizeof w))
        goto fail;
    if (ace_abbrev(w, "IDENTIFIER") && strlen(w) >= 2) {
        unsigned nid = 0;
        if (!ace_ch(&l, '='))
            goto fail;
        do {
            uint32_t id;
            if (nid >= ACE_MAX_IDS) { st = SS$_IVACL; goto fail; }
            ace_ws(&l);
            mark = l.i;
            st = ace_parse_id(&l, &id);
            if (!(st & 1))
                goto fail;
            ace_put32(ace + 8 + 4 * nid++, id);
        } while (ace_ch(&l, '+'));
        while (ace_ch(&l, ',')) {
            st = SS$_IVACL;
            ace_ws(&l);
            mark = l.i;
            if (!ace_word(&l, w, sizeof w) || !ace_ch(&l, '='))
                goto fail;
            if (ace_abbrev(w, "OPTIONS"))
                st = ace_parse_bits(&l, ace_option_names, 4, &flags);
            else if (ace_abbrev(w, "ACCESS"))
                st = ace_parse_bits(&l, ace_access_names, 5, &access);
            if (!(st & 1))
                goto fail;
            if (w[0] == 'O')
                flags <<= 8;                    /* ACE$V_DEFAULT is bit 8 */
        }
        size = 8u + 4u * nid;
        ace[1] = ACE_KEYID;
    } else if (ace_abbrev(w, "DEFAULT_PROTECTION") && strlen(w) >= 3) {
        uint32_t deny[4] = { 0x1Fu, 0x1Fu, 0x1Fu, 0x1Fu };
        while (ace_ch(&l, ',')) {
            int cat = -1;
            st = SS$_IVACL;
            ace_ws(&l);
            mark = l.i;
            if (!ace_word(&l, w, sizeof w))
                goto fail;
            if (ace_abbrev(w, "OPTIONS") && strlen(w) >= 2) {
                if (!ace_ch(&l, '=') || !((st = ace_parse_bits(&l, ace_option_names, 4, &flags)) & 1))
                    goto fail;
                flags <<= 8;
                continue;
            }
            if (ace_abbrev(w, "SYSTEM")) cat = 0;
            else if (ace_abbrev(w, "OWNER")) cat = 1;
            else if (ace_abbrev(w, "GROUP")) cat = 2;
            else if (ace_abbrev(w, "WORLD")) cat = 3;
            if (cat < 0 || !(ace_ch(&l, ':') || ace_ch(&l, '=')))
                goto fail;
            (void)ace_parse_prot(&l, &deny[cat]);
        }
        size = 24u;
        ace[1] = ACE_DIRDEF;
        ace_put32(ace + 8, deny[0]); ace_put32(ace + 12, deny[1]);
        ace_put32(ace + 16, deny[2]); ace_put32(ace + 20, deny[3]);
    } else {
        goto fail;
    }
    st = SS$_IVACL;
    ace_ws(&l);
    mark = l.i;
    if (!ace_ch(&l, ')'))
        goto fail;
    ace_ws(&l);
    if (l.i != l.n)
        goto fail;
    ace[0] = (uint8_t)size;
    ace[2] = (uint8_t)flags; ace[3] = (uint8_t)(flags >> 8);
    if (ace[1] == ACE_KEYID)
        ace_put32(ace + 4, access);
    if (aclent->dsc$w_length < size)
        return SS$_BUFFEROVF;
    memcpy(aclent->dsc$a_pointer, ace, size);
    if (errpos)
        *errpos = (uint16_t)l.i;
    return SS$_NORMAL;
fail:
    if (errpos)
        *errpos = (uint16_t)mark;
    return st;
}

/* Text of one identifier, as SHOW ACL prints it. */
static void ace_format_id(uint32_t id, char *out, size_t cap)
{
    char name[40];
    uint16_t nl = 0;
    struct dsc$descriptor_s nd = { sizeof(name) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, name };
    uint32_t resid = 0, attr = 0;

    if (id == ACE_ID_ANY) { snprintf(out, cap, "*"); return; }
    if (!(id & 0x80000000u)) {
        uint32_t g = (id >> 16) & 0x3FFFu, m = id & 0xFFFFu;
        if (g == ACE_UIC_WILD_G && m == ACE_UIC_WILD_M) { snprintf(out, cap, "[*,*]"); return; }
        if (g == ACE_UIC_WILD_G) { snprintf(out, cap, "[*,%o]", m); return; }
        if (m == ACE_UIC_WILD_M) { snprintf(out, cap, "[%o,*]", g); return; }
    }
    if ((sys$idtoasc(id, &nl, &nd, &resid, &attr, NULL) & 1) && nl > 0) {
        name[nl < sizeof(name) ? nl : sizeof(name) - 1] = '\0';
        if (id & 0x80000000u)
            snprintf(out, cap, "%s", name);
        else
            snprintf(out, cap, "[%s]", name);
        return;
    }
    if (id & 0x80000000u)
        snprintf(out, cap, "%%X%08X", id);
    else
        snprintf(out, cap, "[%o,%o]", (id >> 16) & 0x3FFFu, id & 0xFFFFu);
}

static void ace_cat(char *buf, size_t cap, const char *s)
{
    size_t n = strlen(buf);
    if (n < cap - 1)
        snprintf(buf + n, cap - n, "%s", s);
}

static void ace_format_bits(char *buf, size_t cap, uint32_t mask, const char *const *names,
                            unsigned nn)
{
    unsigned k;
    int first = 1;
    for (k = 0; k < nn; k++)
        if (mask & (1u << k)) {
            if (!first) ace_cat(buf, cap, "+");
            ace_cat(buf, cap, names[k]);
            first = 0;
        }
    if (first)
        ace_cat(buf, cap, "NONE");
}

static void ace_format_prot(char *buf, size_t cap, const char *cat, uint32_t deny)
{
    ace_cat(buf, cap, cat);
    ace_cat(buf, cap, ":");
    if (!(deny & 1u)) ace_cat(buf, cap, "R");
    if (!(deny & 2u)) ace_cat(buf, cap, "W");
    if (!(deny & 4u)) ace_cat(buf, cap, "E");
    if (!(deny & 8u)) ace_cat(buf, cap, "D");
}

uint32_t sys$format_acl(const struct dsc$descriptor_s *aclent, uint16_t *acllen,
                        struct dsc$descriptor_s *aclstr, uint16_t *width,
                        struct dsc$descriptor_s *trmdsc, uint16_t *indent, void *accnam,
                        void *nullarg)
{
    const uint8_t *a;
    char buf[512];
    unsigned size, flags;
    size_t n;

    (void)width; (void)trmdsc; (void)indent; (void)accnam; (void)nullarg;
    if (!aclent || !aclent->dsc$a_pointer || !aclstr || !aclstr->dsc$a_pointer)
        return SS$_BADPARAM;
    a = (const uint8_t *)aclent->dsc$a_pointer;
    size = a[0];
    if (size < 8u || size > aclent->dsc$w_length)
        return SS$_IVACL;
    flags = (unsigned)(a[2] | (a[3] << 8));
    buf[0] = '\0';
    if (a[1] == ACE_KEYID) {
        unsigned k, nid = (size - 8u) / 4u;
        if (nid == 0 || (size - 8u) % 4u)
            return SS$_IVACL;
        ace_cat(buf, sizeof buf, "(IDENTIFIER=");
        for (k = 0; k < nid; k++) {
            char idt[64];
            ace_format_id(ace_get32(a + 8 + 4 * k), idt, sizeof idt);
            if (k) ace_cat(buf, sizeof buf, "+");
            ace_cat(buf, sizeof buf, idt);
        }
        if (flags & 0x0F00u) {
            ace_cat(buf, sizeof buf, ",OPTIONS=");
            ace_format_bits(buf, sizeof buf, flags >> 8, ace_option_names, 4);
        }
        ace_cat(buf, sizeof buf, ",ACCESS=");
        ace_format_bits(buf, sizeof buf, ace_get32(a + 4), ace_access_names, 5);
        ace_cat(buf, sizeof buf, ")");
    } else if (a[1] == ACE_DIRDEF && size >= 24u) {
        ace_cat(buf, sizeof buf, "(DEFAULT_PROTECTION");
        if (flags & 0x0F00u) {
            ace_cat(buf, sizeof buf, ",OPTIONS=");
            ace_format_bits(buf, sizeof buf, flags >> 8, ace_option_names, 4);
        }
        ace_format_prot(buf, sizeof buf, ",SYSTEM", ace_get32(a + 8));
        ace_format_prot(buf, sizeof buf, ",OWNER", ace_get32(a + 12));
        ace_format_prot(buf, sizeof buf, ",GROUP", ace_get32(a + 16));
        ace_format_prot(buf, sizeof buf, ",WORLD", ace_get32(a + 20));
        ace_cat(buf, sizeof buf, ")");
    } else {
        return SS$_IVACL;                       /* an ACE type this does not render */
    }
    n = strlen(buf);
    if (n > aclstr->dsc$w_length) {
        memcpy(aclstr->dsc$a_pointer, buf, aclstr->dsc$w_length);
        if (acllen) *acllen = aclstr->dsc$w_length;
        return SS$_BUFFEROVF;
    }
    memcpy(aclstr->dsc$a_pointer, buf, n);
    if (acllen) *acllen = (uint16_t)n;
    return SS$_NORMAL;
}

/* ======================================================================
 * $GRANTID / $REVOKID -- the process rights list (vms-7d5a). The list is
 * executive state (vms_kif_rights, VMS_IOCTL_RIGHTS); the Files-11 ACP's ACL
 * check matches an identifier ACE against it, so granting an identifier
 * changes what the process may open.
 *
 *   sys$grantid(pidadr, prcnam, id, name, prvatr, segment)
 *     id      address of a quadword {identifier, attributes}, or
 *     name    the identifier's name (looked up with $ASCTOID) when id is 0
 *     prvatr  receives the attributes the identifier had (0 if newly granted)
 *   SS$_WASCLR newly granted / SS$_WASSET already held (the OpenVMS V7.3 /
 *   Alpha V8.4 probe, docs/oracle/semantics/rights); $REVOKID the reverse.
 *   SS$_NOPRIV without CMKRNL, SS$_NOSUCHID for an unknown name.
 * ====================================================================== */
static uint32_t rights_op(uint32_t op, const uint32_t *pidadr,
                          const struct dsc$descriptor_s *prcnam, const uint32_t *id,
                          const struct dsc$descriptor_s *name, uint32_t *prvatr)
{
    uint32_t value, attrib = 0, st, pid = 0;

    if (prcnam && prcnam->dsc$w_length)
        return SS$_UNSUPPORTED;                 /* by process name: not this service yet */
    if (pidadr)
        pid = *pidadr;
    if (id) {
        value = id[0];
        attrib = id[1];
    } else if (name && name->dsc$a_pointer && name->dsc$w_length) {
        uint32_t a = 0;
        st = sys$asctoid(name, &value, &a);
        if (!(st & 1))
            return st;
    } else {
        return SS$_BADPARAM;
    }
    st = vms_kif_rights(op, pid, value, &attrib);
    if ((st & 1) && prvatr)
        *prvatr = attrib;
    return st;
}

uint32_t sys$grantid(const uint32_t *pidadr, const struct dsc$descriptor_s *prcnam,
                     const uint32_t *id, const struct dsc$descriptor_s *name,
                     uint32_t *prvatr, uint32_t segment)
{
    (void)segment;
    return rights_op(VMS_RIGHTS_OP_GRANT, pidadr, prcnam, id, name, prvatr);
}

uint32_t sys$revokid(const uint32_t *pidadr, const struct dsc$descriptor_s *prcnam,
                     const uint32_t *id, const struct dsc$descriptor_s *name,
                     uint32_t *prvatr, uint32_t segment)
{
    (void)segment;
    return rights_op(VMS_RIGHTS_OP_REVOKE, pidadr, prcnam, id, name, prvatr);
}
