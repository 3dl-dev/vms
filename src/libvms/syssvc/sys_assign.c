/*
 * sys_assign.c - $ASSIGN / $DASSGN System Services
 *
 * Implements VMS I/O channel assignment on top of Linux file descriptors.
 * VMS programs do not use file descriptors directly; instead they assign
 * a 16-bit channel number to a device or file, then use $QIO/$QIOW on
 * that channel. Channel state is stored in the Per-Process Control Block.
 */

/*
 * OVMX service register (rd vms-d89) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * These two used to be exempt for containing a vms_kif_* call. That exemption
 * was wrong here in a way worth naming: the executive is reached for ONE device
 * (the console terminal), and every other channel this file hands out is a
 * private Linux fd. The register now has to say so.
 *
 * THEY CITED vms-1c57, WHICH IS CLOSED (vms-fab). It wired the console channel
 * and met its outcome. vms-6aa owns the remainder these lines describe, and it
 * covers $QIO/$QIOW in src/libvms/syssvc/sys_qio.c too, because a channel is
 * what $QIO operates on -- fixing either half alone rebuilds the two-models
 * divergence vms-1c57 was filed to end.
 *
 * OVMX-PARTIAL: sys$assign (vms-6aa) -- exec: a channel for the executive's
 *     registered console terminal comes from vms_kif_assign(), so the device
 *     table and the I/O path agree on what "the terminal" is.
 * OVMX-LOCAL: sys$assign -- every other channel is a Linux fd recorded in this
 *     process's PCB. The channel number means nothing to any other process, and
 *     nothing outside this image can see the assignment.
 * OVMX-PARTIAL: sys$dassgn (vms-6aa) -- exec: deassigning a terminal channel
 *     releases the executive's channel through vms_kif_dassgn().
 * OVMX-LOCAL: sys$dassgn -- for every other channel it closes a process-local fd
 *     and clears a process-local PCB slot.
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <ctype.h>
#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "vms/pcb.h"
#include "vms_kif.h"

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
    /*
     * vms-527: nonzero if this name identifies the INET pseudo-device --
     * TCPIP$DEVICE: (the transport device) or a BG<n>: unit. Like a mailbox,
     * BGn: is executive-resident (src/kernel/vms_bg.c): $ASSIGN allocates a
     * fresh unit through the executive (vms_kif_bg_create), the channel's fd
     * stays -1, and $QIO routes through qio_bg_op() instead of the fd path.
     */
    int  is_bg;
    /*
     * vms-1c57: nonzero if this name identifies the executive's console
     * terminal (src/kernel/vms_devtab.c's ONE registered device, OPA0:).
     * $ASSIGN must obtain a channel FROM THE EXECUTIVE for this case --
     * see the call to vms_kif_assign() below -- so that the device table
     * and the I/O path agree on what "the terminal" is, instead of $QIO
     * running against a raw /dev/tty the executive never heard about.
     */
    int  is_terminal;
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
    result->is_terminal = 0;
    result->is_bg = 0;

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

    /*
     * Terminal device. TT:/TT0: is the generic job-terminal alias; OPA0:
     * and its physical form _OPA0: name the executive's console device
     * directly (src/kernel/vms_devtab.c). OVMX has exactly one terminal
     * registered in the device table, so all four spellings name the same
     * executive row -- see is_terminal below and vms-1c57.
     *
     * /dev/console, NOT /dev/tty (measured, vms-1c57): /dev/tty resolves
     * to the CALLING PROCESS's own controlling terminal, and nothing in
     * OVMX ever establishes one -- not tests/qemu/init.sh, and grepping
     * src/ovmx_init/ for setsid/TIOCSCTTY/"/dev/console" finds nothing
     * either. Every process here inherits whatever fds PID 1 started
     * with, but inheriting a descriptor is not the same as HOLDING a
     * controlling terminal, and open("/dev/tty", ...) needs the latter.
     * The result was measured directly: sys$assign("TT:") against a real
     * /dev/vms returned SS$_NOSUCHDEV with errno ENXIO ("no such device
     * or address") -- the executive channel was granted (vms_kif_assign
     * succeeded and was cleaned back up), only the LOCAL open() failed.
     * OPA0: is, by the device table's own comment, OVMX's one physical
     * console -- so the Linux-side path for it should be the physical
     * console device, not a session-relative indirection this codebase
     * never sets up. This does not extend to a real terminal-session
     * facility (TIOCSCTTY, job/process login binding a real ctty): that
     * is out of vms-d0b's own stated scope ("console terminal only") and
     * out of this item's, and is not invented here.
     */
    if (strcmp(upper, "TT") == 0 || strcmp(upper, "TT0") == 0 ||
        strcmp(upper, "OPA0") == 0 || strcmp(upper, "_OPA0") == 0) {
        strncpy(result->resolved_path, "/dev/console", sizeof(result->resolved_path) - 1);
        result->resolved_path[sizeof(result->resolved_path) - 1] = '\0';
        result->is_terminal = 1;
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

    /*
     * INET pseudo-device (vms-527). TCPIP$DEVICE: is the INET transport
     * device VMS TCP/IP Services present; BG<n>: is a specific INET unit.
     * Both resolve to a BG-create in the executive -- $ASSIGN allocates a
     * fresh BGn: unit the way $CREMBX allocates a fresh mailbox unit (this
     * first increment does not model rendezvous to a specific existing BG
     * unit; every $ASSIGN gets its own). BG is distinct from the NIC device
     * ETH0: it layers over (device-native-naming).
     */
    if (strcmp(upper, "TCPIP$DEVICE") == 0) {
        result->is_bg = 1;
        return 1;
    }
    if (strncmp(upper, "BG", 2) == 0 && upper[2] != '\0') {
        char *endp;
        long unit = strtol(upper + 2, &endp, 10);
        if (*endp == '\0' && unit >= 0) {
            result->is_bg = 1;
            return 1;
        }
    }

    return 0;  /* Not a recognized VMS device */
}

/*
 * assign_resolve_mailbox_by_name - reach a mailbox by its LOGICAL NAME (vms-mb1).
 *
 * sys$crembx publishes the name a mailbox was created with (its lognam
 * argument) into the executive-resident LNM$SYSTEM table, pointing at the
 * mailbox's own "MBAn:" device name (src/libvms/syssvc/sys_mailbox.c, on the
 * executive-resident LNM$SYSTEM of vms-d37 / vms-d44). An UNRELATED process
 * that knows only that NAME -- never the unit number, and holding no pipe or
 * shared fd to the creator -- reaches the same executive-resident mailbox by
 * translating the name here. That is exactly what OpenVMS $ASSIGN does with
 * its device-name argument: it translates it through the logical-name tables
 * before it looks a device up (OpenVMS I/O User's Reference, $ASSIGN). This
 * iterates (a logical may point at another logical) with the same small cap
 * VMS uses, and stops the instant a translation resolves to a mailbox device.
 *
 * SCOPED TO THE MAILBOX CASE ON PURPOSE. The caller consults this ONLY when
 * the raw name is not already a recognized device, and ADOPTS the result ONLY
 * when it resolves to a mailbox -- so SYS$INPUT/SYS$OUTPUT/OPA0: and ordinary
 * file assignment keep exactly the behaviour they had before this item. The
 * translation uses the public sys$trnlnm with no table named, i.e. the
 * standard search order, which reaches the executive-resident LNM$SYSTEM after
 * the process-private tables miss (src/libvms/syssvc/sys_logical.c).
 *
 * Returns 1 and fills *out with the "MBAn:" device name (and *devres with its
 * resolution) when the name resolves to a mailbox; 0 otherwise.
 */
static int assign_resolve_mailbox_by_name(const char *name,
                                          char *out, size_t outsz,
                                          struct vms_device_result *devres) {
    char cur[256];

    /* Strip a single trailing ':' so "MYBOX" and "MYBOX:" translate the same
     * logical name (the name $CREMBX defines carries no device colon). */
    size_t n = strlen(name);
    if (n >= sizeof(cur)) n = sizeof(cur) - 1;
    memcpy(cur, name, n);
    cur[n] = '\0';
    if (n > 0 && cur[n - 1] == ':') cur[n - 1] = '\0';
    if (cur[0] == '\0') return 0;

    for (int depth = 0; depth < 10; depth++) {
        struct dsc$descriptor_s namdsc;
        vms_cstr_to_desc(&namdsc, cur);

        char equiv[256];
        uint16_t rl = 0;
        struct item_list_3 itmlst[2];
        memset(itmlst, 0, sizeof(itmlst));
        itmlst[0].buflen    = (uint16_t)(sizeof(equiv) - 1);
        itmlst[0].item_code = LNM$_STRING;
        itmlst[0].bufaddr   = equiv;
        itmlst[0].retlen    = &rl;
        equiv[0] = '\0';

        uint32_t st = sys$trnlnm(NULL, NULL, &namdsc, NULL, itmlst);
        if (!(st & 1)) return 0;                 /* not a logical name */
        if (rl >= sizeof(equiv)) rl = (uint16_t)(sizeof(equiv) - 1);
        equiv[rl] = '\0';
        if (rl == 0 || strcmp(equiv, cur) == 0) return 0;  /* no progress */

        if (resolve_vms_device(equiv, devres) && devres->is_mailbox) {
            strncpy(out, equiv, outsz - 1);
            out[outsz - 1] = '\0';
            return 1;
        }

        /* Not a mailbox yet -- follow another level of indirection. */
        strncpy(cur, equiv, sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = '\0';
    }
    return 0;
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
    uint32_t exec_chan = 0;

    int resolved = resolve_vms_device(name, &devres);
    if (!resolved) {
        /*
         * vms-mb1: the raw name is not a literal device -- it may be a
         * mailbox LOGICAL NAME an unrelated process is using to reach a
         * mailbox by name (sys$crembx published it in LNM$SYSTEM). Translate
         * it; if it resolves to a mailbox device, adopt that "MBAn:" name and
         * fall into the mailbox assign path below. Nothing else changes.
         */
        char mbxdev[256];
        if (assign_resolve_mailbox_by_name(name, mbxdev, sizeof(mbxdev),
                                           &devres)) {
            strncpy(name, mbxdev, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            resolved = 1;
        }
    }

    if (resolved) {
        if (devres.is_mailbox) {
            /*
             * vms-d44: $ASSIGN to an EXISTING mailbox by device name -- the
             * rendezvous path an UNRELATED process uses to reach a mailbox
             * it did not $CREMBX itself (having learned "MBAn:" from a
             * logical name translate, or the literal name). The mailbox is
             * entirely the executive's (src/kernel/vms_mbx.c); there is no
             * local fd to open, so this channel's fd stays -1 and
             * IO$_READVBLK/WRITEVBLK route through exec_chan instead (see
             * sys_qio.c's PCB_CHAN_MAILBOX check).
             */
            uint32_t mbx_exec_chan = 0;
            uint32_t st = vms_kif_mbx_assign(name, &mbx_exec_chan);
            if (!(st & 1)) {
                pthread_mutex_unlock(&pcb->chan_lock);
                return st;
            }

            pcb->channels[slot].fd = -1;
            pcb->channels[slot].in_use = 1;
            pcb->channels[slot].ref_count = 1;
            pcb->channels[slot].flags = PCB_CHAN_MAILBOX;
            pcb->channels[slot].mbx_peer_fd = -1;
            pcb->channels[slot].exec_chan = mbx_exec_chan;
            strncpy(pcb->channels[slot].devnam, name,
                    sizeof(pcb->channels[slot].devnam) - 1);
            pcb->channels[slot].devnam[sizeof(pcb->channels[slot].devnam) - 1] = '\0';
            *chan = (uint16_t)slot;

            pthread_mutex_unlock(&pcb->chan_lock);
            return SS$_NORMAL;
        }

        if (devres.is_bg) {
            /*
             * vms-527: $ASSIGN TCPIP$DEVICE:/BGn: -- allocate a fresh INET
             * pseudo-device unit + channel in the executive (src/kernel/
             * vms_bg.c), the analogue of the mailbox $CREMBX path above. The
             * socket is executive-resident; there is no local fd to open, so
             * this channel's fd stays -1 and $QIO routes through qio_bg_op()
             * (see sys_qio.c's PCB_CHAN_BG check). If the executive is absent
             * vms_kif_bg_create returns SS$_NOSUCHDEV -- no per-process socket
             * fake (INV-6).
             */
            uint32_t bg_exec_chan = 0;
            uint32_t st = vms_kif_bg_create(&bg_exec_chan, NULL, NULL, 0);
            if (!(st & 1)) {
                pthread_mutex_unlock(&pcb->chan_lock);
                return st;
            }

            pcb->channels[slot].fd = -1;
            pcb->channels[slot].in_use = 1;
            pcb->channels[slot].ref_count = 1;
            pcb->channels[slot].flags = PCB_CHAN_BG;
            pcb->channels[slot].mbx_peer_fd = -1;
            pcb->channels[slot].exec_chan = bg_exec_chan;
            strncpy(pcb->channels[slot].devnam, name,
                    sizeof(pcb->channels[slot].devnam) - 1);
            pcb->channels[slot].devnam[sizeof(pcb->channels[slot].devnam) - 1] = '\0';
            *chan = (uint16_t)slot;

            pthread_mutex_unlock(&pcb->chan_lock);
            return SS$_NORMAL;
        }

        if (devres.is_terminal) {
            /*
             * vms-1c57: THE CHANNEL IS THE IDENTITY. A terminal is a row in
             * the executive's device table (src/kernel/vms_devtab.c), not a
             * fact this process is entitled to decide on its own -- so the
             * channel comes FROM the executive before anything is opened
             * locally. "OPA0:" is passed literally: it is the one terminal
             * OVMX's device table carries (see resolve_vms_device's is_terminal
             * comment), so every alias a program can spell (TT:, TT0:, OPA0:,
             * _OPA0:) resolves to the same executive row. If the executive
             * refuses (no such device, no free channel, ...) $ASSIGN refuses
             * too -- there is no per-process fallback identity for a terminal
             * (CLAUDE.md Rule 11).
             */
            uint32_t st = vms_kif_assign("OPA0:", &exec_chan);
            if (!(st & 1)) {
                pthread_mutex_unlock(&pcb->chan_lock);
                return st;
            }
        }

        if (devres.resolved_fd >= 0) {
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
        /*
         * The executive channel was granted but there is no local byte-I/O
         * path to go with it (e.g. no /dev/tty in this process's controlling
         * terminal). Give the executive channel back rather than leaking it
         * -- $ASSIGN as a whole failed, so nothing should remain assigned.
         */
        if (exec_chan != 0)
            (void)vms_kif_dassgn(exec_chan);
        pthread_mutex_unlock(&pcb->chan_lock);
        return SS$_NOSUCHDEV;
    }

    /* Populate the channel table entry */
    pcb->channels[slot].fd = fd;
    pcb->channels[slot].in_use = 1;
    pcb->channels[slot].ref_count = 1;
    pcb->channels[slot].flags = 0;
    pcb->channels[slot].mbx_peer_fd = -1;
    pcb->channels[slot].exec_chan = exec_chan;
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

    /*
     * vms-527: an INET pseudo-device channel is released through the BG
     * driver's own $DASSGN ioctl (vms_kif_bg_dassgn), which also releases the
     * executive-resident socket. It cannot go through the generic
     * vms_kif_dassgn below: BG channels live in the executive's bg_channels
     * list, not its device table, so the generic $DASSGN would not find one
     * (and the generic path lives in the substrate-agnostic kernel-core,
     * which must not name the host socket API). fd is -1, so there is no
     * local descriptor to close afterwards.
     */
    if (pcb->channels[chan].flags & PCB_CHAN_BG) {
        uint32_t st = vms_kif_bg_dassgn(pcb->channels[chan].exec_chan);
        if (!(st & 1)) {
            pthread_mutex_unlock(&pcb->chan_lock);
            return st;
        }
    } else if (pcb->channels[chan].exec_chan != 0) {
        /*
         * vms-1c57: give the executive's channel back FIRST. If it refuses --
         * which should not happen, since this process is the only one that
         * could have obtained this exec_chan -- leave the whole channel
         * assigned rather than tearing down the local half only: the channel
         * is the identity, so a channel the executive still holds is still
         * assigned, whatever the local bookkeeping believes.
         */
        uint32_t st = vms_kif_dassgn(pcb->channels[chan].exec_chan);
        if (!(st & 1)) {
            pthread_mutex_unlock(&pcb->chan_lock);
            return st;
        }
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
    pcb->channels[chan].exec_chan = 0;
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

/*
 * vms$$chan_exec_chan - Internal helper: the EXECUTIVE channel number this
 * PCB slot was bound to by $ASSIGN, or 0 if it was never bound to one (a
 * plain file, a mailbox, or one of SYS$INPUT/SYS$OUTPUT/SYS$ERROR -- none
 * of those are device-table rows).
 *
 * vms-1c57: used by sys$qio to revalidate a terminal channel against the
 * executive before doing real I/O on it, instead of trusting a local slot
 * number the executive never saw.
 */
uint32_t vms$$chan_exec_chan(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return 0;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return 0;
    if (!pcb->channels[chan].in_use) return 0;
    return pcb->channels[chan].exec_chan;
}

/*
 * vms$$chan_is_mailbox - Internal helper: is this channel a mailbox
 * channel (vms-d44)? Used by sys$qio to route IO$_READVBLK/WRITEVBLK to
 * the executive (vms_kif_mbx_read/write) instead of the fd-based path --
 * a mailbox channel's fd is always -1, since the mailbox itself is
 * entirely the executive's.
 */
int vms$$chan_is_mailbox(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return 0;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return 0;
    if (!pcb->channels[chan].in_use) return 0;
    return (pcb->channels[chan].flags & PCB_CHAN_MAILBOX) ? 1 : 0;
}

/*
 * vms$$chan_is_bg - Internal helper: is this channel an INET pseudo-device
 * (BGn:) channel (vms-527)? Used by sys$qio to route the $QIO functions to
 * the executive's BG driver (vms_kif_bg_*) instead of the fd-based path -- a
 * BG channel's fd is always -1, since the socket is executive-resident.
 */
int vms$$chan_is_bg(uint16_t chan) {
    if (chan == 0 || chan >= PCB_MAX_CHANNELS) return 0;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return 0;
    if (!pcb->channels[chan].in_use) return 0;
    return (pcb->channels[chan].flags & PCB_CHAN_BG) ? 1 : 0;
}
