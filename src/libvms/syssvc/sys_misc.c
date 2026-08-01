/*
 * sys_misc.c - Miscellaneous System Services
 *
 * Implements $SETPRV (privilege management), $GETSYI (system information),
 * and related services. Privilege state is stored in the Per-Process
 * Control Block (PCB).
 *
 * SYI$_ and JPI$_ item code constants are defined in prcdef.h.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$setprv (vms-pv1) -- sets pcb->cur_privs/perm_privs in
 *     the per-process PCB and returns SS$_NORMAL. Nothing validates the grant
 *     and nothing outside the process reads pcb->cur_privs, so a process can
 *     award itself any privilege and the grant binds nothing but its own
 *     later in-process checks (sys$setuai's SYSPRV test is one of them). The
 *     executive's vms_kif_setprv is declared OVMX-UNWIRED against this item.
 * OVMX-USERSPACE: sys$getsyi (vms-5b4) -- answers from uname() and host
 *     sysconf() values, not from an executive system block. csidadr and
 *     nodename are both discarded ((void)csidadr; (void)nodename;), so a
 *     request aimed at another cluster node is answered with this machine's
 *     numbers as though it had been aimed here.
 * OVMX-USERSPACE: sys$getsyiw (vms-5b4) -- the wait form of the same answer.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <time.h>
#include "starlet.h"
#include "vms/pcb.h"
#include "sysgen_params.h"
#include "ovmx_identity.h"

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
 *   SYI$_NODENAME      - System node name (Linux hostname)
 *   SYI$_HW_NAME       - Hardware description ("OVMX Virtual System")
 *   SYI$_VERSION        - System version string
 *   SYI$_AVAILCPU_CNT  - Number of available CPUs
 *   SYI$_MEMSIZE       - Physical memory size in pages
 *   SYI$_SCSNODE       - Configured cluster node name (SYSGEN SCSNODE;
 *                        distinct from SYI$_NODENAME, see vms-ci.8)
 *   SYI$_SCSSYSTEMID   - Configured cluster system ID (SYSGEN SCSSYSTEMID)
 *   SYI$_CLUSTER_MEMBER - Whether VAXCLUSTER participation is enabled
 *   SYI$_CLUSTER_NODES - Cluster node count (1 for standalone; no live
 *                        cluster wire yet, see vms-ci.3)
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
                /* Machine surface: the true-to-arch VMS-compat token from
                 * the identity SSOT (INV-1). This used to answer "V0.1"
                 * while F$GETSYI answered "V7.3" -- two different answers
                 * to the same question, which is the tell INV-1 kills. */
                const char *ver = ovmx_compat_version();
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

            case SYI$_SCSNODE: {
                /* Configured cluster node identity (SYSGEN SCSNODE) —
                 * distinct from SYI$_NODENAME (Linux hostname). Falls
                 * back to the OVMX default when SYSGEN is unconfigured. */
                char node[SYSGEN_STRVAL_LEN];
                if (sysgen_read_string("SCSNODE", node, sizeof(node)) != 0) {
                    strncpy(node, "OVMX", sizeof(node) - 1);
                    node[sizeof(node) - 1] = '\0';
                }
                uint16_t len = (uint16_t)strlen(node);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, node, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case SYI$_SCSSYSTEMID: {
                uint32_t sysid = 0;   /* OVMX default when unconfigured */
                (void)sysgen_read_param("SCSSYSTEMID", &sysid);
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = sysid;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;
            }

            case SYI$_CLUSTER_MEMBER: {
                uint32_t vaxcluster = 0;   /* OVMX default: not cluster-enabled */
                (void)sysgen_read_param("VAXCLUSTER", &vaxcluster);
                uint32_t member = (vaxcluster != 0) ? 1 : 0;
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = member;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;
            }

            case SYI$_CLUSTER_NODES: {
                /* OVMX has no live cluster wire yet (vms-ci.3) — report
                 * this node only. */
                uint32_t nodes = 1;
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = nodes;
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
