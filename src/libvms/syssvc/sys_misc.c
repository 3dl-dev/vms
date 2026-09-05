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
 * OVMX-PARTIAL: sys$getsyi (vms-5919) -- exec: SYI$_CLUSTER_MEMBER and
 *     SYI$_CLUSTER_NODES read the CONNECTION MANAGER's own CLUB through the one
 *     projection that owns them (vms_kif_cluster_getsyi ->
 *     VMS_IOCTL_CLUSTER_GETSYI -> cluster_api_getsyi_project, FC-P3.7/P3.9), so
 *     F$GETSYI and SHOW CLUSTER read the SAME executive state and cannot
 *     disagree. Absent /dev/vms, or with the connection manager not started,
 *     both items are left unretrieved (honest -- not a non-member cluster of
 *     one, not the retired daemon's file). No fabricated membership.
 * OVMX-LOCAL: sys$getsyi -- the REMAINING items (NODENAME/VERSION/SCSNODE/
 *     SCSSYSTEMID/... ) answer from uname()/host sysconf(), not an executive
 *     system block. csidadr and nodename are still discarded
 *     ((void)csidadr; (void)nodename;), so a request aimed at another cluster
 *     node is answered with this machine's numbers as though aimed here (vms-642
 *     open; the cluster-item cutover above is the first executive-backed slice).
 * OVMX-PARTIAL: sys$getsyiw (vms-5919) -- exec: the same SYI$_CLUSTER_MEMBER /
 *     SYI$_CLUSTER_NODES read of the connection manager's CLUB as sys$getsyi
 *     above (this is the wait form of the same service).
 * OVMX-LOCAL: sys$getsyiw -- the remaining items answer from uname()/host
 *     sysconf(), as sys$getsyi's local half.
 * OVMX-USERSPACE: sys$setddir (vms-947) -- the process default directory is a
 *     per-process construct on real VMS too (the RMS default-directory string
 *     held in P1 process space), not a shared executive resource. OVMX keeps it
 *     in the PCB (pcb->default_dir via vms_pcb_get / vms_pcb_set_default_dir) --
 *     the same store sys$assign reads to resolve SYS$DISK and relative file
 *     specs -- so answering from and mutating the PCB fakes nothing: there is no
 *     executive-owned copy this could diverge from.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <time.h>
#include "starlet.h"
#include "vms/pcb.h"
#include "sysgen_params.h"
#include "ovmx_identity.h"
#include "vms_kif.h"        /* the executive OWNS privilege + cluster membership */

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
    uint64_t mask = prvadr ? *prvadr : 0;
    uint64_t prev = 0;

    /*
     * THE MUTATION IS THE EXECUTIVE'S (vms-pv1), and it does NOT depend on a
     * userspace PCB existing. Route it through /dev/vms: vms_ioctl_setprv
     * (kernel/vms_access.c) authorizes the request against this process's
     * AUTHORIZED (permanent) mask -- a caller without SETPRV that reaches
     * outside it gets the authorized subset and SS$_NOTALLPRIV, and cannot
     * widen the authorized mask at all (SS$_NOPRIV) -- and OWNS the resulting
     * state. This is what a process may NOT do for itself: writing
     * pcb->cur_privs directly was the vms-b2e per-process privilege LARP, a
     * self-assertion nothing authoritative read. If /dev/vms is unreachable the
     * kif layer returns an error status (INV-6, CLAUDE.md Rule 9); we never
     * fall back to a per-process fake grant. (Gating the whole service on
     * vms_pcb_get() would have made the executive-authoritative operation
     * fail SS$_BADPARAM for any caller without a userspace PCB -- the PCB is a
     * cache, not the authority.)
     */
    uint32_t st = vms_kif_setprv(mask, enbflg ? 1 : 0, prmflg ? 1 : 0, &prev);

    /*
     * Best-effort: mirror the executive's AUTHORITATIVE masks into the PCB
     * when this process has one, read back from the executive itself
     * ($GETJPI-self). This is a COPY of executive-owned state, never an
     * independent decision: the two remaining in-process readers --
     * sys_process.c fork inheritance and vmsprocess/access_modes.c's
     * CMKRNL/CMEXEC mode gate -- then see exactly what the executive holds. No
     * PCB (a bare image that never called vms_pcb_init) and no executive
     * read-back both leave the cache alone rather than invent a mask; the
     * status returned above is still the executive's.
     */
    struct vms_pcb *pcb = vms_pcb_get();
    if (pcb) {
        struct vms_procinfo info;
        memset(&info, 0, sizeof(info));
        if (vms_kif_getjpi_self(&info) & 1) {
            pthread_mutex_lock(&pcb->priv_lock);
            pcb->cur_privs = info.cur_privs;
            pcb->perm_privs = info.perm_privs;
            pthread_mutex_unlock(&pcb->priv_lock);
        }
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
                 * to the same question, which is the tell INV-1 kills.
                 * vms-28a: real VMS returns this as a fixed 8-char SPACE-PADDED
                 * field ("V7.3    "), byte-confirmed on the live oracle -- emit
                 * the padded field, not a counted trimmed token. */
                char field[OVMX_VMS_VERSION_FIELD_LEN];
                ovmx_compat_version_field(field);
                uint16_t len = OVMX_VMS_VERSION_FIELD_LEN;
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, field, len);
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

            case SYI$_ARCH_NAME: {
                /* The architecture OVMX was actually built for, as the
                 * VMS-style uppercase token (X86_64 / AARCH64), from the
                 * identity SSOT (INV-1) -- the same source F$GETSYI must use so
                 * the two surfaces never disagree. Was UNHANDLED: the item fell
                 * through to `default` while the routine still returned
                 * SS$_NORMAL, so a caller (MMK's main(), which asks for
                 * SYI$_ARCH_NAME to build its MMS$ARCH_NAME / MMSxxxx macros)
                 * trusted the success status and read an UNWRITTEN buffer and
                 * length -- uninitialized stack -- then walked it to the next
                 * NUL, smashing its own stack (vms-95c). Answering the item is
                 * the honest fix (Rule 9 / INV-6): report the metal truthfully,
                 * never a success for an answer not produced. */
                const char *arch = ovmx_hw_arch();
                uint16_t len = (uint16_t)strlen(arch);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, arch, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case SYI$_ARCH_TYPE: {
                /* VMS architecture-type code (longword): 1=VAX 2=Alpha 3=IA64
                 * 4=x86_64. Kept consistent with SYI$_ARCH_NAME above. */
#if defined(__x86_64__)
                uint32_t archtype = 4;   /* x86-64 */
#elif defined(__aarch64__)
                uint32_t archtype = 0;   /* OpenVMS never shipped on ARM */
#else
                uint32_t archtype = 0;
#endif
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = archtype;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
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

            case SYI$_CLUSTER_MEMBER:
            case SYI$_CLUSTER_NODES: {
                /*
                 * FC-P3.9 cutover (from vms-5919, itself a cutover from
                 * vms-8d4's file-reading facade): both items are answered from
                 * the CONNECTION MANAGER's own CLUB, through the ONE projection
                 * that owns them (cluster_api_getsyi_project, FC-P3.7) --
                 * VMS_IOCTL_CLUSTER_GETSYI. So F$GETSYI and SHOW CLUSTER cannot
                 * disagree about the same node: they read the same CLUB.
                 *
                 * WHAT CHANGED, AND WHY IT IS AN UPGRADE. The predecessor
                 * counted rows in a module-global block that a USERSPACE daemon
                 * wrote, and derived "member" from "the block has >= 1 row" --
                 * a count of what a daemon had published, not a fact about this
                 * cluster. CLUSTER_MEMBER is now the executive's own
                 * cl->state == VMS_CLUSTER_MEMBER, which ONLY a real membership
                 * record naming this node's SCSSYSTEMID ever sets, and
                 * CLUSTER_NODES is the CLUB's own p. 7-49 SELECTED-flag member
                 * count. NOTHING here derives one from the other.
                 *
                 * CLUSTER_NODES IS NO LONGER FLOORED AT 1. The predecessor
                 * returned 1 when the block was empty ("this node only"), which
                 * was a userspace `?:` and not a reading of anything. A node
                 * that is not a cluster member reports the CLUB's real count,
                 * and a caller that wants "how many systems do I know about"
                 * asks SHOW CLUSTER, which reads the CSB table.
                 *
                 * SS$_NOSUCHDEV -- no /dev/vms, or the connection manager is
                 * not started (VAXCLUSTER=0, or before CLUSTER_START) -- leaves
                 * the item UNRETRIEVED, which is the honest answer for a
                 * question nothing in the executive can answer (Rule 9/INV-6,
                 * vms-b44). It is NOT reported as a non-member cluster of one.
                 */
                struct vms_cluster_getsyi_args cargs;
                uint32_t st = vms_kif_cluster_getsyi(&cargs);
                uint32_t val;

                if (st != SS$_NORMAL) {
                    if (item->retlen) *item->retlen = 0;
                    break;
                }
                val = (item->item_code == SYI$_CLUSTER_MEMBER)
                          ? (uint32_t)cargs.cluster_member
                          : cargs.cluster_nodes;
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = val;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;
            }

            default:
                /* An item this build does not answer must not masquerade as a
                 * produced result: zero its return length so a caller cannot
                 * read a stale/uninitialized length as though it were valid
                 * (the garbage-length footgun that crashed MMK via
                 * SYI$_ARCH_NAME, vms-95c). The buffer is left untouched, as on
                 * VMS for an unretrieved item. */
                if (item->retlen) *item->retlen = 0;
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

/*
 * sys$setddir - Set/read the process default directory string.
 *
 * $SETDDIR reads and (optionally) changes the process default directory -- the
 * [dir] component used to resolve relative file specifications. It is a
 * per-process construct; OVMX stores it in the PCB (pcb->default_dir), the same
 * store sys$assign consults to resolve SYS$DISK and relative specs, so a change
 * made here is observable by subsequent relative filespec resolution.
 *
 * Arguments (OpenVMS System Services Reference, $SETDDIR):
 *   new_dir  - descriptor of the new default directory string. If 0 (NULL) the
 *              directory is not changed (a read-only call).
 *   old_len  - optional word to receive the length of the previous directory
 *              string returned: the number of bytes placed in old_dir's buffer
 *              when old_dir is supplied, otherwise the full previous length.
 *   old_dir  - optional descriptor of a buffer to receive the previous default
 *              directory string, truncated to the buffer's length.
 *
 * The previous directory is captured BEFORE any change, matching VMS order.
 *
 * Device coordination: OVMX conflates device+directory into pcb->default_dir.
 * A bare bracketed "[dir]" new spec keeps whatever device the current default
 * carries (mirrors SET DEFAULT / cmd_set_default in dcl_cmd_set.c); a full spec
 * that names a device/logical (contains ':') or a bare name is stored verbatim.
 * This keeps $SETDDIR and SET DEFAULT consistent and preserves a device set
 * separately via the SYS$DISK logical.
 *
 * Returns SS$_NORMAL on success; SS$_BADPARAM for a missing process context or
 * an empty / over-long new directory string.
 */
uint32_t sys$setddir(const struct dsc$descriptor_s *new_dir,
                     unsigned short *old_len,
                     struct dsc$descriptor_s *old_dir)
{
    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb)
        return SS$_BADPARAM;

    /* Snapshot the current (about-to-be-previous) default directory string. */
    char prev[256];
    strncpy(prev, pcb->default_dir, sizeof(prev) - 1);
    prev[sizeof(prev) - 1] = '\0';
    size_t prev_len = strlen(prev);

    /* Return the previous directory to the caller, if requested. */
    if (old_dir && old_dir->dsc$a_pointer) {
        size_t cap = old_dir->dsc$w_length;
        size_t n = (prev_len < cap) ? prev_len : cap;
        if (n > 0)
            memcpy(old_dir->dsc$a_pointer, prev, n);
        if (old_len)
            *old_len = (unsigned short)n;
    } else if (old_len) {
        *old_len = (unsigned short)prev_len;
    }

    /* No new directory descriptor -> read-only call. */
    if (!new_dir || !new_dir->dsc$a_pointer)
        return SS$_NORMAL;

    size_t nlen = new_dir->dsc$w_length;
    if (nlen == 0 || nlen >= sizeof(pcb->default_dir))
        return SS$_BADPARAM;

    char newspec[256];
    memcpy(newspec, new_dir->dsc$a_pointer, nlen);
    newspec[nlen] = '\0';

    char merged[256];
    if (newspec[0] == '[' && !strchr(newspec, ':')) {
        /* Bracketed directory only -> keep the current default's device. */
        char device[128] = "";
        const char *colon = strchr(prev, ':');
        if (colon) {
            size_t dlen = (size_t)(colon - prev);
            if (dlen < sizeof(device)) {
                memcpy(device, prev, dlen);
                device[dlen] = '\0';
            }
        }
        if (device[0])
            snprintf(merged, sizeof(merged), "%s:%s", device, newspec);
        else {
            strncpy(merged, newspec, sizeof(merged) - 1);
            merged[sizeof(merged) - 1] = '\0';
        }
    } else {
        strncpy(merged, newspec, sizeof(merged) - 1);
        merged[sizeof(merged) - 1] = '\0';
    }

    if (strlen(merged) >= sizeof(pcb->default_dir))
        return SS$_BADPARAM;

    vms_pcb_set_default_dir(merged);
    return SS$_NORMAL;
}
