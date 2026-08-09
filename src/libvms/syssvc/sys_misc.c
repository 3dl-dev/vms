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
 * OVMX-EXECUTIVE: sys$setprv (vms-pv1) proof=tests/qemu/test_syssvc_setprv.c -- the privilege mutation is the executive's: sys$setprv routes to vms_kif_setprv (VMS_IOCTL_SETPRV -> vms_ioctl_setprv, kernel/vms_access.c), which authorizes the grant against this process's AUTHORIZED mask (a caller without SETPRV cannot widen past it -- SS$_NOTALLPRIV/SS$_NOPRIV) and OWNS the result. A process can no longer award itself a privilege by writing pcb->cur_privs (the vms-b2e LARP class this closes). The PCB masks below are only a COPY of the executive's, re-read via $GETJPI-self for the two remaining in-process readers (sys_process.c fork inheritance, vmsprocess/access_modes.c's CMKRNL/CMEXEC mode gate) -- not part of the answer sys$setprv returns, which is wholly the executive's.
 * OVMX-USERSPACE: sys$getsyi (vms-642) -- answers from uname() and host
 *     sysconf() values, not from an executive system block. csidadr and
 *     nodename are both discarded ((void)csidadr; (void)nodename;), so a
 *     request aimed at another cluster node is answered with this machine's
 *     numbers as though it had been aimed here.
 * OVMX-USERSPACE: sys$getsyiw (vms-642) -- the wait form of the same answer.
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
#include "vms_kif.h"        /* the executive OWNS privilege state (vms-pv1) */

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

    uint64_t mask = prvadr ? *prvadr : 0;
    uint64_t prev = 0;

    /*
     * THE MUTATION IS THE EXECUTIVE'S (vms-pv1). Route it through /dev/vms:
     * vms_ioctl_setprv (kernel/vms_access.c) authorizes the request against
     * this process's AUTHORIZED (permanent) mask -- a caller without SETPRV
     * that reaches outside it gets the authorized subset and SS$_NOTALLPRIV,
     * and cannot widen the authorized mask at all (SS$_NOPRIV) -- and OWNS the
     * resulting state. This is what a process may NOT do for itself: writing
     * pcb->cur_privs directly was the vms-b2e per-process privilege LARP, a
     * self-assertion nothing authoritative read. If /dev/vms is unreachable the
     * kif layer returns an error status (INV-6, CLAUDE.md Rule 9); we never
     * fall back to a per-process fake grant.
     */
    uint32_t st = vms_kif_setprv(mask, enbflg ? 1 : 0, prmflg ? 1 : 0, &prev);

    /*
     * Mirror the executive's AUTHORITATIVE masks into the PCB, read back from
     * the executive itself ($GETJPI-self). This is a COPY of executive-owned
     * state, never an independent decision: the two remaining in-process
     * readers -- sys_process.c fork inheritance and vmsprocess/access_modes.c's
     * CMKRNL/CMEXEC mode gate -- then see exactly what the executive holds. If
     * the read-back cannot reach the executive we leave the cache alone rather
     * than invent a mask.
     */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    if (vms_kif_getjpi_self(&info) & 1) {
        pthread_mutex_lock(&pcb->priv_lock);
        pcb->cur_privs = info.cur_privs;
        pcb->perm_privs = info.perm_privs;
        pthread_mutex_unlock(&pcb->priv_lock);
    }

    /* The previous mask reported to the caller is the executive's, not a
     * pre-call snapshot of the local cache. */
    if (prvprv) *prvprv = prev;
    return st;
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
