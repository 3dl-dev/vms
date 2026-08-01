/*
 * sys_assign.c - $ASSIGN / $DASSGN System Services
 *
 * Implements VMS I/O channel assignment on top of Linux file descriptors.
 * VMS programs do not use file descriptors directly; instead they assign
 * a 16-bit channel number to a device or file, then use $QIO/$QIOW on
 * that channel. Channel state is stored in the Per-Process Control Block.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * OVMX-USERSPACE: sys$assign (vms-dv1) -- allocates a slot in
 *     pcb->channels[] in the per-process PCB and stores a Linux fd in it. No
 *     executive device or channel database is consulted; the channel table is
 *     process-local memory, so no other process can name the channel and it
 *     does not survive exec.
 * OVMX-USERSPACE: sys$dassgn (vms-dv1) -- closes the fd in the caller's own
 *     pcb->channels[chan]; a channel assigned by another process cannot be
 *     named, let alone deassigned.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <ctype.h>
#include "starlet.h"
#include "vms/pcb.h"

/*
 * VMS device resolution result.
 *
 * After resolve_vms_device() returns:
 *   - If resolved_fd >= 0, the caller should dup() that fd (for SYS$INPUT etc.)
 *   - If resolved_path[0] != '\0', the caller should open() that path
 *   - If neither, the name was not a recognized VMS device; fall through
 *     to open the raw name as a file.
 */
struct vms_device_result {
    int  resolved_fd;           /* fd to dup (-1 if not applicable) */
    char resolved_path[256];    /* Linux path to open ("" if not applicable) */
    int  is_mailbox;            /* nonzero if MBA<n>: device */
    int  mbx_unit;              /* mailbox unit number (if is_mailbox) */
};

/*
 * resolve_vms_device - Map a VMS device name to a Linux path or fd.
 *
 * Checks if 'name' is a recognized VMS device specification (ending
 * in ':').  If it matches a known device, populates 'result' with
 * the Linux equivalent.
 *
 * Returns 1 if the name was resolved (use result), 0 if not (fall through).
 */
static int resolve_vms_device(const char *name, struct vms_device_result *result) {
    char upper[256];
    size_t len;

    result->resolved_fd = -1;
    result->resolved_path[0] = '\0';
    result->is_mailbox = 0;
    result->mbx_unit = 0;

    if (!name || !name[0])
        return 0;

    /* Upcase for comparison */
    len = strlen(name);
    if (len >= sizeof(upper))
        len = sizeof(upper) - 1;
    for (size_t i = 0; i < len; i++)
        upper[i] = (char)toupper((unsigned char)name[i]);
    upper[len] = '\0';

    /* VMS device names end with ':' */
    if (len == 0 || upper[len - 1] != ':')
        return 0;

    /* Strip the trailing ':' for matching */
    upper[len - 1] = '\0';

    /* Terminal device */
    if (strcmp(upper, "TT") == 0 || strcmp(upper, "TT0") == 0) {
        strncpy(result->resolved_path, "/dev/tty", sizeof(result->resolved_path) - 1);
        result->resolved_path[sizeof(result->resolved_path) - 1] = '\0';
        return 1;
    }

    /* Standard I/O devices - use dup() of the existing fd */
    if (strcmp(upper, "SYS$INPUT") == 0) {
        result->resolved_fd = 0;  /* stdin */
        return 1;
    }
    if (strcmp(upper, "SYS$OUTPUT") == 0) {
        result->resolved_fd = 1;  /* stdout */
        return 1;
    }
    if (strcmp(upper, "SYS$ERROR") == 0) {
        result->resolved_fd = 2;  /* stderr */
        return 1;
    }

    /* Null device */
    if (strcmp(upper, "NLA0") == 0) {
        strncpy(result->resolved_path, "/dev/null", sizeof(result->resolved_path) - 1);
        result->resolved_path[sizeof(result->resolved_path) - 1] = '\0';
        return 1;
    }

    /* Default disk - use process default directory from PCB */
    if (strcmp(upper, "SYS$DISK") == 0) {
        struct vms_pcb *pcb = vms_pcb_get();
        if (pcb && pcb->default_dir[0]) {
            strncpy(result->resolved_path, pcb->default_dir,
                    sizeof(result->resolved_path) - 1);
            result->resolved_path[sizeof(result->resolved_path) - 1] = '\0';
        } else {
            strncpy(result->resolved_path, ".", sizeof(result->resolved_path) - 1);
            result->resolved_path[sizeof(result->resolved_path) - 1] = '\0';
        }
        return 1;
    }

    /* Mailbox device MBA<n>: */
    if (strncmp(upper, "MBA", 3) == 0 && upper[3] != '\0') {
        char *endp;
        long unit = strtol(upper + 3, &endp, 10);
        if (*endp == '\0' && unit >= 0) {
            result->is_mailbox = 1;
            result->mbx_unit = (int)unit;
            return 1;
        }
    }

    return 0;  /* Not a recognized VMS device */
}

/*
 * sys$assign - Assign an I/O channel to a device.
 *
 * On VMS, this assigns a channel number to a device. On OVMX, we resolve
 * VMS device names (TT:, SYS$INPUT:, NLA0:, etc.) to Linux equivalents,
 * then open or dup the result and store the mapping in the PCB channel table.
 *
 * Parameters:
 *   devnam  - Descriptor containing the device/file name
 *   chan    - Receives the assigned channel number
 *   acmode  - Access mode (ignored in this implementation)
 *   mbxnam  - Associated mailbox name (ignored in this implementation)
 *
 * Returns:
 *   SS$_NORMAL    - Channel assigned successfully
 *   SS$_BADPARAM  - NULL descriptor or channel pointer
 *   SS$_IVDEVNAM  - Empty device name
 *   SS$_EXQUOTA   - No free channels available
 *   SS$_NOSUCHDEV - Could not open the device/file
 */
uint32_t sys$assign(const struct dsc$descriptor_s *devnam,
                    uint16_t *chan,
                    uint32_t acmode,
                    const struct dsc$descriptor_s *mbxnam) {
    (void)acmode;
    (void)mbxnam;

    if (!devnam || !chan) return SS$_BADPARAM;
    if (!devnam->dsc$a_pointer || devnam->dsc$w_length == 0)
        return SS$_IVDEVNAM;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_BADPARAM;

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
        return SS$_EXQUOTA;
    }

    /* Extract device name from descriptor */
    char name[256];
    dsc$strncpy(name, devnam, sizeof(name));

    /* Attempt VMS device name resolution */
    struct vms_device_result devres;
    int fd = -1;

    if (resolve_vms_device(name, &devres)) {
        if (devres.is_mailbox) {
            /* Mailbox devices are handled by sys$crembx, not sys$assign directly.
             * For now, return an error; the user should use sys$crembx. */
            pthread_mutex_unlock(&pcb->chan_lock);
            return SS$_IVDEVNAM;
        } else if (devres.resolved_fd >= 0) {
            /* SYS$INPUT/SYS$OUTPUT/SYS$ERROR: dup the standard fd */
            fd = dup(devres.resolved_fd);
        } else if (devres.resolved_path[0]) {
            /* Resolved to a Linux path: open it */
            fd = open(devres.resolved_path, O_RDWR);
            if (fd < 0) {
                fd = open(devres.resolved_path, O_RDONLY);
            }
        }
    } else {
        /* Not a VMS device -- try to open as a plain file */
        fd = open(name, O_RDWR);
        if (fd < 0) {
            fd = open(name, O_RDONLY);
        }
    }

    if (fd < 0) {
        pthread_mutex_unlock(&pcb->chan_lock);
        return SS$_NOSUCHDEV;
    }

    /* Populate the channel table entry */
    pcb->channels[slot].fd = fd;
    pcb->channels[slot].in_use = 1;
    pcb->channels[slot].ref_count = 1;
    pcb->channels[slot].flags = 0;
    pcb->channels[slot].mbx_peer_fd = -1;
    strncpy(pcb->channels[slot].devnam, name,
            sizeof(pcb->channels[slot].devnam) - 1);
    pcb->channels[slot].devnam[sizeof(pcb->channels[slot].devnam) - 1] = '\0';
    *chan = (uint16_t)slot;

    pthread_mutex_unlock(&pcb->chan_lock);
    return SS$_NORMAL;
}

/*
 * sys$dassgn - Deassign an I/O channel.
 *
 * Closes the underlying Linux file descriptor and releases the
 * channel table entry for reuse. For mailbox channels, also closes
 * the peer end of the socketpair.
 *
 * Returns:
 *   SS$_NORMAL - Channel deassigned
 *   SS$_IVCHAN - Invalid or unassigned channel number
 */
uint32_t sys$dassgn(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return SS$_IVCHAN;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_IVCHAN;

    pthread_mutex_lock(&pcb->chan_lock);

    if (!pcb->channels[chan].in_use) {
        pthread_mutex_unlock(&pcb->chan_lock);
        return SS$_IVCHAN;
    }

    /* Close the underlying fd */
    if (pcb->channels[chan].fd >= 0) {
        close(pcb->channels[chan].fd);
    }

    /* For mailbox channels, also close the peer fd */
    if ((pcb->channels[chan].flags & PCB_CHAN_MAILBOX) &&
        pcb->channels[chan].mbx_peer_fd >= 0) {
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

/*
 * vms$$chan_to_fd - Internal helper: look up the Linux fd for a channel number.
 *
 * Used by sys$qio and other I/O services to translate a VMS channel
 * into the Linux file descriptor needed for actual I/O operations.
 *
 * Returns the fd on success, or -1 if the channel is invalid.
 */
int vms$$chan_to_fd(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return -1;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return -1;
    if (!pcb->channels[chan].in_use) return -1;
    return pcb->channels[chan].fd;
}
