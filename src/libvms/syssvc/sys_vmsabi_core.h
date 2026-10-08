/*
 * sys_vmsabi_core.h - the internal half of the VMS-ABI system services
 * (vms-38b). sys_vmsabi.c sees only the VMS argument forms (32-bit
 * descriptors and item lists, src/libvms/include/vms/); this interface, in
 * plain native types, is how it reaches the services OVMX implements with its
 * own structures (sys_vmsabi_core.c). The two never meet in one TU.
 */
#ifndef SYS_VMSABI_CORE_H
#define SYS_VMSABI_CORE_H

#include <stdint.h>

/* One item-list entry with native addresses. */
struct ovmx_abi_item {
    uint16_t  len;
    uint16_t  code;
    void     *buf;
    uint16_t *retlen;
};

uint32_t ovmx_vmsabi_assign(const char *dev, unsigned devlen, uint16_t *chan,
                            uint32_t acmode, const char *mbx, unsigned mbxlen,
                            uint32_t flags);
uint32_t ovmx_vmsabi_dassgn(uint16_t chan);

/* $TRNLNM (create == 0) or $CRELNM (create == 1); items[n] terminated by the
 * caller's count. */
uint32_t ovmx_vmsabi_lnm(int create, const uint32_t *attr,
                         const char *tab, unsigned tablen,
                         const char *log, unsigned loglen,
                         const uint8_t *acmode,
                         const struct ovmx_abi_item *items, unsigned n);

#endif /* SYS_VMSABI_CORE_H */
