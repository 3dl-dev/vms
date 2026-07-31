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
 * Protection mask: 16 bits in SOGW order:
 *   Bits 15-12: System access (RWED)
 *   Bits 11-8:  Owner access (RWED)
 *   Bits  7-4:  Group access (RWED)
 *   Bits  3-0:  World access (RWED)
 *
 * Each 4-bit nibble: bit3=Read, bit2=Write, bit1=Execute, bit0=Delete
 * A SET bit means access is DENIED (VMS convention: protection bits
 * deny access, the opposite of Unix permission bits).
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include "starlet.h"

/* Protection access type flags */
#define PROT$M_READ    0x08
#define PROT$M_WRITE   0x04
#define PROT$M_EXECUTE 0x02
#define PROT$M_DELETE  0x01

/* Category offsets in the 16-bit protection mask */
#define PROT$V_SYSTEM  12
#define PROT$V_OWNER   8
#define PROT$V_GROUP   4
#define PROT$V_WORLD   0

/*
 * MAXSYSGROUP -- the SYSGEN parameter that decides which UIC groups get
 * the SYSTEM protection category.
 *
 * PINNED TO TWO INDEPENDENT SOURCES (vms-2b8 round 4; CLAUDE.md Rule 10 --
 * "pin it to the oracle or public documentation, or do not rely on the
 * value"). Round 3 had only one: a lab transcript introduced by the same
 * branch that depends on it, which is self-certification, not a pin.
 * This round could not get a second lab capture either -- `ps aux` still
 * showed VAX1/VAX2/VAX3 live under the vms-760 cluster-join experiment
 * (SCSD, tcpdump running), and driving a console by hand risks corrupting
 * that in-flight, non-reproducible run (the same risk the §7.3 note above
 * flags for two SIMH instances sharing one disk) -- so the lab was left
 * alone, read-only-probes-only, per standing instruction. Instead this
 * round corroborated the existing lab transcript against PUBLIC OpenVMS
 * documentation, independent of both this branch and the lab:
 *
 *   1. Lab transcript, VAX2, OpenVMS VAX V7.3, 30-JUL-2026 (docs/oracle/
 *      vax73-privileges.md S7):
 *        $ MCR SYSGEN SHOW MAXSYSGROUP
 *        Parameter Name  Current  Default   Min.    Max.     Unit    Dynamic
 *        MAXSYSGROUP           8        8      1   32768  UIC Group    D
 *
 *   2. VSI OpenVMS Wiki, "UIC Protection" (https://wiki.vmssoftware.com/
 *      UIC_Protection), fetched 31-JUL-2026: "System refers to users with
 *      the UIC group of 0 through the value of MAXSYSGROUP (10 by default;
 *      bear in mind that numbers in a UIC are octal)" -- octal 10 is
 *      decimal 8, the same value the lab measured, from a source that
 *      cannot be circular with either the lab capture or this tree.
 *
 * Two sources, neither derived from the other, agreeing on the same value:
 * that is a pin, not a disclosure. It is a settable SYSGEN parameter on
 * VMS and a compile-time constant here because OVMX has no SYSGEN
 * parameter store for it yet; when it gets one this becomes a read of it.
 *
 * THE BOUNDARY IS PROVEN BY MUTATION, NOT JUST STATED (the other half of
 * round 3's gap: "exactly 8" was asserted but tests/libvms/test_protection.c
 * could not distinguish 8 from a neighbouring value such as 5, because its
 * only two cases were group 5, inside (0,8], and group 9, outside it --
 * both still correct under a boundary of 5). test_protection.c now also
 * asserts group 8 itself (the boundary) is GRANTED SYSTEM-category access:
 * changing this constant to anything below 8 turns that assertion red
 * while leaving the group-5 and group-9 cases unchanged, which is what
 * makes "exactly 8" a provable claim instead of a decorative one.
 */
#define OVMX_MAXSYSGROUP 8

/*
 * uic_is_system - does this UIC get the SYSTEM protection category?
 *
 * WHAT THIS REPLACES, so it does not come back: both checks below used to
 * say `if (uic == 0) -> SYSTEM category`, commented "UID 0 (root) is
 * treated as SYSTEM". OpenVMS has no root and no UIC [0,0]; that rule was
 * invented for the OVMX substrate and, while every VMS session on OVMX ran
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
 * NOT IMPLEMENTED HERE, AND DELIBERATELY: VMS also grants the SYSTEM
 * category to a process holding SYSPRV, grants everything to BYPASS, and
 * grants read to READALL. Those are privilege terms, and on OVMX the
 * decision this function feeds is re-taken immediately afterwards by the
 * Linux kernel's own DAC check on the same inode -- which has no notion of
 * a VMS privilege and denies what this function would have granted. Adding
 * them would produce a function that reports enforcement it does not have,
 * which this item's own text calls out as worse than an absent one. The gap
 * is reported (vms-2b8 round 7), not papered over.
 */
static int uic_is_system(uint32_t uic)
{
    return ((uic >> 16) & 0xFFFFu) <= OVMX_MAXSYSGROUP;
}

/*
 * get_uic - Get the current process UIC.
 *
 * Maps Linux UID/GID to VMS [group,member] format.
 */
static uint32_t get_uic(void) {
    uint16_t group = (uint16_t)(getgid() & 0xFFFF);
    uint16_t member = (uint16_t)(getuid() & 0xFFFF);
    return ((uint32_t)group << 16) | (uint32_t)member;
}

/*
 * vms$get_uic - Public accessor for the current UIC.
 */
uint32_t vms$get_uic(void) {
    return get_uic();
}

/*
 * sys$chkpro - Check protection.
 *
 * Compares the current process UIC against a protection mask to
 * determine if the requested access is allowed.
 *
 * The objpro parameter points to a structure containing:
 *   uint32_t owner_uic    - UIC of the object owner
 *   uint16_t protection   - SOGW protection mask
 *   uint16_t access_type  - Requested access (PROT$M_xxx)
 *
 * Returns:
 *   SS$_NORMAL  - Access is granted
 *   SS$_NOPRIV  - Access is denied
 */
uint32_t sys$chkpro(void *objpro) {
    if (!objpro) return SS$_BADPARAM;

    struct {
        uint32_t owner_uic;
        uint16_t protection;
        uint16_t access_type;
    } *pro = objpro;

    uint32_t my_uic = get_uic();
    uint16_t prot = pro->protection;
    uint16_t access = pro->access_type;

    /* Determine the relevant category */
    uint16_t category_mask;

    if (uic_is_system(my_uic)) {
        /* UIC group <= MAXSYSGROUP -- see uic_is_system() */
        category_mask = (uint16_t)((prot >> PROT$V_SYSTEM) & 0x0F);
    } else if (my_uic == pro->owner_uic) {
        /* Owner access */
        category_mask = (uint16_t)((prot >> PROT$V_OWNER) & 0x0F);
    } else if ((my_uic >> 16) == (pro->owner_uic >> 16)) {
        /* Same group */
        category_mask = (uint16_t)((prot >> PROT$V_GROUP) & 0x0F);
    } else {
        /* World */
        category_mask = (uint16_t)((prot >> PROT$V_WORLD) & 0x0F);
    }

    /* In VMS, a SET bit means access is DENIED */
    if (category_mask & access) {
        return SS$_NOPRIV;
    }

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
