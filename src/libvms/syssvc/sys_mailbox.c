/*
 * sys_mailbox.c - $CREMBX / $DELMBX System Services
 *
 * Implements VMS mailbox creation and deletion using Unix socketpairs.
 * On real VMS, mailboxes are kernel-managed message-oriented IPC devices
 * with device names like MBA<n>:.  This implementation creates a
 * SOCK_DGRAM socketpair to preserve message boundaries, assigns one
 * end to a channel, and stores the other end as the peer fd.
 *
 * If a logical name is provided, it is created in LNM$PROCESS_TABLE
 * pointing to the mailbox device name MBA<n>:.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include "starlet.h"
#include "vms/pcb.h"

/* Auto-assigned mailbox unit number, starting from 1 */
static int mbx_next_unit = 1;
static pthread_mutex_t mbx_unit_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * sys$crembx - Create a mailbox and assign a channel.
 *
 * Creates a message-oriented IPC channel implemented as a Unix
 * SOCK_DGRAM socketpair.  One end is assigned to the returned
 * channel number; the other end is stored in mbx_peer_fd for
 * use by readers/writers.
 *
 * Parameters:
 *   prmflg - Permanent flag (1 = permanent mailbox, 0 = temporary)
 *   chan    - Receives the assigned channel number
 *   maxmsg - Maximum message size in bytes (0 = default 256)
 *   bufquo - Buffer quota in bytes (0 = default 1024)
 *   promsk - Protection mask (currently ignored)
 *   acmode - Access mode (currently ignored)
 *   lognam - Optional logical name to create for this mailbox
 *
 * Returns:
 *   SS$_NORMAL    - Mailbox created successfully
 *   SS$_BADPARAM  - NULL channel pointer
 *   SS$_EXQUOTA   - No free channel slots
 *   SS$_SSFAIL    - socketpair() failed
 */
uint32_t sys$crembx(int prmflg,
                    uint16_t *chan,
                    uint32_t maxmsg,
                    uint32_t bufquo,
                    uint32_t promsk,
                    uint32_t acmode,
                    const struct dsc$descriptor_s *lognam) {
    (void)prmflg;
    (void)maxmsg;
    (void)bufquo;
    (void)promsk;
    (void)acmode;

    if (!chan) return SS$_BADPARAM;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_BADPARAM;

    /* Create the socketpair for the mailbox */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        return SS$_SSFAIL;
    }

    /* Allocate a mailbox unit number */
    pthread_mutex_lock(&mbx_unit_lock);
    int unit = mbx_next_unit++;
    pthread_mutex_unlock(&mbx_unit_lock);

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
        close(sv[0]);
        close(sv[1]);
        return SS$_EXQUOTA;
    }

    /* Build the device name MBA<unit>: */
    char devname[32];
    snprintf(devname, sizeof(devname), "MBA%d:", unit);

    /* Populate the channel table entry.
     * sv[0] is the "writer" end (assigned to this channel).
     * sv[1] is the "reader" end (stored in mbx_peer_fd). */
    pcb->channels[slot].fd = sv[0];
    pcb->channels[slot].in_use = 1;
    pcb->channels[slot].ref_count = 1;
    pcb->channels[slot].flags = PCB_CHAN_MAILBOX;
    pcb->channels[slot].mbx_peer_fd = sv[1];
    strncpy(pcb->channels[slot].devnam, devname,
            sizeof(pcb->channels[slot].devnam) - 1);
    pcb->channels[slot].devnam[sizeof(pcb->channels[slot].devnam) - 1] = '\0';

    *chan = (uint16_t)slot;

    pthread_mutex_unlock(&pcb->chan_lock);

    /* If a logical name was provided, create it in the process table
     * pointing to the mailbox device name. */
    if (lognam && lognam->dsc$a_pointer && lognam->dsc$w_length > 0) {
        struct dsc$descriptor_s tabdsc;
        vms_cstr_to_desc(&tabdsc, LNM$_PROCESS_TABLE);

        char equiv[32];
        snprintf(equiv, sizeof(equiv), "MBA%d:", unit);

        struct item_list_3 itmlst[2];
        itmlst[0].buflen    = (uint16_t)strlen(equiv);
        itmlst[0].item_code = LNM$_STRING;
        itmlst[0].bufaddr   = equiv;
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
 * sys$delmbx - Delete (mark for deletion) a mailbox.
 *
 * On real VMS, this marks the mailbox for deletion when all channels
 * are deassigned.  This implementation immediately closes both ends
 * of the socketpair and releases the channel.
 *
 * Parameters:
 *   chan - Channel number of the mailbox to delete
 *
 * Returns:
 *   SS$_NORMAL  - Mailbox deleted
 *   SS$_IVCHAN  - Invalid or non-mailbox channel
 */
uint32_t sys$delmbx(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return SS$_IVCHAN;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_IVCHAN;

    pthread_mutex_lock(&pcb->chan_lock);

    if (!pcb->channels[chan].in_use) {
        pthread_mutex_unlock(&pcb->chan_lock);
        return SS$_IVCHAN;
    }

    /* Verify this is actually a mailbox channel */
    if (!(pcb->channels[chan].flags & PCB_CHAN_MAILBOX)) {
        pthread_mutex_unlock(&pcb->chan_lock);
        return SS$_IVCHAN;
    }

    /* Close both ends of the socketpair */
    if (pcb->channels[chan].fd >= 0) {
        close(pcb->channels[chan].fd);
    }
    if (pcb->channels[chan].mbx_peer_fd >= 0) {
        close(pcb->channels[chan].mbx_peer_fd);
    }

    /* Clear the channel table entry */
    pcb->channels[chan].fd = -1;
    pcb->channels[chan].in_use = 0;
    pcb->channels[chan].ref_count = 0;
    pcb->channels[chan].flags = 0;
    pcb->channels[chan].mbx_peer_fd = -1;
    pcb->channels[chan].devnam[0] = '\0';

    pthread_mutex_unlock(&pcb->chan_lock);
    return SS$_NORMAL;
}
