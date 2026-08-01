// SPDX-License-Identifier: GPL-2.0
/*
 * vms_devtab.c - Executive-resident device table (vms-d0b)
 *
 * A VMS device is a thing the EXECUTIVE knows about. The driver enters
 * a unit in the I/O database at boot and from that moment the device
 * exists for every process on the node: $ASSIGN takes a channel to it,
 * $GETDVI reads its attributes, $DEVICE_SCAN enumerates it, and
 * SHOW DEVICE / SHOW TERMINAL are readers of that one table. Ownership,
 * reference count and terminal characteristics are properties of the
 * DEVICE, not of the process doing the asking -- which is why a VMS
 * terminal name means anything at all.
 *
 * A device table that lived in a process's own memory would pass every
 * single-process test and still be a facade (CLAUDE.md rule 11): the
 * decisive check is A-writes / B-reads, and it is what
 * tests/qemu/test_kmod_devtab.c does against a real /dev/vms.
 *
 * The console terminal OPA0: is created here, at module init. No
 * process registers it; a process that never asked for it still sees
 * it, exactly as on VMS where the terminal driver creates the console
 * unit during system initialization.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/cred.h>
#include <linux/uidgid.h>

#include "vms_internal.h"

/*
 * Device class codes. Values mirror src/libvms/include/dcdef.h so the
 * executive and the runtime cannot disagree about what a class means.
 */
#define DC__TERM        6   /* DC$_TERM */

/*
 * Device type codes: 0 is "Unknown".
 *
 * PROVENANCE (rule 10): the oracle displays an unidentified terminal
 * as "Device_Type: Unknown" -- observed by issuing
 * SET TERMINAL/DEVICE_TYPE=UNKNOWN on the ~/vax OpenVMS VAX V7.3 lab
 * console and reading SHOW TERMINAL back
 * (docs/oracle/vax73-terminal-device.md). OVMX's console is a serial
 * line whose terminal type is genuinely not identified -- the oracle's
 * own login procedure fails the same way on the same kind of console
 * ("%SET-W-NOTSET, error modifying OPA0: -SET-I-UNKTERM, unknown
 * terminal type") -- so Unknown is what is true here, not a
 * placeholder standing in for a value we could not find.
 */
#define VMS_DT_UNKNOWN  0

/* ================================================================
 * The table
 * ================================================================ */

static LIST_HEAD(vms_device_list);
static DEFINE_SPINLOCK(vms_device_list_lock);

/*
 * Console terminal defaults.
 *
 * PROVENANCE (rule 10, and flagged for operator sign-off per the
 * vms-purity-guardrail rule -- these are constants, and constants are
 * signed off, not self-certified):
 *
 *   - The name OPA0: is the OpenVMS console terminal, observed on both
 *     lab nodes (SHOW TERMINAL prints the physical form "_OPA0:").
 *   - The characteristic SET below is the set the oracle reports for a
 *     terminal whose device type is Unknown: Interactive, Echo,
 *     Type_ahead, TTsync, Lowercase, Wrap, Broadcast, Fulldup,
 *     Set_speed, Insert editing, Numeric Keypad, VMS Style Input --
 *     with everything else clear, notably No Line Editing, which the
 *     oracle clears when the device type becomes Unknown.
 *   - HARDCOPY is the one oracle-set bit deliberately NOT copied. On
 *     the lab the console is a physical LA36 printing terminal and the
 *     bit survives a device-type change because it describes that
 *     hardware. OVMX's console is not a printing terminal, so claiming
 *     Hardcopy would be a statement about our hardware that is false.
 *   - Width 132 / Page 24 are the pristine console values observed on
 *     lab node VAX2, which had never had SET TERMINAL issued on it.
 *     CAVEAT recorded honestly: that console is an LA36 (132-column
 *     paper), so this is the oracle's console default rather than a
 *     value derived from OVMX's own serial line, whose real geometry
 *     the executive cannot interrogate.
 */
#define VMS_CONSOLE_DEVNAM  "OPA0:"

#define VMS_CONSOLE_DEVCHAR (VMS_TTC_INTERACTIVE   | \
                             VMS_TTC_ECHO          | \
                             VMS_TTC_TYPEAHEAD     | \
                             VMS_TTC_TTSYNC        | \
                             VMS_TTC_LOWERCASE     | \
                             VMS_TTC_WRAP          | \
                             VMS_TTC_BROADCAST     | \
                             VMS_TTC_FULLDUP       | \
                             VMS_TTC_SET_SPEED     | \
                             VMS_TTC_INSERT_EDITING | \
                             VMS_TTC_NUMERIC_KEYPAD | \
                             VMS_TTC_VMS_STYLE_INPUT)

#define VMS_CONSOLE_WIDTH   132
#define VMS_CONSOLE_PAGE    24

/*
 * normalize_devnam - fold a caller-supplied device name into the
 * physical form the table is keyed by: upper case, exactly one
 * trailing colon, no leading underscore.
 *
 * Returns 0 on success, or a VMS status on a name that cannot be a
 * device name at all.
 */
static uint32_t normalize_devnam(const char *in, char *out, size_t outsz)
{
    size_t i, n = 0;

    if (!in || !out || outsz < 2)
        return SS__BADPARAM;

    /* The physical name form drops the leading underscore. */
    if (in[0] == '_')
        in++;

    for (i = 0; in[i] != '\0'; i++) {
        char c = in[i];

        if (i >= VMS_DEVNAM_SIZE)          /* unterminated / oversized */
            return SS__IVDEVNAM;
        if (c == ':') {
            if (in[i + 1] != '\0')          /* colon must be last */
                return SS__IVDEVNAM;
            break;
        }
        if (!isalnum((unsigned char)c) && c != '$')
            return SS__IVDEVNAM;
        if (n + 2 >= outsz)
            return SS__IVDEVNAM;
        out[n++] = (char)toupper((unsigned char)c);
    }

    if (n == 0)
        return SS__IVDEVNAM;

    out[n++] = ':';
    out[n] = '\0';
    return SS__NORMAL;
}

/* Caller holds vms_device_list_lock. */
static struct vms_device *devtab_lookup_locked(const char *devnam)
{
    struct vms_device *dev;

    list_for_each_entry(dev, &vms_device_list, list) {
        if (strcmp(dev->devnam, devnam) == 0)
            return dev;
    }
    return NULL;
}

/*
 * vms_devtab_create - enter a unit in the executive's device table.
 *
 * Called by the executive itself, never from an ioctl: on VMS units
 * are created by drivers during system initialization, not by user
 * processes asking for them.
 */
static struct vms_device *vms_devtab_create(const char *devnam,
                                            uint32_t devclass,
                                            uint32_t devtype,
                                            uint64_t devchar,
                                            uint32_t width, uint32_t page)
{
    struct vms_device *dev;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return NULL;

    strscpy(dev->devnam, devnam, sizeof(dev->devnam));
    dev->devclass = devclass;
    dev->devtype  = devtype;
    dev->devchar  = devchar;
    dev->width    = width;
    dev->page     = page;
    INIT_LIST_HEAD(&dev->chanlist);
    spin_lock_init(&dev->lock);

    spin_lock(&vms_device_list_lock);
    list_add_tail(&dev->list, &vms_device_list);
    spin_unlock(&vms_device_list_lock);

    return dev;
}

int vms_devtab_init(void)
{
    struct vms_device *console;

    console = vms_devtab_create(VMS_CONSOLE_DEVNAM, DC__TERM, VMS_DT_UNKNOWN,
                                VMS_CONSOLE_DEVCHAR,
                                VMS_CONSOLE_WIDTH, VMS_CONSOLE_PAGE);
    if (!console)
        return -ENOMEM;

    pr_info("vms: device table initialized, console terminal %s created\n",
            VMS_CONSOLE_DEVNAM);
    return 0;
}

void vms_devtab_cleanup(void)
{
    struct vms_device *dev, *tmp;

    spin_lock(&vms_device_list_lock);
    list_for_each_entry_safe(dev, tmp, &vms_device_list, list) {
        list_del(&dev->list);
        kfree(dev);
    }
    spin_unlock(&vms_device_list_lock);
}

/* ================================================================
 * Channels
 * ================================================================ */

/* Caller holds proc->chan_lock. */
static struct vms_channel *chan_find_locked(struct vms_proc *proc, uint32_t chan)
{
    struct vms_channel *ch;

    list_for_each_entry(ch, &proc->channels, list) {
        if (ch->chan == chan)
            return ch;
    }
    return NULL;
}

/* Caller holds dev->lock. Clear an outstanding allocation. */
static void device_dealloc_locked(struct vms_device *dev)
{
    if (!dev->allocated)
        return;
    dev->allocated = 0;
    dev->owner_pid = 0;
    dev->owner_linux_pid = 0;
    dev->owner_uic = 0;
    if (dev->refcnt > 0)
        dev->refcnt--;
}

/*
 * Give a channel back: unlink it from the device and drop the
 * reference it held.
 *
 * Note what this does NOT do: it does not touch ownership. Ownership
 * is allocation, and allocation is released by $DALLOC or by the
 * owner's death -- not by handing back a channel. That split is the
 * oracle's, not a convenience: on the lab a process holding an open
 * channel never became the device's owner in the first place.
 */
static void device_release_channel(struct vms_channel *ch)
{
    struct vms_device *dev = ch->dev;

    spin_lock(&dev->lock);
    list_del(&ch->devlink);
    if (dev->refcnt > 0)
        dev->refcnt--;
    spin_unlock(&dev->lock);
}

/*
 * Release everything a dying process held: its channels, and any
 * device it had allocated. A device allocated by a process that no
 * longer exists would be permanently unallocatable, which is not a
 * state VMS has.
 */
void vms_proc_release_channels(struct vms_proc *proc)
{
    struct vms_channel *ch, *tmp;
    struct vms_device *dev;
    LIST_HEAD(doomed);

    spin_lock(&proc->chan_lock);
    list_for_each_entry_safe(ch, tmp, &proc->channels, list)
        list_move(&ch->list, &doomed);
    spin_unlock(&proc->chan_lock);

    list_for_each_entry_safe(ch, tmp, &doomed, list) {
        list_del(&ch->list);
        device_release_channel(ch);
        kfree(ch);
    }

    spin_lock(&vms_device_list_lock);
    list_for_each_entry(dev, &vms_device_list, list) {
        spin_lock(&dev->lock);
        if (dev->allocated && dev->owner_linux_pid == proc->linux_pid)
            device_dealloc_locked(dev);
        spin_unlock(&dev->lock);
    }
    spin_unlock(&vms_device_list_lock);
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

/*
 * $ASSIGN - take a channel to a device.
 *
 * ORACLE-PINNED SEMANTICS (docs/oracle/vax73-terminal-device.md
 * section 7), because the obvious guesses are both wrong:
 *
 *   - Assigning a channel does NOT make the caller the device's
 *     owner. On the lab an open channel to NLA0: left "Owner process"
 *     empty and "Owner process ID" 00000000 for as long as it was
 *     held. Ownership is $ALLOC's job.
 *
 *   - $ASSIGN to a device another process owns SUCCEEDS. A detached
 *     process on the lab assigned a channel to OPA0: -- the console,
 *     owned by the interactive job -- and got
 *     %SYSTEM-S-NORMAL. SS$_DEVALLOC is what $ALLOC returns in that
 *     situation, not $ASSIGN; the same detached process got
 *     %SYSTEM-W-DEVALLOC from ALLOCATE OPA0: seconds later.
 *
 * NOT MODELLED, deliberately (rule 10 -- do not invent a handler for
 * a condition we cannot pin): the lab's OPA0: carries a device
 * protection mask of S:RWPL,O:RWPL,G,W, so a process outside the
 * system UIC group would be refused by protection, not by allocation.
 * The probe ran as SYSTEM, so it pins the allocated-device case and
 * says nothing about the unprivileged case. OVMX has no device
 * protection to check yet and therefore does not check one.
 */
long vms_ioctl_assign(struct vms_proc *proc, unsigned long arg)
{
    struct vms_assign_args args;
    struct vms_channel *ch;
    struct vms_device *dev;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.chan = 0;
        args.status = status;
        goto out;
    }

    ch = kzalloc(sizeof(*ch), GFP_KERNEL);
    if (!ch)
        return -ENOMEM;

    spin_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        spin_unlock(&vms_device_list_lock);
        kfree(ch);
        args.chan = 0;
        args.status = SS__NOSUCHDEV;
        goto out;
    }

    ch->dev = dev;
    ch->owner_linux_pid = proc->linux_pid;

    spin_lock(&dev->lock);
    dev->refcnt++;
    list_add_tail(&ch->devlink, &dev->chanlist);
    spin_unlock(&dev->lock);
    spin_unlock(&vms_device_list_lock);

    spin_lock(&proc->chan_lock);
    /*
     * Channel numbers are opaque to the caller on VMS ("the system
     * assigns the channel"), so the allocation policy below -- small
     * ascending non-zero integers, never reused within a process -- is
     * an OVMX design choice and is not claimed to match VMS's.
     */
    ch->chan = ++proc->next_chan;
    list_add_tail(&ch->list, &proc->channels);
    spin_unlock(&proc->chan_lock);

    args.chan = ch->chan;
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

long vms_ioctl_dassgn(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dassgn_args args;
    struct vms_channel *ch;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->chan_lock);
    ch = chan_find_locked(proc, args.chan);
    if (ch)
        list_del(&ch->list);
    spin_unlock(&proc->chan_lock);

    if (!ch) {
        args.status = SS__IVCHAN;
    } else {
        device_release_channel(ch);
        kfree(ch);
        args.status = SS__NORMAL;
    }

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $ALLOC - allocate a device to this process.
 *
 * This, not $ASSIGN, is what makes a process a device's owner. Every
 * branch below is a case observed on the ~/vax OpenVMS VAX V7.3 lab
 * (docs/oracle/vax73-terminal-device.md sections 7-9):
 *
 *   allocated to another process   -> %SYSTEM-W-DEVALLOC
 *       ALLOCATE OPA0: from a detached process while the interactive
 *       job held the console.
 *   another process holds channels -> %SYSTEM-W-DEVALLOC
 *       ALLOCATE NLA0: with "Owner process" empty and a reference
 *       count of 2 -- foreign channels alone are enough.
 *   already allocated to us        -> success, reference count unchanged
 *       ALLOCATE OPA0: twice in a row: 2 -> 3 -> 3.
 *   otherwise                      -> success, reference count + 1
 *
 * The caller's UIC is taken from its Linux credentials. That mapping
 * is NOT oracle-pinned and is noted as such in vms-d0b's findings; it
 * will have to agree with whatever replaces the VMS_UIC_* environment
 * facade (vms-2b8).
 */
long vms_ioctl_alloc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_alloc_args args;
    struct vms_device *dev;
    struct vms_channel *ch;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;
    int foreign = 0;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    spin_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        spin_unlock(&vms_device_list_lock);
        args.status = SS__NOSUCHDEV;
        goto out;
    }

    spin_lock(&dev->lock);
    if (dev->allocated && dev->owner_linux_pid == proc->linux_pid) {
        args.status = SS__NORMAL;            /* already ours; idempotent */
    } else if (dev->allocated) {
        args.status = SS__DEVALLOC;
    } else {
        list_for_each_entry(ch, &dev->chanlist, devlink) {
            if (ch->owner_linux_pid != proc->linux_pid) {
                foreign = 1;
                break;
            }
        }
        if (foreign) {
            args.status = SS__DEVALLOC;
        } else {
            dev->allocated = 1;
            dev->owner_pid = proc->vms_pid;
            dev->owner_linux_pid = proc->linux_pid;
            dev->owner_uic =
                (((uint32_t)from_kgid(&init_user_ns, current_gid()) & 0xFFFFu) << 16) |
                ((uint32_t)from_kuid(&init_user_ns, current_uid()) & 0xFFFFu);
            dev->refcnt++;
            args.status = SS__NORMAL;
        }
    }
    spin_unlock(&dev->lock);
    spin_unlock(&vms_device_list_lock);

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $DALLOC - give an allocated device back.
 *
 * Deallocating a device this process does not have allocated is
 * %SYSTEM-W-DEVNOTALLOC on the lab (a second DEALLOCATE OPA0: right
 * after the first).
 */
long vms_ioctl_dalloc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_alloc_args args;
    struct vms_device *dev;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    spin_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        spin_unlock(&vms_device_list_lock);
        args.status = SS__NOSUCHDEV;
        goto out;
    }

    spin_lock(&dev->lock);
    if (dev->allocated && dev->owner_linux_pid == proc->linux_pid) {
        device_dealloc_locked(dev);
        args.status = SS__NORMAL;
    } else {
        args.status = SS__DEVNOTALLOC;
    }
    spin_unlock(&dev->lock);
    spin_unlock(&vms_device_list_lock);

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/* Snapshot a device row for userspace. Takes dev->lock. */
static void devinfo_fill(struct vms_device *dev, struct vms_devinfo *info)
{
    memset(info, 0, sizeof(*info));

    spin_lock(&dev->lock);
    strscpy(info->devnam, dev->devnam, sizeof(info->devnam));
    info->devclass  = dev->devclass;
    info->devtype   = dev->devtype;
    info->owner_pid = dev->owner_pid;
    info->owner_uic = dev->owner_uic;
    info->allocated = dev->allocated;
    info->refcnt    = dev->refcnt;
    info->errcnt    = dev->errcnt;
    info->opcnt     = dev->opcnt;
    info->devchar   = dev->devchar;
    info->width     = dev->width;
    info->page      = dev->page;
    spin_unlock(&dev->lock);
}

long vms_ioctl_getdvi(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getdvi_args args;
    struct vms_device *dev = NULL;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (args.select == VMS_DVI_SEL_CHAN) {
        struct vms_channel *ch;

        spin_lock(&proc->chan_lock);
        ch = chan_find_locked(proc, args.chan);
        if (ch)
            dev = ch->dev;
        spin_unlock(&proc->chan_lock);

        if (!dev) {
            memset(&args.info, 0, sizeof(args.info));
            args.status = SS__IVCHAN;
            goto out;
        }
        devinfo_fill(dev, &args.info);
        args.status = SS__NORMAL;
        goto out;
    }

    if (args.select != VMS_DVI_SEL_DEVNAM) {
        args.status = SS__BADPARAM;
        goto out;
    }

    args.info.devnam[VMS_DEVNAM_SIZE - 1] = '\0';
    status = normalize_devnam(args.info.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        memset(&args.info, 0, sizeof(args.info));
        args.status = status;
        goto out;
    }

    spin_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (dev)
        devinfo_fill(dev, &args.info);
    spin_unlock(&vms_device_list_lock);

    if (!dev) {
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NOSUCHDEV;
    } else {
        args.status = SS__NORMAL;
    }

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

long vms_ioctl_devscan(struct vms_proc *proc, unsigned long arg)
{
    struct vms_devscan_args args;
    struct vms_device *dev, *found = NULL;
    uint32_t i = 0;

    (void)proc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&vms_device_list_lock);
    list_for_each_entry(dev, &vms_device_list, list) {
        if (i == args.index) {
            found = dev;
            break;
        }
        i++;
    }
    if (found)
        devinfo_fill(found, &args.info);
    spin_unlock(&vms_device_list_lock);

    if (!found) {
        args.status = SS__NOMOREDEV;
    } else {
        args.index++;
        args.status = SS__NORMAL;
    }

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

long vms_ioctl_ttsetmode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_setmode_args args;
    struct vms_channel *ch;
    struct vms_device *dev = NULL;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    /*
     * Characteristics are set THROUGH A CHANNEL, as VMS sets them
     * ($QIO IO$_SETMODE). No channel, no change -- a process cannot
     * redefine a terminal it never opened.
     */
    spin_lock(&proc->chan_lock);
    ch = chan_find_locked(proc, args.chan);
    if (ch)
        dev = ch->dev;
    spin_unlock(&proc->chan_lock);

    if (!dev) {
        args.status = SS__IVCHAN;
        goto out;
    }

    if (dev->devclass != DC__TERM) {
        /* IO$_SETMODE terminal function on a non-terminal device. */
        args.status = SS__IVDEVNAM;
        goto out;
    }

    spin_lock(&dev->lock);
    if (args.flags & VMS_TTSET_CHAR) {
        dev->devchar |= args.setchar;
        dev->devchar &= ~args.clrchar;
    }
    if (args.flags & VMS_TTSET_WIDTH)
        dev->width = args.width;
    if (args.flags & VMS_TTSET_PAGE)
        dev->page = args.page;
    dev->opcnt++;
    spin_unlock(&dev->lock);

    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
