/*
 * rights_vmsabi.c - the rights-database services that take a string, by their
 * upper-case (VMS-ABI) names (vms-8b5): SYS$ADD_IDENT.
 *
 * An image LINKed on real OpenVMS Alpha reaches the rights database through
 * SECURESHRP.EXE (src/vmslink/vms_vectors/SECURESHRP.vec). The services whose
 * arguments are values or addresses of longwords and quadwords ($ADD_HOLDER,
 * $REM_HOLDER, $REM_IDENT, $FIND_HOLDER, $FIND_HELD, $FINISH_RDB) have the same
 * argument layout for a 32-bit-pointer VMS caller, so the vector names OVMX's
 * own lower-case service (rightslist_live.c). $ADD_IDENT takes the name as a
 * VMS descriptor: the 8-byte 32-bit form (vms/descrip.h) or the 64-bit form,
 * recognised as VMS services recognise it by its MBO word (1) and MBMO
 * longword (-1). This entry point reads either and calls sys$add_ident with
 * OVMX's own descriptor.
 *
 * Built only for the Alpha: the 32-bit address field needs the alpha-dec-vms
 * compiler's #pragma __required_pointer_size.
 */
#include <stdint.h>

#include "descrip.h"
#include "starlet.h"       /* sys$add_ident (rightslist_live.c) */

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short
struct vms_dsc32 {
    unsigned short len;
    unsigned char  dtype, cls;
    char          *ptr;
};
#pragma __required_pointer_size __restore

struct vms_dsc64 {
    unsigned short     mbo;
    unsigned char      dtype, cls;
    int                mbmo;
    unsigned long long len;
    unsigned long long ptr;
};

uint32_t SYS$ADD_IDENT(const void *name, uint32_t id, uint32_t attrib, uint32_t *resid)
{
    struct dsc$descriptor_s d = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_S, 0 };
    if (name) {
        const struct vms_dsc64 *d64 = name;
        if (d64->mbo == 1 && d64->mbmo == -1) {
            d.dsc$w_length  = (uint16_t)d64->len;
            d.dsc$a_pointer = (char *)(uintptr_t)d64->ptr;
        } else {
            const struct vms_dsc32 *d32 = name;
            d.dsc$w_length  = d32->len;
            d.dsc$a_pointer = d32->ptr;
        }
    }
    return sys$add_ident(name ? &d : 0, id, attrib, resid);
}
