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
 * OVMX-USERSPACE: sys$chkpro (vms-f15) -- decides in this process, from the
 *     caller's own getuid()/getgid() and the protection word the caller
 *     itself passed in. There is no executive reference monitor, no rights
 *     list and no ACL evaluation, so the checker and the checked are the same
 *     process and the answer binds nothing.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include "starlet.h"
#include "ovmx_secparam.h"
#include "ovmx_fileprot.h"

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
