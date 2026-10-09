/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_prot.h - the executive's ONE object-protection decision (rd vms-c6d1).
 *
 * VMS decides access to every protected object -- a file, a mailbox, a device --
 * the same way: the accessor's UIC and enabled privileges against the object's
 * OWNER UIC and its SOGW protection mask (OpenVMS Guide to System Security,
 * "UIC-Based Protection"; the $CHKPRO system service exposes the same decision).
 * Before this header the executive carried that decision once, privately, inside
 * the Files-11 ACP (vmsfs_acp.c acp_check_access), and nothing else could use it:
 * a mailbox had no protection at all, so any local process could $ASSIGN NETACP's
 * request mailbox and destructively read the access-control passwords in it.
 *
 * Every object class now calls vms_prot_check(): the ACP for a file header's
 * FH2$W_FILEPROT / FH2$L_FILEOWNER, the mailbox driver (vms_mbx.c) for the
 * promsk and owner UIC $CREMBX recorded. One decision, one set of privilege
 * overrides, one negative control per property (tests/qemu/facility_defects.sh
 * acp-bypass-ignored / acp-readall-ignored / acp-sysprv-ignored mutate THIS file,
 * so they redden the file AND the mailbox suites). It is the VMS model, not the
 * host's: no Linux uid/gid/mode test is consulted anywhere on this path -- the
 * UIC is the executive's (struct vms_proc::uic, set by SETIDENT/LOGINOUT) and the
 * privileges are the executive's enabled mask (cur_privs).
 *
 * THE RULE (public documentation, Rule 8 clean-room):
 *   - The mask is four 4-bit fields, System (bits 0-3), Owner (4-7), Group (8-11),
 *     World (12-15). Within a field a SET bit DENIES: bit 0 read, bit 1 write,
 *     bit 2 execute (files) / logical I/O (devices), bit 3 delete (files) /
 *     physical I/O (devices).
 *   - An accessor belongs to every category it qualifies for, and each wanted
 *     bit must be allowed by at least one of those categories. World always
 *     applies; Group when the UIC groups match; Owner when the UICs match; System
 *     when the accessor's UIC group is <= MAXSYSGROUP or SYSPRV is enabled.
 *   - GRPPRV puts the accessor in the SYSTEM category of objects its group owns.
 *   - BYPASS lifts the check entirely. READALL grants read (and, for a file, the
 *     execute/traversal right) but never write or delete.
 *
 * ACCESS CONTROL LISTS: the ACE walk is per object class (it reads that class's
 * own ACL store -- for a file, the FH2 ACL area), but what a matching ACE MEANS
 * is decided here, in vms_prot_check_acl(), so every object class that gains an
 * ACL store gets the same semantics.
 *
 * Header-only (static inline) so the one source compiles into vms.ko and into the
 * NetBSD `vms' pseudo-device unchanged, with no new translation unit to register.
 * Callers include vms_internal.h first (uint32_t/uint64_t, SS__*, VMS_PRV_M_*).
 */
#ifndef _VMS_PROT_H
#define _VMS_PROT_H

/* Access bits within one SOGW category (a SET bit in the mask DENIES it). */
#define VMS_PROT_ACC_READ     0x1u   /* R */
#define VMS_PROT_ACC_WRITE    0x2u   /* W */
#define VMS_PROT_ACC_EXECUTE  0x4u   /* E (files) / L, logical I/O (devices) */
#define VMS_PROT_ACC_DELETE   0x8u   /* D (files) / P, physical I/O (devices) */

/* What READALL may grant: read, and a file's execute (directory traversal). */
#define VMS_PROT_READALL_GRANTS (VMS_PROT_ACC_READ | VMS_PROT_ACC_EXECUTE)

/*
 * MAXSYSGROUP -- the SYSGEN parameter deciding which UIC groups get the SYSTEM
 * category; its documented default is 8 (measured on the VAX V7.3 lab, and the
 * VSI wiki's "10 octal" -- src/libvms/include/ovmx_secparam.h carries both pins).
 * OVMX maps root to UIC group 0, which `group <= MAXSYSGROUP` covers, exactly as
 * it covers a VMS [1,x] system process.
 */
#define VMS_PROT_MAXSYSGROUP  8u

/*
 * vms_prot_check_acl - grant `want` (VMS_PROT_ACC_* bits) on an object owned by
 * `owner_uic` under protection mask `prot` to an accessor with UIC `acc_uic` and
 * enabled privileges `privs`, given the object's ACL verdict: `ace_matched` is
 * set when the accessor matched an identifier ACE, whose ACE$L_ACCESS is
 * `ace_access` (the caller walks the object's own ACL store -- for a file, the
 * FH2 ACL area, vmsfs_acp.c acp_acl_match). VMS order (docs/oracle/vax73-acl.md):
 * the privilege overrides, then a matching ACE that grants everything wanted;
 * a matching ACE that does not grant leaves only the system and owner fields of
 * the protection code able to grant (group and world are closed by it).
 * SS__NORMAL if granted, SS__NOPRIV if refused -- never a silent allow.
 */
static inline uint32_t vms_prot_check_acl(uint32_t acc_uic, uint64_t privs,
                                          uint32_t owner_uic, uint16_t prot,
                                          unsigned want, int ace_matched,
                                          uint32_t ace_access)
{
    uint32_t acc_group = (acc_uic >> 16) & 0xFFFFu;
    uint32_t own_group = (owner_uic >> 16) & 0xFFFFu;
    unsigned denied;
    int is_system, is_owner, is_group;

    /* BYPASS lifts every access control. READALL grants the read bits. */
    if (privs & VMS_PRV_M_BYPASS)
        return SS__NORMAL;
    if ((privs & VMS_PRV_M_READALL) && !(want & ~VMS_PROT_READALL_GRANTS))
        return SS__NORMAL;

    is_owner  = (acc_uic == owner_uic);
    is_group  = (acc_group == own_group);
    /* GRPPRV: the system category for an object of the accessor's own group
     * (OpenVMS VAX V7.3, docs/oracle/vax73-acl.md "GRPPRV": [200,5] with GRPPRV
     * reads a S:RWED file owned by [200,1], not one owned by [300,1]; a denying
     * ACE leaves it the system field, as for any system-category accessor). */
    is_system = (acc_group <= VMS_PROT_MAXSYSGROUP) ||
                (privs & VMS_PRV_M_SYSPRV) != 0 ||
                ((privs & VMS_PRV_M_GRPPRV) != 0 && acc_group == own_group);

    if (ace_matched && (ace_access & want) == want)
        return SS__NORMAL;

    /* Each wanted bit must be left un-denied by SOME category the accessor is
     * in. Start denied; clear a bit as soon as a category allows it. Group and
     * world apply unless a matching ACE denied. */
    denied = want;
    if (is_system) denied &= ~(~(unsigned)(prot & 0xFu) & want);
    if (is_owner)  denied &= ~(~(unsigned)((prot >> 4) & 0xFu) & want);
    if (!ace_matched) {
        if (is_group)  denied &= ~(~(unsigned)((prot >> 8) & 0xFu) & want);
        /* World: */    denied &= ~(~(unsigned)((prot >> 12) & 0xFu) & want);
    }

    return denied ? SS__NOPRIV : SS__NORMAL;
}

/* vms_prot_check - the same decision for an object with no ACL (a mailbox: the
 * executive holds no ACL store for one yet). */
static inline uint32_t vms_prot_check(uint32_t acc_uic, uint64_t privs,
                                      uint32_t owner_uic, uint16_t prot,
                                      unsigned want)
{
    return vms_prot_check_acl(acc_uic, privs, owner_uic, prot, want, 0, 0u);
}

/*
 * vms_prot_require_priv - an operation reserved to holders of a PRIVILEGE, not
 * guarded by an object's owner and mask: grant it only if every bit of `needed`
 * (VMS_PRV_M_*) is among the accessor's ENABLED privileges `privs`. VMS gates
 * system-level operations this way -- connecting a device to its driver
 * (SYSGEN CONNECT) needs CMKRNL (OpenVMS System Management Utilities Reference,
 * SYSGEN CONNECT) -- and the terminal class driver uses it for binding a
 * substrate line to a terminal unit (rd vms-f8c). SS__NORMAL or SS__NOPRIV.
 */
static inline uint32_t vms_prot_require_priv(uint64_t privs, uint64_t needed)
{
    return ((privs & needed) == needed) ? SS__NORMAL : SS__NOPRIV;
}

#endif /* _VMS_PROT_H */
