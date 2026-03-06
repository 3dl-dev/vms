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

    if (my_uic == 0) {
        /* UID 0 (root) is treated as SYSTEM */
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
 * vms$check_access - Simplified access check (convenience wrapper).
 *
 * Compares caller_uic against owner_uic and the protection mask to
 * determine if the requested access_type is allowed.
 *
 * Returns 1 if access is granted, 0 if denied.
 */
int vms$check_access(uint32_t caller_uic, uint32_t owner_uic,
                     uint32_t protection, int access_type) {
    uint16_t prot = (uint16_t)protection;
    uint16_t access = (uint16_t)access_type;

    /* Determine the relevant category based on caller vs owner UIC */
    uint16_t category_mask;

    if (caller_uic == 0) {
        /* UID 0 (root) is treated as SYSTEM */
        category_mask = (uint16_t)((prot >> PROT$V_SYSTEM) & 0x0F);
    } else if (caller_uic == owner_uic) {
        /* Owner access */
        category_mask = (uint16_t)((prot >> PROT$V_OWNER) & 0x0F);
    } else if ((caller_uic >> 16) == (owner_uic >> 16)) {
        /* Same group */
        category_mask = (uint16_t)((prot >> PROT$V_GROUP) & 0x0F);
    } else {
        /* World */
        category_mask = (uint16_t)((prot >> PROT$V_WORLD) & 0x0F);
    }

    /* In VMS, a SET bit means access is DENIED */
    if (category_mask & access) {
        return 0;
    }

    return 1;
}
