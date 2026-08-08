/*
 * sys_mailbox.c - $CREMBX / $DELMBX System Services
 *
 * EXECUTIVE-RESIDENT (vms-d44). A mailbox is a record-oriented device one
 * process creates and another opens (OpenVMS System Services Reference,
 * $CREMBX) -- SHARED system state, so it lives in vms.ko
 * (src/kernel/vms_mbx.c), reached through src/libvmssys/vms_kif.c, exactly
 * like LNM$SYSTEM (vms-d37) and the device table (vms-d0b) before it.
 *
 * THIS REPLACES a per-process AF_UNIX socketpair implementation that
 * created a "MBA1:" wholly local to the creating process: a socketpair end
 * is not a thing an unrelated process can open, so two processes that each
 * called $CREMBX with the same logical name each got their OWN mailbox and
 * neither could ever reach the other's -- the exact INV-6 shape CLAUDE.md
 * Rule 9 forbids (a facility that looks like shared system state while
 * sharing nothing). See src/kernel/vms_mbx.h for the full account.
 *
 * NO PER-PROCESS FALLBACK: if /dev/vms is unreachable, every call below
 * fails honestly with SS$_NOSUCHDEV (propagated from vms_kif_mbx_*), never
 * a private substitute.
 *
 * RENDEZVOUS. A named mailbox's logical name is defined in LNM$SYSTEM
 * (executive-resident since vms-d37), not LNM$PROCESS_TABLE as the
 * userspace-only implementation used: a name in LNM$PROCESS is invisible
 * to every other process, so it could never let an unrelated process find
 * this mailbox at all -- defeating the one thing $CREMBX's logical-name
 * argument exists for. STATED AS AN OVMX DESIGN CHOICE: on genuine VMS,
 * $CREMBX's logical name lands in the caller's LNM$PROCESS_TABLE by
 * default, and only reaches LNM$SYSTEM under an explicit
 * /TABLE=LNM$SYSTEM-equivalent qualifier plus SYSNAM or SYSPRV privilege.
 * This implementation always targets LNM$SYSTEM when a name is given
 * (requiring the same privilege the executive already gates LNM$SYSTEM
 * mutation with, vms-5b7) -- a real, disclosed deviation, not a hidden
 * one, made because OVMX has no /TABLE qualifier plumbed through this
 * call's signature yet and a mailbox name nobody outside this process can
 * look up is not a rendezvous mechanism.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-PARTIAL: sys$crembx (vms-d44) -- exec: the mailbox itself (the unit
 *     number, the message queue, the buffer-quota accounting) is
 *     executive-resident in vms.ko (vms_kif_mbx_create); a logical name, if
 *     given, is defined in the executive-resident LNM$SYSTEM table
 *     (vms_kif_lnm_define, already wired by vms-d37/sys_logical.c).
 * OVMX-LOCAL: sys$crembx -- the returned channel NUMBER is a slot index into
 *     this process's own PCB channel table (pcb->channels[]), exactly like
 *     every other $ASSIGN-family channel (src/libvms/syssvc/sys_assign.c);
 *     the slot stores the EXECUTIVE's channel number in exec_chan, which is
 *     what actually identifies the mailbox to vms.ko.
 * OVMX-PARTIAL: sys$delmbx (vms-d44) -- exec: the actual deletion-marking
 *     decision is the executive's (vms_kif_mbx_delmbx); this call does not
 *     touch this process's own PCB slot, matching real $DELMBX (deletion
 *     happens on the LAST channel's $DASSGN, which this call is not).
 * OVMX-LOCAL: sys$delmbx -- resolving the caller's channel NUMBER to the
 *     executive's channel number (pcb->channels[chan].exec_chan) is a
 *     lookup in this process's own PCB, done before the executive is ever
 *     asked; a channel that is not this process's own mailbox channel is
 *     refused SS$_IVCHAN locally, without an ioctl round trip.
 */

#include <stdint.h>
#include <string.h>
#include "starlet.h"
#include "lnmdef.h"
#include "vms/pcb.h"
#include "vms_kif.h"

/*
 * sys$crembx - Create a mailbox and assign a channel.
 *
 * Parameters:
 *   prmflg - Permanent flag (1 = permanent, needs PRMMBX; 0 = temporary,
 *            needs TMPMBX -- both privileges are checked by the executive)
 *   chan    - Receives the assigned channel number
 *   maxmsg - Maximum message size in bytes (0 = executive default)
 *   bufquo - Buffer quota in bytes (0 = executive default)
 *   promsk - Protection mask (not yet enforced by the executive)
 *   acmode - Access mode (not yet enforced by the executive)
 *   lognam - Optional logical name; if given, defined in LNM$SYSTEM
 *            pointing at the mailbox's device name (see file header)
 *
 * Returns:
 *   SS$_NORMAL    - Mailbox created successfully
 *   SS$_BADPARAM  - NULL channel pointer
 *   SS$_EXQUOTA   - No free PCB channel slots
 *   SS$_NOPRIV    - Caller lacks PRMMBX (permanent) or TMPMBX (temporary)
 *   SS$_NOSUCHDEV - The executive is unreachable (INV-6: no fallback)
 */
uint32_t sys$crembx(int prmflg,
                    uint16_t *chan,
                    uint32_t maxmsg,
                    uint32_t bufquo,
                    uint32_t promsk,
                    uint32_t acmode,
                    const struct dsc$descriptor_s *lognam) {
    (void)promsk;
    (void)acmode;

    if (!chan) return SS$_BADPARAM;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_BADPARAM;

    uint32_t exec_chan = 0, unit = 0;
    char devnam[32];
    uint32_t st = vms_kif_mbx_create(prmflg, maxmsg, bufquo,
                                      &exec_chan, &unit,
                                      devnam, sizeof(devnam));
    if (!(st & 1)) return st;

    pthread_mutex_lock(&pcb->chan_lock);

    /* Find a free channel slot (slot 0 is reserved) */
    int slot = -1;
    for (int i = 1; i < PCB_MAX_CHANNELS; i++) {
        if (!pcb->channels[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&pcb->chan_lock);
        (void)vms_kif_dassgn(exec_chan);
        return SS$_EXQUOTA;
    }

    /* No local fd: the mailbox is entirely the executive's. IO$_READVBLK/
     * WRITEVBLK on this channel route through exec_chan (see sys_qio.c's
     * PCB_CHAN_MAILBOX check), never through pcb->channels[slot].fd. */
    pcb->channels[slot].fd = -1;
    pcb->channels[slot].in_use = 1;
    pcb->channels[slot].ref_count = 1;
    pcb->channels[slot].flags = PCB_CHAN_MAILBOX;
    pcb->channels[slot].mbx_peer_fd = -1;
    pcb->channels[slot].exec_chan = exec_chan;
    strncpy(pcb->channels[slot].devnam, devnam,
            sizeof(pcb->channels[slot].devnam) - 1);
    pcb->channels[slot].devnam[sizeof(pcb->channels[slot].devnam) - 1] = '\0';

    *chan = (uint16_t)slot;

    pthread_mutex_unlock(&pcb->chan_lock);

    /*
     * If a logical name was provided, define it in LNM$SYSTEM (see file
     * header for why SYSTEM, not PROCESS): the equivalence string is the
     * mailbox's own executive device name, so any process that translates
     * this name can then sys$assign() to the same executive-resident
     * mailbox.
     */
    if (lognam && lognam->dsc$a_pointer && lognam->dsc$w_length > 0) {
        struct dsc$descriptor_s tabdsc;
        vms_cstr_to_desc(&tabdsc, LNM$_SYSTEM_TABLE);

        struct item_list_3 itmlst[2];
        itmlst[0].buflen    = (uint16_t)strlen(devnam);
        itmlst[0].item_code = LNM$_STRING;
        itmlst[0].bufaddr   = devnam;
        itmlst[0].retlen    = NULL;
        itmlst[1].buflen    = 0;
        itmlst[1].item_code = 0;
        itmlst[1].bufaddr   = NULL;
        itmlst[1].retlen    = NULL;

        sys$crelnm(NULL, &tabdsc, lognam, NULL, itmlst);
    }

    return SS$_NORMAL;
}

/*
 * sys$delmbx - Mark a mailbox for deletion.
 *
 * Does NOT deassign the caller's own channel (real VMS behaviour: deletion
 * happens once the LAST channel -- this one or any other process's -- is
 * $DASSGN'd). Call sys$dassgn() separately to give this channel back.
 *
 * Parameters:
 *   chan - Channel number of the mailbox to mark
 *
 * Returns:
 *   SS$_NORMAL    - Mailbox marked for deletion
 *   SS$_IVCHAN    - Invalid, unassigned, or non-mailbox channel
 *   SS$_NOSUCHDEV - The executive is unreachable (INV-6: no fallback)
 */
uint32_t sys$delmbx(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return SS$_IVCHAN;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_IVCHAN;

    pthread_mutex_lock(&pcb->chan_lock);

    if (!pcb->channels[chan].in_use ||
        !(pcb->channels[chan].flags & PCB_CHAN_MAILBOX)) {
        pthread_mutex_unlock(&pcb->chan_lock);
        return SS$_IVCHAN;
    }

    uint32_t exec_chan = pcb->channels[chan].exec_chan;
    pthread_mutex_unlock(&pcb->chan_lock);

    return vms_kif_mbx_delmbx(exec_chan);
}
