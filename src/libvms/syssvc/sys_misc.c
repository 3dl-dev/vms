/*
 * sys_misc.c - Miscellaneous System Services
 *
 * Implements $SETPRV (privilege management), $GETSYI (system information),
 * and related services. Privilege state is stored in the Per-Process
 * Control Block (PCB).
 *
 * SYI$_ and JPI$_ item code constants are defined in prcdef.h.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <time.h>
#include "starlet.h"
#include "vms/pcb.h"

/*
 * sys$setprv - Set or clear process privileges.
 *
 * On real VMS this controls what operations a process can perform.
 * Privilege state is stored in the PCB and shared across threads.
 *
 * Parameters:
 *   enbflg - 1 to enable privileges, 0 to disable
 *   prvadr - Pointer to 64-bit privilege mask (bits to change)
 *   prmflg - 1 to change permanent privileges, 0 for current only
 *   prvprv - Receives previous privilege mask (or NULL)
 */
uint32_t sys$setprv(uint32_t enbflg, const uint64_t *prvadr,
                    uint32_t prmflg, uint64_t *prvprv) {
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_BADPARAM;

    pthread_mutex_lock(&pcb->priv_lock);

    /* Return previous privileges */
    if (prvprv) *prvprv = pcb->cur_privs;

    if (prvadr) {
        if (enbflg) {
            /* Enable specified privileges */
            pcb->cur_privs |= *prvadr;
            if (prmflg) {
                pcb->perm_privs |= *prvadr;
            }
        } else {
            /* Disable specified privileges */
            pcb->cur_privs &= ~(*prvadr);
            if (prmflg) {
                pcb->perm_privs &= ~(*prvadr);
            }
        }
    }

    pthread_mutex_unlock(&pcb->priv_lock);
    return SS$_NORMAL;
}

/*
 * sys$getsyi - Get system information.
 *
 * Returns information about the system via an item list. Supported items:
 *   SYI$_NODENAME      - System node name (hostname)
 *   SYI$_HW_NAME       - Hardware description ("OVMX Virtual System")
 *   SYI$_VERSION        - System version string
 *   SYI$_AVAILCPU_CNT  - Number of available CPUs
 *   SYI$_MEMSIZE       - Physical memory size in pages
 */
uint32_t sys$getsyi(uint32_t efn, const uint32_t *csidadr,
                    const struct dsc$descriptor_s *nodename,
                    const struct item_list_3 *itmlst,
                    void *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm) {
    (void)efn; (void)csidadr; (void)nodename; (void)iosb;
    (void)astadr; (void)astprm;

    if (!itmlst) return SS$_BADPARAM;

    struct utsname uts;
    uname(&uts);

    for (const struct item_list_3 *item = itmlst;
         item->buflen != 0 || item->item_code != 0; item++) {
        switch (item->item_code) {
            case SYI$_NODENAME: {
                uint16_t len = (uint16_t)strlen(uts.nodename);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, uts.nodename, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case SYI$_VERSION: {
                const char *ver = "V0.1";
                uint16_t len = (uint16_t)strlen(ver);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, ver, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case SYI$_HW_NAME: {
                const char *hw = "OVMX Virtual System";
                uint16_t len = (uint16_t)strlen(hw);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, hw, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case SYI$_AVAILCPU_CNT:
            case SYI$_ACTIVECPU_CNT: {
                long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
                if (ncpus < 1) ncpus = 1;
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = (uint32_t)ncpus;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;
            }

            case SYI$_MEMSIZE: {
                long pages = sysconf(_SC_PHYS_PAGES);
                long psize = sysconf(_SC_PAGE_SIZE);
                /* Convert to VMS 512-byte pages (cast to uint64_t to avoid overflow) */
                uint32_t vms_pages = (uint32_t)((uint64_t)pages * ((uint64_t)psize / 512));
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = vms_pages;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;
            }

            default:
                break;
        }
    }

    return SS$_NORMAL;
}

/*
 * sys$getsyiw - Get system information (synchronous wrapper).
 */
uint32_t sys$getsyiw(uint32_t efn, const uint32_t *csidadr,
                     const struct dsc$descriptor_s *nodename,
                     const struct item_list_3 *itmlst,
                     void *iosb,
                     void (*astadr)(uint32_t), uint32_t astprm) {
    return sys$getsyi(efn, csidadr, nodename, itmlst, iosb, astadr, astprm);
}
