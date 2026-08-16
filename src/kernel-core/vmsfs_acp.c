// SPDX-License-Identifier: GPL-2.0
/*
 * vmsfs_acp.c - Files-11 (ODS-2) ACP: channel + mount front-end (vms-149,
 *               epic vms-208, second rung after the kernel-resident codec vms-dcd)
 *
 * See vms_acp.h for the full rationale and the clean-room posture, and
 * docs/design-files11-acp-executive.md §4.2/§4.3 for the design. In one line:
 * on real OpenVMS a file operation is a $QIO on a channel $ASSIGNed to the
 * mounted volume's device, serviced by the XQP in the caller's context; OVMX
 * already has $ASSIGN/$QIO/the FIB scaffolding but no ACP arm, and this file
 * adds the CHANNEL half of that arm -- an executive-global mounted-volume table
 * and a FILE-CLASS channel bound to a mounted volume -- so a later rung can
 * carry the ACP-QIO file operations (IO$_ACCESS/READVBLK/...) over it.
 *
 * WHAT THIS RUNG DOES AND DOES NOT DO. It establishes:
 *   - the executive-global mounted-volume table (design §4.3): a $MOUNT records
 *     a unit as an ODS-2 volume EVERY process then sees -- real shared executive
 *     state, not a per-process fake (CLAUDE.md Rule 9 / INV-6);
 *   - a file-class channel: $ASSIGN of a mounted unit returns an executive
 *     channel bound to the volume (not a Linux fd), released by the ordinary
 *     $DASSGN (VMS_IOCTL_DASSGN) exactly as a mailbox channel is.
 * It does NOT open the backing block device, parse the home block/SCB, or carry
 * any ACP-QIO file operation -- those are the later rungs of epic vms-208
 * (vms-204/vms-c60/vms-5303). Fail-honest throughout: an $ASSIGN of a unit that
 * is not a mounted volume returns SS$_NOSUCHDEV, never a fabricated channel.
 *
 * RESIDENCY / SUBSTRATE. Lives in the shared substrate-agnostic core
 * (src/kernel-core/), like vms_mbx.c / vms_lock.c, so it is inherited by both
 * SYSKRNLs the design targets; it touches the host only through the exec_*
 * shim contracts (exec_kbackend.h) and exec_list_* (exec_list.h), with no
 * <linux/...> header of its own. It follows the SAME channel pattern mailboxes
 * use: a channel that is not a struct vms_device row gets its own small binding
 * (struct vms_acp_chan) in its own per-process list (struct vms_proc::
 * file_channels), drawing its channel NUMBER from the same proc->next_chan
 * space, with $DASSGN (vms_devtab.c's vms_ioctl_dassgn) falling back to
 * vms_acp_dassgn() below.
 */

#include "vms_internal.h"
#include "exec_kbackend.h"
#include "exec_list.h"

/*
 * One executive-resident mounted ODS-2 volume. THIS RUNG carries only the
 * unit name and the assigned-channel refcount; the backing block device, the
 * validated home block/SCB and the codec volume handle are added by the full
 * $MOUNT rung (design §4.3, §4.4).
 */
struct vms_acp_volume {
    exec_list_node_t list;              /* in vms_acp_vol_list */
    char             devnam[VMS_DEVNAM_SIZE]; /* canonical unit name, e.g. "DKA0:" */
    uint32_t         refcnt;            /* file-class channels assigned, any process */
};

/* One process's file-class channel to a mounted volume. */
struct vms_acp_chan {
    exec_list_node_t list;              /* in proc->file_channels */
    uint32_t         chan;
    struct vms_acp_volume *vol;
};

/*
 * The executive-global mounted-volume table. A single lock guards both the
 * list and every volume's refcnt: the table is small (a handful of mounted
 * units) and refcnt is only touched on $ASSIGN/$DASSGN/$DISMOUNT, so a
 * per-volume lock would buy nothing. Initialized at module load in
 * vms_acp_init() (NOT a static-initializer macro -- a NetBSD kmutex cannot be
 * statically initialized; the same choice vms_mbx.c/vms_eflag.c make).
 */
EXEC_LIST_HEAD(vms_acp_vol_list);
exec_lock_t vms_acp_vol_lock;

void vms_acp_init(void)
{
    exec_lock_init(&vms_acp_vol_lock);
    pr_info("vms: ODS-2 ACP mounted-volume table initialized\n");
}

void vms_acp_cleanup(void)
{
    struct vms_acp_volume *vol, *tmp;

    exec_lock(&vms_acp_vol_lock);
    exec_list_for_each_entry_safe(vol, tmp, &vms_acp_vol_list, list) {
        exec_list_del(&vol->list);
        exec_free(vol);
    }
    exec_unlock(&vms_acp_vol_lock);
    exec_lock_destroy(&vms_acp_vol_lock);
}

/*
 * acp_normalize_devnam - fold a caller-supplied unit name into a canonical
 * upper-case "ddcu:" form (leading physical-name '_' stripped, exactly one
 * trailing colon). Rejects an empty or over-length name. Broader than
 * vms_mbx.c's MBA-only normalizer because an ACP volume can be any disk unit
 * (DKA0:, DSA0:, the discovered SYS$SYSDEVICE unit, ...), so this validates the
 * general device-name SHAPE rather than a fixed prefix.
 */
static uint32_t acp_normalize_devnam(const char *in, char *out, size_t outsz)
{
    size_t i, n = 0;

    if (!in || !out || outsz < 2)
        return SS__BADPARAM;

    if (in[0] == '_')                   /* physical-name underscore */
        in++;

    for (i = 0; in[i] != '\0'; i++) {
        char c = in[i];

        if (c == ':') {
            if (in[i + 1] != '\0')      /* nothing may follow the colon */
                return SS__IVDEVNAM;
            break;
        }
        /* A device name is ddcu: -- letters, digits and '$' (e.g. DSA0:,
         * SYS$SYSDEVICE). Anything else is not a device name at all. */
        if (!isalnum((unsigned char)c) && c != '$')
            return SS__IVDEVNAM;
        if (n + 2 >= outsz)             /* leave room for ':' and NUL */
            return SS__IVDEVNAM;
        out[n++] = (char)toupper((unsigned char)c);
    }

    if (n == 0)
        return SS__IVDEVNAM;

    out[n++] = ':';
    out[n] = '\0';
    return SS__NORMAL;
}

/* Caller holds vms_acp_vol_lock. */
static struct vms_acp_volume *acp_vol_find_locked(const char *devnam)
{
    struct vms_acp_volume *vol;

    exec_list_for_each_entry(vol, &vms_acp_vol_list, list) {
        if (strcmp(vol->devnam, devnam) == 0)
            return vol;
    }
    return NULL;
}

/* Caller holds proc->chan_lock. */
static struct vms_acp_chan *acp_chan_find_locked(struct vms_proc *proc, uint32_t chan)
{
    struct vms_acp_chan *ch;

    exec_list_for_each_entry(ch, &proc->file_channels, list) {
        if (ch->chan == chan)
            return ch;
    }
    return NULL;
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

/*
 * $MOUNT an ODS-2 volume into the executive-global table (design §4.3). Records
 * the unit as a mounted volume that every process then sees -- the shared
 * executive state that replaces the userspace adapter's per-process passthrough.
 * Idempotent: mounting a unit already in the table returns SS$_NORMAL (the
 * volume IS mounted), rather than inventing an "already mounted" condition this
 * tree cannot oracle-pin.
 *
 * PRIVILEGE. Real $MOUNT of a system volume is privileged; this rung does NOT
 * gate the mount on a privilege it cannot yet enforce faithfully -- that check
 * (VOLPRO / mount privileges) lands with the full $MOUNT rung that also binds
 * the backing device and validates the home block. Stated rather than silently
 * omitted (CLAUDE.md Rule 10): the mount table is a foundation the later rung
 * hardens, not a security control claimed here.
 */
long vms_ioctl_acp_mount(struct vms_proc *proc, unsigned long arg)
{
    struct vms_acp_mount_args args;
    struct vms_acp_volume *vol;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    (void)proc;
    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = acp_normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    /* Allocate before taking the table lock -- exec_zalloc may sleep. */
    vol = exec_zalloc(sizeof(*vol));
    if (!vol)
        return -ENOMEM;

    exec_lock(&vms_acp_vol_lock);
    if (acp_vol_find_locked(devnam)) {
        exec_unlock(&vms_acp_vol_lock);
        exec_free(vol);                 /* already mounted -- idempotent */
        args.status = SS__NORMAL;
        goto out;
    }
    strscpy(vol->devnam, devnam, sizeof(vol->devnam));
    vol->refcnt = 0;
    exec_list_add_tail(&vol->list, &vms_acp_vol_list);
    exec_unlock(&vms_acp_vol_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $DISMOUNT: remove a volume from the executive-global table. SS$_NOSUCHDEV if
 * it is not mounted. Refused SS$_DEVALLOC while any file-class channel is still
 * assigned to it -- a channel bound to a volume must never point at freed
 * storage, so the volume cannot be dismounted out from under it. (SS$_DEVALLOC
 * for "busy with assigned channels" is an OVMX choice of an already-pinned
 * status, not an oracle-pinned $DISMOUNT semantic -- labelled as such.)
 */
long vms_ioctl_acp_dmount(struct vms_proc *proc, unsigned long arg)
{
    struct vms_acp_dmount_args args;
    struct vms_acp_volume *vol;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    (void)proc;
    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = acp_normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    exec_lock(&vms_acp_vol_lock);
    vol = acp_vol_find_locked(devnam);
    if (!vol) {
        exec_unlock(&vms_acp_vol_lock);
        args.status = SS__NOSUCHDEV;
        goto out;
    }
    if (vol->refcnt > 0) {
        exec_unlock(&vms_acp_vol_lock);
        args.status = SS__DEVALLOC;     /* channels still assigned -- busy */
        goto out;
    }
    exec_list_del(&vol->list);
    exec_unlock(&vms_acp_vol_lock);
    exec_free(vol);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $ASSIGN a FILE-CLASS channel to a mounted ODS-2 volume (design §4.2). The
 * channel is the executive's -- bound to the mounted volume, drawn from the
 * caller's proc->next_chan space -- not a Linux fd. SS$_NOSUCHDEV when the unit
 * is not a mounted volume: the fail-honest answer, never a fabricated channel
 * to a volume the executive does not have (CLAUDE.md Rule 9 / INV-6). $DASSGN
 * releases it (vms_acp_dassgn(), reached from vms_ioctl_dassgn's fallback).
 */
long vms_ioctl_acp_assign(struct vms_proc *proc, unsigned long arg)
{
    struct vms_acp_assign_args args;
    struct vms_acp_volume *vol;
    struct vms_acp_chan *ch;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = acp_normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    /* Allocate before taking any lock -- exec_zalloc may sleep. */
    ch = exec_zalloc(sizeof(*ch));
    if (!ch)
        return -ENOMEM;

    exec_lock(&vms_acp_vol_lock);
    vol = acp_vol_find_locked(devnam);
    if (!vol) {
        exec_unlock(&vms_acp_vol_lock);
        exec_free(ch);
        args.status = SS__NOSUCHDEV;    /* not a mounted volume -- fail honest */
        goto out;
    }
    vol->refcnt++;
    exec_unlock(&vms_acp_vol_lock);

    ch->vol = vol;
    exec_lock(&proc->chan_lock);
    ch->chan = ++proc->next_chan;
    exec_list_add_tail(&ch->list, &proc->file_channels);
    exec_unlock(&proc->chan_lock);

    args.chan = ch->chan;
    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/* ================================================================
 * $DASSGN fallback and process teardown -- called from vms_devtab.c
 * ================================================================ */

/*
 * Release one file-class channel by number, for vms_ioctl_dassgn()'s fallback
 * when `chan` is neither a device channel nor a mailbox channel. Returns 0 if
 * `chan` named a file channel (released, and its volume's refcnt dropped), or
 * -ENOENT if it did not (so the generic $DASSGN can report SS$_IVCHAN itself).
 */
int vms_acp_dassgn(struct vms_proc *proc, uint32_t chan)
{
    struct vms_acp_chan *ch;

    exec_lock(&proc->chan_lock);
    ch = acp_chan_find_locked(proc, chan);
    if (ch)
        exec_list_del(&ch->list);
    exec_unlock(&proc->chan_lock);

    if (!ch)
        return -ENOENT;

    exec_lock(&vms_acp_vol_lock);
    if (ch->vol->refcnt > 0)
        ch->vol->refcnt--;
    exec_unlock(&vms_acp_vol_lock);

    exec_free(ch);
    return 0;
}

/*
 * Give back every file-class channel a dying process holds (process teardown),
 * dropping each volume's refcnt so a later $DISMOUNT is not blocked by a channel
 * whose owner no longer exists -- exactly as vms_mbx_release_all() does for
 * mailbox channels.
 */
void vms_acp_release_all(struct vms_proc *proc)
{
    struct vms_acp_chan *ch, *tmp;
    EXEC_LIST_HEAD(doomed);

    exec_lock(&proc->chan_lock);
    exec_list_for_each_entry_safe(ch, tmp, &proc->file_channels, list)
        exec_list_move(&ch->list, &doomed);
    exec_unlock(&proc->chan_lock);

    exec_list_for_each_entry_safe(ch, tmp, &doomed, list) {
        exec_list_del(&ch->list);
        exec_lock(&vms_acp_vol_lock);
        if (ch->vol->refcnt > 0)
            ch->vol->refcnt--;
        exec_unlock(&vms_acp_vol_lock);
        exec_free(ch);
    }
}
