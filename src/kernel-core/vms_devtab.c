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

/*
 * SUBSTRATE-AGNOSTIC EXECUTIVE CORE (rd vms-31b, epic vms-8e8 -- the device
 * table, the facility Phase E deferred until its two missing seams landed). This
 * file lives in src/kernel-core/ and names NO <linux/...> symbol: every host
 * primitive it needs goes through the kernel-backend shim. Phase E could not
 * move it because three seams were missing -- the BLOCK LAYER (lookup_bdev /
 * MAJOR / MINOR / dev_t, the disk-unit enumeration), HOST-TASK CREDENTIALS (the
 * caller's uid/gid, for caller_uic), and the cross-facility vms_proc_hash_lock
 * (setterm writes proc->terminal under it). Phase F landed the host-task seam
 * (exec_current_*) and the process-table lock; vms-31b adds the last missing
 * piece, the BLOCK-DEVICE seam (exec_blockdev_*, exec_kbackend.h §8), and
 * promotes the facility. The Linux vms.ko provides the backend
 * (exec_kbackend_linux.h / exec_list_linux.h); the NetBSD `vms' module will
 * provide its own -- including the block-device path->dev_t mapping -- when
 * devtab joins its SRCS (a later item; the NetBSD block seam is contract-only
 * today, following the exec_rbtree precedent).
 */
#include "vms_internal.h"     /* struct vms_device/vms_channel/vms_proc, the SS$/
                              * args/status codes, vms_proc_hash_lock (extern),
                              * the C-string + ctype + fixed-width vocabulary */
#include "exec_kbackend.h"    /* exec_lock/copy/alloc/current/blockdev */
#include "exec_list.h"        /* exec_list_* (device list, channel lists) */

/*
 * Device class codes. Values mirror src/libvms/include/dcdef.h so the
 * executive and the runtime cannot disagree about what a class means.
 */
#define DC__TERM        6   /* DC$_TERM */
#define DC__DISK        1   /* DC$_DISK */
#define DC__SCOM        3   /* DC$_SCOM -- serial-communications / LAN class */

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

/*
 * The device list anchor is statically empty (EXEC_LIST_HEAD, portable). Its
 * guard is a plain exec_lock_t initialized at module load in vms_devtab_init()
 * below, NOT statically: the NetBSD backend's kmutex cannot be statically
 * initialized, so runtime exec_lock_init() is the substrate-agnostic form. On
 * Linux it forwards to spin_lock_init, so behaviour is unchanged.
 */
static EXEC_LIST_HEAD(vms_device_list);
static exec_lock_t vms_device_list_lock;

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

    exec_list_for_each_entry(dev, &vms_device_list, list) {
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
                                            uint32_t shareable,
                                            uint64_t devchar,
                                            uint32_t width, uint32_t page)
{
    struct vms_device *dev;

    dev = exec_zalloc(sizeof(*dev));
    if (!dev)
        return NULL;

    strscpy(dev->devnam, devnam, sizeof(dev->devnam));
    dev->devclass  = devclass;
    dev->devtype   = devtype;
    dev->shareable = shareable;
    dev->devchar   = devchar;
    dev->width     = width;
    dev->page      = page;
    exec_list_head_init(&dev->chanlist);
    exec_lock_init(&dev->lock);

    exec_lock(&vms_device_list_lock);
    exec_list_add_tail(&dev->list, &vms_device_list);
    exec_unlock(&vms_device_list_lock);

    return dev;
}

/*
 * Disk unit naming (vms-3e8).
 *
 * PROVENANCE (CLAUDE.md Rule 8, published-doc-derived). A VMS device name is
 * "ddcu:" -- a two-letter device code, a controller letter, and a unit number
 * (VSI OpenVMS I/O User's Reference Manual; OpenVMS User's Manual, "Devices").
 * DK is the device code for a direct-access DISK on a generic/SCSI controller,
 * A is the first controller, and for such disks the unit number encodes the
 * SCSI target as target*100 (+LUN) -- so the first target is DKA0:, the second
 * DKA100:, the third DKA200:, which is the numbering documented for SCSI disks
 * and observed on SCSI-based OpenVMS systems. None of this is copied from VSI
 * source; it is the published external naming convention.
 *
 * WHAT IS AN OVMX DESIGN CHOICE, labelled as such (Rule 8): virtio-blk has no
 * SCSI target, so mapping "the Nth virtio block device (vd[a-z]) to SCSI target
 * N" -- vda->DKA0:, vdb->DKA100:, vdc->DKA200: -- is OVMX's, not VMS's. It is
 * the natural positional mapping, and it is stable (a device's letter fixes its
 * unit), but it is not presented as VMS-authentic.
 *
 * ENUMERATION. The block layer exports no module-callable iterator over its
 * gendisks, so the executive enumerates the virtio-blk NAME SPACE it can name:
 * it probes /dev/vda../dev/vdz with lookup_bdev(), and creates a unit ONLY for
 * a name that resolves to a real block device. A gap in the name space is a gap
 * in the unit space -- no unit is invented for a device that is not there
 * (INV-6: the executive reports what it knows, never a plausible fake).
 */
#define VMS_DISK_UNITS      26          /* vda .. vdz */

static void vms_devtab_probe_disks(void)
{
    int i;

    for (i = 0; i < VMS_DISK_UNITS; i++) {
        char path[16];
        char devnam[VMS_DEVNAM_SIZE];
        char backing[VMS_BACKING_SIZE];
        struct vms_device *disk;
        exec_dev_t dev;
        int n;

        n = snprintf(path, sizeof(path), "/dev/vd%c", 'a' + i);
        if (n < 0 || n >= (int)sizeof(path))
            continue;

        /*
         * exec_blockdev_lookup() resolves the /dev node to an exec_dev_t without
         * opening the device (Linux: lookup_bdev). A nonzero return here is the
         * ordinary case (this letter has no disk); it is not an error, just the
         * end of the contiguous run.
         */
        if (exec_blockdev_lookup(path, &dev) != 0)
            continue;

        snprintf(devnam,  sizeof(devnam),  "DKA%u:", (unsigned)i * 100u);
        snprintf(backing, sizeof(backing), "vd%c", 'a' + i);

        /*
         * A disk is shareable=0 here for the same honest reason the console is
         * (section 7.3): no OVMX test exercises a shareable disk's ownership
         * yet, and claiming shareable=1 without an assertion behind it would be
         * the unmeasured claim the console's comment warns against. Filled in
         * when a disk ownership test lands.
         */
        disk = vms_devtab_create(devnam, DC__DISK, VMS_DT_UNKNOWN,
                                 0 /* shareable */, 0 /* devchar */,
                                 0 /* width */, 0 /* page */);
        if (!disk) {
            pr_warn("vms: out of memory creating disk unit %s (%s)\n",
                    devnam, backing);
            return;
        }

        /*
         * Set the backing under dev->lock for form, though nothing else runs
         * yet: this is module init, before misc_register() creates /dev/vms,
         * so no process can be reading the table. The lock keeps the write
         * paired with vms_ioctl_disk_resolve()'s read, which does take it.
         */
        exec_lock(&disk->lock);
        strscpy(disk->backing, backing, sizeof(disk->backing));
        disk->backing_major = exec_blockdev_major(dev);
        disk->backing_minor = exec_blockdev_minor(dev);
        exec_unlock(&disk->lock);

        pr_info("vms: disk unit %s -> %s (%u:%u)\n",
                devnam, backing, exec_blockdev_major(dev), exec_blockdev_minor(dev));
    }
}

/*
 * vms_devtab_add_disk - enter ONE disk unit that the substrate, not the shared
 * probe above, enumerated (rd vms-618).
 *
 * WHY. vms_devtab_probe_disks() enumerates the NAME SPACE Linux's virtio-blk
 * driver uses (/dev/vda../dev/vdz). That name space does not exist on every
 * substrate: on NetBSD/vax the node's disks are MSCP units (/dev/ra1c,
 * /dev/ra2c under SIMH), and the executive's device-native unit map lives in
 * that substrate's block backend (src/kernel-netbsd/vms_blockdev_netbsd.c,
 * vms-47d). So the shared probe finds nothing there and the substrate enters
 * its own units through this one entry point -- which is what VMS does anyway:
 * "the DRIVER enters a unit in the I/O database at boot".
 *
 * Everything a device MEANS still lives here: the row, its ownership,
 * allocation and reference count are this facility's, identical on every
 * substrate. Only WHICH units exist is the substrate's to say (INV-6: a unit is
 * entered only for a device that really resolved -- see the caller).
 *
 * Called from module init only, before /dev/vms exists, so no process can be
 * reading the table. Returns 0, or -ENOMEM if the row could not be allocated.
 */
int vms_devtab_add_disk(const char *devnam, const char *backing,
                        uint32_t backing_major, uint32_t backing_minor)
{
    struct vms_device *disk;

    if (!devnam || !backing)
        return -EINVAL;

    /*
     * shareable = 0 for the same honest reason the console and the probed
     * Linux disks are (see vms_devtab_probe_disks above): no OVMX test
     * exercises a shareable disk's ownership yet, and claiming shareable = 1
     * without an assertion behind it would be an unmeasured claim.
     */
    disk = vms_devtab_create(devnam, DC__DISK, VMS_DT_UNKNOWN,
                             0 /* shareable */, 0 /* devchar */,
                             0 /* width */, 0 /* page */);
    if (!disk) {
        pr_warn("vms: out of memory creating disk unit %s (%s)\n",
                devnam, backing);
        return -ENOMEM;
    }

    exec_lock(&disk->lock);
    strscpy(disk->backing, backing, sizeof(disk->backing));
    disk->backing_major = backing_major;
    disk->backing_minor = backing_minor;
    exec_unlock(&disk->lock);

    pr_info("vms: disk unit %s -> %s (%u:%u)\n",
            devnam, backing, (unsigned)backing_major, (unsigned)backing_minor);
    return 0;
}

/*
 * The NIC as a VMS device (vms-9d2, epic vms-67f L0 -- the device face the
 * TCP/IP and DECnet stacks layer over; design docs/design-tcpip-services-ovmx.md
 * §4 "L0 NIC as VMS device").
 *
 * THE VMS-VISIBLE NAME IS ONE CONSTANT, ON PURPOSE (operator, 2026-08-14). The
 * name the executive enters this unit under is defined HERE and nowhere else:
 * the device is BORN in this table, and SHOW DEVICE / $ASSIGN / $GETDVI are all
 * readers that operate on whatever name the table holds, never on a literal of
 * their own. So the naming decision is a one-line change to VMS_NIC_DEVNAM
 * below; nothing downstream re-encodes the name. The tests that query it carry a
 * matching constant pointing back here.
 *
 * NAMING RULING -- device-native-naming (operator 2026-08-14). OVMX device
 * names TRACK THE NATIVE KERNEL device name of the substrate, not the VMS
 * driver-letter convention (so this is NOT presented as VMS-authentic -- Rule
 * 8, an OVMX design choice, labelled). On the Linux substrate the primary
 * Ethernet net device roots at "eth", so the VMS-visible name is ETH0:: the
 * native "eth" root, unit 0, rendered colon-terminated VMS-style. (This
 * supersedes the VMS "EWn:"/"EZn:" LAN-controller naming the design's §4 first
 * proposed.) The constant is FIXED to ETH0: rather than derived per-interface:
 * the actual host interface (which may be eth0 or a predictable-naming
 * enp0s1) is recorded privately in dev->netif and is never the VMS name --
 * ETH0: is the stable native-rooted face regardless.
 *
 * PROVENANCE for the remaining attributes (CLAUDE.md Rule 8, published-doc-
 * derived; flagged for operator sign-off per the vms-purity-guardrail rule --
 * these are constants):
 *
 *   - Device class DC$_SCOM (serial-communications device). OpenVMS LAN/Ethernet
 *     controllers (EWA0:, XQA0:, ...) report DVI$_DEVCLASS = DC$_SCOM -- the
 *     device class name is always DC$_SCOM for a LAN device (VSI OpenVMS I/O
 *     User's Reference Manual, LAN drivers; VSI LAN Devices, Counters, and
 *     Functions Reference). The "network device" wording SHOW DEVICE prints is
 *     the DEV$M_NET *characteristic* bit, not the class. DC__SCOM's numeric
 *     value mirrors src/libvms/include/dcdef.h, as the class constants above do.
 *
 *   - Device type Unknown (0). This unit is NIC-agnostic -- it fronts whatever
 *     the host's primary Ethernet net device is (virtio-net, e1000, a real NIC),
 *     so there is no single VMS device type (DEQNA/DE435/...) that is honestly
 *     true of it. Unknown is what is true, not a placeholder (same reasoning as
 *     the console's device type above).
 *
 *   - shareable = 1. A LAN controller is a SHAREABLE device on VMS: many users
 *     and protocols $ASSIGN the same controller concurrently through the
 *     template-device mechanism (OpenVMS I/O User's Reference Manual, LAN
 *     drivers; the DEV$M_SHR characteristic). This is the FIRST shareable device
 *     in the table, so -- exactly as the console's shareable=0 comment in
 *     vms_devtab_init() says the first shareable device must -- test_kmod_devtab
 *     now asserts the shareable side of the ownership rule against it: a channel
 *     to ETH0: confers NO ownership (owner stays unowned), the property measured
 *     on NLA0: (docs/oracle/vax73-terminal-device.md §7). CAVEAT, disclosed: the
 *     shareable bit itself is taken from the published LAN-driver docs, not from
 *     an OVMX oracle capture of SHOW DEVICE/FULL on a LAN device.
 */
#define VMS_NIC_DEVNAM  "ETH0:"   /* device-native-naming (operator 2026-08-14) */

static void vms_devtab_probe_nic(void)
{
    char ifname[VMS_NETIF_SIZE];
    int link_up = 0;
    struct vms_device *nic;

    /*
     * Ask the host, through the GENERIC netdev seam, for its primary non-loopback
     * Ethernet net device (exec_netdev_primary, exec_kbackend.h §11). A nonzero
     * return is the honest "this node has no NIC" case: the executive enters NO
     * ETH0: unit, so SHOW DEVICE has no ETH0: row and $ASSIGN ETH0: is
     * SS$_NOSUCHDEV -- never a fake device for a NIC that is not there (INV-6).
     * The interface NAME is the executive's private record (INV-4): it decides
     * WHICH real device ETH0: fronts and is never surfaced to a VMS program.
     */
    ifname[0] = '\0';
    if (exec_netdev_primary(ifname, sizeof(ifname), &link_up) != 0) {
        pr_info("vms: no primary Ethernet net device; %s not created\n",
                VMS_NIC_DEVNAM);
        return;
    }

    nic = vms_devtab_create(VMS_NIC_DEVNAM, DC__SCOM, VMS_DT_UNKNOWN,
                            1 /* shareable -- a LAN controller is shared */,
                            0 /* devchar */, 0 /* width */, 0 /* page */);
    if (!nic) {
        pr_warn("vms: out of memory creating Ethernet unit %s (%s)\n",
                VMS_NIC_DEVNAM, ifname);
        return;
    }

    /*
     * Record the backing interface under dev->lock for form (nothing else runs
     * yet -- this is module init, before /dev/vms exists), keeping the write
     * paired with any future reader that takes the lock.
     */
    exec_lock(&nic->lock);
    strscpy(nic->netif, ifname, sizeof(nic->netif));
    nic->link_up = link_up ? 1u : 0u;
    exec_unlock(&nic->lock);

    pr_info("vms: ethernet unit %s -> %s (carrier %s)\n",
            VMS_NIC_DEVNAM, ifname, link_up ? "up" : "down");
}

int vms_devtab_init(void)
{
    struct vms_device *console;

    /*
     * Init the device-list guard BEFORE the first vms_devtab_create() below --
     * that helper takes vms_device_list_lock, so on NetBSD (kmutex, no static
     * init) it must already be live. On Linux exec_lock_init forwards to
     * spin_lock_init, which the former DEFINE_SPINLOCK did at load; behaviour is
     * unchanged, only the init moves from static to this first line.
     */
    exec_lock_init(&vms_device_list_lock);

    /*
     * shareable = 0: the oracle's terminals are not shareable. Neither
     * "Terminal OPA0: ..." nor "Terminal TTA0: ..." carries the word
     * "shareable" in SHOW DEVICE/FULL's status clause, where NLA0: and
     * the MBAn: mailboxes do.
     *
     * DISCLOSED, not hidden: this is the ONLY device in the table, so
     * the shareable = 1 side of the ownership rule -- measured on NLA0:,
     * see section 7.3 -- has nothing to exercise it and no test asserts
     * it. It is written because leaving it out would silently claim that
     * every device confers ownership, which the oracle contradicts. The
     * first shareable device added here owes the suite that assertion.
     */
    console = vms_devtab_create(VMS_CONSOLE_DEVNAM, DC__TERM, VMS_DT_UNKNOWN,
                                0 /* shareable */,
                                VMS_CONSOLE_DEVCHAR,
                                VMS_CONSOLE_WIDTH, VMS_CONSOLE_PAGE);
    if (!console)
        return -ENOMEM;

    /*
     * Enumerate the node's disks the way a VMS driver enters its units at boot
     * (vms-3e8): DKA0: for the first virtio block device, DKA100: for the
     * second, and so on. Like the console above, no process introduces these
     * -- they exist in the executive's I/O database before /dev/vms does.
     */
    vms_devtab_probe_disks();

    /*
     * Enter the node's primary Ethernet controller as ETH0: the same way (vms-9d2,
     * epic vms-67f L0). Sourced through the generic netdev abstraction so it is
     * NIC-agnostic; if the node has no Ethernet net device, none is entered --
     * SHOW DEVICE ETH0: then has nothing and $ASSIGN ETH0: is SS$_NOSUCHDEV, the
     * honest "no NIC" state (INV-6). Like the disks and the console, it exists in
     * the I/O database before /dev/vms does; no process introduces it.
     */
    vms_devtab_probe_nic();

    pr_info("vms: device table initialized, console terminal %s created\n",
            VMS_CONSOLE_DEVNAM);
    return 0;
}

void vms_devtab_cleanup(void)
{
    struct vms_device *dev, *tmp;

    exec_lock(&vms_device_list_lock);
    exec_list_for_each_entry_safe(dev, tmp, &vms_device_list, list) {
        exec_list_del(&dev->list);
        exec_free(dev);
    }
    exec_unlock(&vms_device_list_lock);
}

/* ================================================================
 * Channels
 * ================================================================ */

/* Caller holds proc->chan_lock. */
static struct vms_channel *chan_find_locked(struct vms_proc *proc, uint32_t chan)
{
    struct vms_channel *ch;

    exec_list_for_each_entry(ch, &proc->channels, list) {
        if (ch->chan == chan)
            return ch;
    }
    return NULL;
}

/*
 * The caller's UIC, from its host-task credentials.
 *
 * The [group,member] -> UIC packing is the facility's own; only the raw
 * uid/gid read crosses the kernel-backend seam (exec_current_gid/uid, the real
 * not-effective id, mapped into the host's initial id namespace -- Linux
 * from_kgid(&init_user_ns, current_gid()) / from_kuid(&init_user_ns,
 * current_uid())).
 *
 * NOT ORACLE-PINNED, and recorded as such in vms-d0b's findings: it is
 * OVMX's own mapping and no test asserts it. It will have to agree with
 * whatever replaces the VMS_UIC_* environment facade (vms-2b8).
 */
static uint32_t caller_uic(void)
{
    return ((exec_current_gid() & 0xFFFFu) << 16) |
            (exec_current_uid() & 0xFFFFu);
}

/* Caller holds dev->lock. Does `pid` still hold a channel to this device? */
static int device_has_channel_locked(struct vms_device *dev, pid_t pid)
{
    struct vms_channel *ch;

    exec_list_for_each_entry(ch, &dev->chanlist, devlink) {
        if (ch->owner_linux_pid == pid)
            return 1;
    }
    return 0;
}

/*
 * Caller holds dev->lock. End ownership that rests on nothing but a
 * channel.
 *
 * ORACLE (VAX 7.3, non-shareable terminal TTA0:): a bare OPEN/WRITE
 * made the opener the owner; CLOSE put the device back to
 * Owner "" / reference count 0. An ALLOCATION is different and is not
 * touched here -- DEALLOCATE OPA0: left the still-channel-holding job
 * as Owner "SYSTEM" with the reference count going 3 -> 2, so the
 * allocation is what ends, not the ownership.
 */
static void device_release_implicit_owner_locked(struct vms_device *dev, pid_t pid)
{
    if (dev->allocated)
        return;
    if (pid == 0 || dev->owner_linux_pid != pid)
        return;
    if (device_has_channel_locked(dev, pid))
        return;

    dev->owner_pid = 0;
    dev->owner_linux_pid = 0;
    dev->owner_uic = 0;
}

/*
 * Caller holds dev->lock. Give an allocation back: the device stops
 * being "allocated" and loses the reference the allocation held. What
 * happens to ownership afterwards is the implicit rule above -- the
 * ex-allocator keeps the device while it still holds a channel.
 */
static void device_dealloc_locked(struct vms_device *dev)
{
    pid_t owner = dev->owner_linux_pid;

    if (!dev->allocated)
        return;
    dev->allocated = 0;
    if (dev->refcnt > 0)
        dev->refcnt--;
    device_release_implicit_owner_locked(dev, owner);
}

/*
 * Give a channel back: unlink it from the device, drop the reference it
 * held, and end any ownership that channel was carrying.
 */
static void device_release_channel(struct vms_channel *ch)
{
    struct vms_device *dev = ch->dev;
    pid_t pid = ch->owner_linux_pid;

    exec_lock(&dev->lock);
    exec_list_del(&ch->devlink);
    if (dev->refcnt > 0)
        dev->refcnt--;
    device_release_implicit_owner_locked(dev, pid);
    exec_unlock(&dev->lock);
}

/*
 * Release everything a dying process held: its channels (which ends any
 * ownership resting on them) and any device it had allocated.
 *
 * ORACLE: STOP CHANHOLD -- a detached process whose only claim on TTA0:
 * was one assigned channel -- put the device back to Owner "" with a
 * reference count of 0. A device left owned by a process that no longer
 * exists is not a state VMS has.
 */
void vms_proc_release_channels(struct vms_proc *proc)
{
    struct vms_channel *ch, *tmp;
    struct vms_device *dev;
    EXEC_LIST_HEAD(doomed);

    exec_lock(&proc->chan_lock);
    exec_list_for_each_entry_safe(ch, tmp, &proc->channels, list)
        exec_list_move(&ch->list, &doomed);
    exec_unlock(&proc->chan_lock);

    exec_list_for_each_entry_safe(ch, tmp, &doomed, list) {
        exec_list_del(&ch->list);
        device_release_channel(ch);
        exec_free(ch);
    }

    exec_lock(&vms_device_list_lock);
    exec_list_for_each_entry(dev, &vms_device_list, list) {
        exec_lock(&dev->lock);
        if (dev->allocated && dev->owner_linux_pid == proc->linux_pid)
            device_dealloc_locked(dev);
        exec_unlock(&dev->lock);
    }
    exec_unlock(&vms_device_list_lock);

    /*
     * Mailbox channels (vms-d44) are a separate list (see vms_mbx.h) --
     * give them back too, exactly as the device channels above. This is
     * what makes a temporary mailbox whose creator dies without an
     * explicit $DASSGN still get freed, instead of leaking for the life
     * of the module.
     */
    vms_mbx_release_all(proc);
}

/*
 * vms_proc_rundown_channels - image rundown's channel release (vms-68f.v).
 *
 * Deassign only the DEVICE channels this process took at access mode >=
 * min_acmode (image rundown passes PSL_C_USER, so only the USER-mode channels
 * an activated image assigned), leaving supervisor/exec/kernel-mode channels
 * -- which are process-permanent -- assigned. This is the resource-level half
 * of "P0 dies at rundown, P1 survives": a channel an image $ASSIGNed is
 * image-scoped and goes here; a channel DCL holds across activations does not.
 * Grounding per class (which classes are image-scoped): docs/oracle/
 * image-rundown-resource-classes.md.
 *
 * NOT released here, deliberately (see that doc's "process-permanent" column):
 *   - mailbox channels (proc->mbx_channels): released only at process death by
 *     vms_mbx_release_all(); image-scoping them is a flagged follow-up, and
 *     NOT releasing them at rundown is the pre-vms-68f.v status quo (no leak
 *     regression -- they still free when the process exits).
 *   - a device the process $ALLOCated: an explicit allocation's rundown class
 *     (image vs process) is not yet oracle-pinned, so this leaves it to process
 *     teardown rather than guess (Rule 8). Deassigning a USER channel still
 *     drops any IMPLICIT ownership resting on that channel, via
 *     device_release_channel(), exactly as full teardown does.
 */
void vms_proc_rundown_channels(struct vms_proc *proc, uint8_t min_acmode)
{
    struct vms_channel *ch, *tmp;
    EXEC_LIST_HEAD(doomed);

    exec_lock(&proc->chan_lock);
    exec_list_for_each_entry_safe(ch, tmp, &proc->channels, list)
        if (ch->acmode >= min_acmode)
            exec_list_move(&ch->list, &doomed);
    exec_unlock(&proc->chan_lock);

    exec_list_for_each_entry_safe(ch, tmp, &doomed, list) {
        exec_list_del(&ch->list);
        device_release_channel(ch);
        exec_free(ch);
    }
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
 *   - A channel to a NON-SHAREABLE device that nobody owns DOES make
 *     the caller its owner, without allocating it. TTA0: on the lab sat
 *     at Owner "" / reference count 0; a bare OPEN/WRITE (a channel and
 *     nothing else) moved it to Owner "SYSTEM" / Owner process ID
 *     20400216 / reference count 1, with no "allocated" in the status
 *     clause -- and a DEALLOCATE at that moment was refused
 *     %SYSTEM-W-DEVNOTALLOC. This is why the console shows an owner on
 *     a system where nobody ever ran ALLOCATE.
 *
 *   - A channel to a SHAREABLE device confers nothing. The identical
 *     DCL sequence on NLA0: ("shareable, mailbox device") left
 *     Owner "" / Owner process ID 00000000 with only the reference
 *     count moving.
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
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.chan = 0;
        args.status = status;
        goto out;
    }

    ch = exec_zalloc(sizeof(*ch));
    if (!ch)
        return -ENOMEM;

    exec_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        exec_unlock(&vms_device_list_lock);
        exec_free(ch);
        args.chan = 0;
        args.status = SS__NOSUCHDEV;
        goto out;
    }

    ch->dev = dev;
    ch->owner_linux_pid = proc->linux_pid;

    /*
     * Record the access mode $ASSIGN was issued from, so image rundown can
     * tell an image-scoped (USER) channel from a process-permanent one
     * (vms-68f.v). VMS $ASSIGN takes an acmode maximized against the caller's
     * mode; OVMX records the caller's current mode, which is that maximum for
     * every caller that does not ask for a more privileged one -- the only
     * callers OVMX has. See docs/design-image-rundown-resource-classes.md.
     */
    exec_lock(&proc->mode_lock);
    ch->acmode = proc->current_mode;
    exec_unlock(&proc->mode_lock);

    exec_lock(&dev->lock);
    dev->refcnt++;
    exec_list_add_tail(&ch->devlink, &dev->chanlist);
    /*
     * Implicit ownership. Note what is NOT here: no reference is added
     * for it (TTA0: showed one channel, one reference, and an owner),
     * and `allocated` stays clear.
     */
    if (!dev->shareable && dev->owner_linux_pid == 0) {
        dev->owner_pid = proc->vms_pid;
        dev->owner_linux_pid = proc->linux_pid;
        dev->owner_uic = caller_uic();
    }
    exec_unlock(&dev->lock);
    exec_unlock(&vms_device_list_lock);

    exec_lock(&proc->chan_lock);
    /*
     * Channel numbers are opaque to the caller on VMS ("the system
     * assigns the channel"), so the allocation policy below -- small
     * ascending non-zero integers, never reused within a process -- is
     * an OVMX design choice and is not claimed to match VMS's.
     */
    ch->chan = ++proc->next_chan;
    exec_list_add_tail(&ch->list, &proc->channels);
    exec_unlock(&proc->chan_lock);

    args.chan = ch->chan;
    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

long vms_ioctl_dassgn(struct vms_proc *proc, unsigned long arg)
{
    struct vms_dassgn_args args;
    struct vms_channel *ch;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->chan_lock);
    ch = chan_find_locked(proc, args.chan);
    if (ch)
        exec_list_del(&ch->list);
    exec_unlock(&proc->chan_lock);

    if (ch) {
        device_release_channel(ch);
        exec_free(ch);
        args.status = SS__NORMAL;
    } else if (vms_acp_dassgn(proc, args.chan) == 0) {
        /*
         * vms-149: `chan` named a Files-11 ACP file-class channel, not a
         * device one -- a file channel is bound to a mounted ODS-2 volume in
         * the executive-global mount table (src/kernel-core/vmsfs_acp.c), not a
         * struct vms_device row, so it is in proc->file_channels rather than
         * proc->channels, but $DASSGN is still the ONE ioctl that releases any
         * channel kind, drawing from the same channel-number space
         * (proc->next_chan). vms_acp_dassgn() has already dropped the volume's
         * assigned-channel reference.
         */
        args.status = SS__NORMAL;
    } else if (vms_mbx_dassgn(proc, args.chan) == 0) {
        /*
         * vms-d44: `chan` named a mailbox channel, not a device one --
         * mailboxes are not struct vms_device rows (see vms_mbx.c's
         * header), so they are not in proc->channels / chan_find_locked()
         * above, but $DASSGN is still the ONE ioctl that releases either
         * kind, drawing from the same channel-number space
         * (proc->next_chan). vms_mbx_dassgn() has already dropped the
         * mailbox's reference and freed it if that was the last one.
         */
        args.status = SS__NORMAL;
    } else {
        args.status = SS__IVCHAN;
    }

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $ALLOC - allocate a device to this process.
 *
 * There is exactly ONE refusal, and it is about OWNERSHIP, not about
 * allocation and not about channels: a device somebody else owns
 * cannot be allocated. Both observed cases on the ~/vax OpenVMS VAX
 * V7.3 lab reduce to it (docs/oracle/vax73-terminal-device.md
 * section 7):
 *
 *   owner holds an allocation  -> %SYSTEM-W-DEVALLOC
 *       ALLOCATE OPA0: from a detached process while the interactive
 *       job held the console.
 *   owner holds only a channel -> %SYSTEM-W-DEVALLOC
 *       CHANHOLD, a detached process whose only claim on TTA0: was one
 *       assigned channel, showed as "Owner process CHANHOLD" with the
 *       status clause carrying no "allocated"; ALLOCATE TTA0: from the
 *       interactive job was refused all the same.
 *   already allocated to us    -> success, reference count unchanged
 *       ALLOCATE OPA0: twice in a row: 2 -> 3 -> 3.
 *   ours by channel, or free   -> success, reference count + 1
 *       ALLOCATE OPA0: from the job that already owned it (by channel,
 *       not by allocation): reference count 2 -> 3, and the status
 *       clause gained the word "allocated".
 *
 * WHAT IS NOT MODELLED (rule 10 -- do not invent a handler for a
 * condition that was never measured): %SYSTEM-W-DEVASSIGN, "device has
 * channels assigned" (2120). VMS clearly has this condition -- its own
 * message facility printed the text -- but no probe ever provoked it,
 * so OVMX does not return it anywhere and no code here is shaped around
 * it. Carried in vms-d0b's findings; the probe that would settle it has
 * to find the operation that raises DEVASSIGN, not assume one.
 */
long vms_ioctl_alloc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_alloc_args args;
    struct vms_device *dev;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    exec_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        exec_unlock(&vms_device_list_lock);
        args.status = SS__NOSUCHDEV;
        goto out;
    }

    exec_lock(&dev->lock);
    if (dev->owner_linux_pid != 0 && dev->owner_linux_pid != proc->linux_pid) {
        args.status = SS__DEVALLOC;
    } else if (dev->allocated) {
        args.status = SS__NORMAL;            /* already ours; idempotent */
    } else {
        dev->allocated = 1;
        dev->owner_pid = proc->vms_pid;
        dev->owner_linux_pid = proc->linux_pid;
        dev->owner_uic = caller_uic();
        dev->refcnt++;
        args.status = SS__NORMAL;
    }
    exec_unlock(&dev->lock);
    exec_unlock(&vms_device_list_lock);

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $DALLOC - give an allocated device back.
 *
 * Deallocating a device this process does not have ALLOCATED is
 * %SYSTEM-W-DEVNOTALLOC on the lab, and "does not have allocated"
 * includes a device it owns by channel: DEALLOCATE TTA0: while holding
 * only an open channel to it was refused %SYSTEM-W-DEVNOTALLOC with
 * Owner "SYSTEM" left in place. A second DEALLOCATE OPA0: right after
 * the first is refused the same way.
 *
 * What a successful $DALLOC does NOT do is make the device unowned:
 * DEALLOCATE OPA0: took the reference count 3 -> 2 and dropped the word
 * "allocated", and the job that still held channels stayed Owner
 * "SYSTEM". Ownership then follows the implicit rule.
 */
long vms_ioctl_dalloc(struct vms_proc *proc, unsigned long arg)
{
    struct vms_alloc_args args;
    struct vms_device *dev;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    exec_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        exec_unlock(&vms_device_list_lock);
        args.status = SS__NOSUCHDEV;
        goto out;
    }

    exec_lock(&dev->lock);
    if (dev->allocated && dev->owner_linux_pid == proc->linux_pid) {
        device_dealloc_locked(dev);
        args.status = SS__NORMAL;
    } else {
        args.status = SS__DEVNOTALLOC;
    }
    exec_unlock(&dev->lock);
    exec_unlock(&vms_device_list_lock);

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * ================================================================
 * SUBSTRATE DISK RESOLVE (rd vms-618)
 * ================================================================
 *
 * The two functions below resolve a DISK unit to its backing block device by
 * READING THIS TABLE. That is the whole job on Linux, where the block seam
 * (exec_blockdev_read_block, exec_kbackend_linux.h) opens the device by dev_t
 * for each I/O -- so a table read is a complete answer.
 *
 * On the NetBSD substrate it is NOT a complete answer, for a reason that is
 * about the host kernel, not about VMS: NetBSD's block-device open is
 * effectively single-holder (spec_vnops returns EBUSY on a second open -- the
 * very constraint that forced the VAX ACP cutover to be ACP-ONLY, rd vms-329),
 * and its block seam reads/writes through a CACHED device vnode. So on that
 * substrate the resolve must ALSO lazily open and cache the vnode at $MOUNT
 * time -- and must NOT hold the INITIALIZE target open, because INITIALIZE.EXE
 * opens that device itself (rd vms-f60). Both are things a table read cannot
 * do, so the NetBSD backend supplies its own definitions
 * (src/kernel-netbsd/vms_blockdev_netbsd.c) and compiles these out by defining
 * OVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE.
 *
 * WHAT IS *NOT* SUBSTRATE-LOCAL, and must never become so (INV-6): the DEVICE
 * TABLE itself and everything ownership-shaped -- $ALLOC/$DALLOC/$ASSIGN/
 * $DASSGN/$GETDVI/$DEVICE_SCAN. There is exactly ONE implementation of those,
 * this file's, on every substrate. Only the host-kernel binding of "which real
 * device backs this unit, and how do I hold it open" differs, which is exactly
 * the kind of thing the kernel-backend seam exists to vary.
 */
#ifndef OVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE
/*
 * vms_devtab_disk_backing - INTERNAL (non-ioctl) resolve of a DISK unit to its
 * backing (major, minor), for the Files-11 ODS-2 ACP $MOUNT (vms-127). The exact
 * lookup vms_ioctl_disk_resolve() does, minus the copyin/copyout: the caller is
 * IN the executive (vmsfs_acp.c reading the home block/SCB to validate a volume),
 * not a process handing structs across /dev/vms. `devnam` must already be a
 * canonical "DDCU:" name (the ACP normalizes before calling). Returns SS$_NORMAL
 * and fills the major_out and minor_out outputs on a disk unit; SS$_NOSUCHDEV if
 * there is no such unit; SS$_IVDEVNAM if the unit exists but is not a disk (only disk units
 * have a backing block device -- the same category verdict the ioctl gives).
 */
uint32_t vms_devtab_disk_backing(const char *devnam,
                                 uint32_t *major_out, uint32_t *minor_out)
{
    struct vms_device *dev;

    if (!devnam)
        return SS__NOSUCHDEV;

    exec_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        exec_unlock(&vms_device_list_lock);
        return SS__NOSUCHDEV;
    }
    if (dev->devclass != DC__DISK) {
        exec_unlock(&vms_device_list_lock);
        return SS__IVDEVNAM;
    }

    exec_lock(&dev->lock);
    if (major_out)
        *major_out = dev->backing_major;
    if (minor_out)
        *minor_out = dev->backing_minor;
    exec_unlock(&dev->lock);
    exec_unlock(&vms_device_list_lock);

    return SS__NORMAL;
}

/*
 * Resolve a DISK unit to the Linux block device the executive enumerated it
 * from (vms-3e8). Read-only: it reports the vda/vdb/... name and the dev_t the
 * unit was created against at module init, so a process that must open the
 * backing device (MOUNT, vms-651) asks the executive rather than scanning
 * /sys/block for itself -- the fact lives in the executive (CLAUDE.md Rule 11).
 *
 * SS$_NOSUCHDEV      no unit by that name in the table.
 * SS$_IVDEVNAM       the name is not a legal device name, OR it names a device
 *                    that is not a DISK (only disk units have a backing block
 *                    device; asking for OPA0:'s backing is a category error).
 *                    The not-a-disk verdict is an OVMX design choice (Rule 8):
 *                    this facility has no VMS counterpart to pin a status
 *                    against, so IVDEVNAM -- "this name is not a (disk) device
 *                    name for this operation" -- is the closest honest answer.
 */
long vms_ioctl_disk_resolve(struct vms_proc *proc, unsigned long arg)
{
    struct vms_diskresolve_args args;
    struct vms_device *dev;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    (void)proc;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    exec_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (!dev) {
        exec_unlock(&vms_device_list_lock);
        args.status = SS__NOSUCHDEV;
        goto out;
    }
    if (dev->devclass != DC__DISK) {
        exec_unlock(&vms_device_list_lock);
        args.status = SS__IVDEVNAM;
        goto out;
    }

    exec_lock(&dev->lock);
    strscpy(args.backing, dev->backing, sizeof(args.backing));
    args.backing_major = dev->backing_major;
    args.backing_minor = dev->backing_minor;
    exec_unlock(&dev->lock);
    exec_unlock(&vms_device_list_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
#endif /* !OVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE */

/* Snapshot a device row for userspace. Takes dev->lock. */
static void devinfo_fill(struct vms_device *dev, struct vms_devinfo *info)
{
    memset(info, 0, sizeof(*info));

    exec_lock(&dev->lock);
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
    exec_unlock(&dev->lock);
}

long vms_ioctl_getdvi(struct vms_proc *proc, unsigned long arg)
{
    struct vms_getdvi_args args;
    struct vms_device *dev = NULL;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    if (args.select == VMS_DVI_SEL_CHAN) {
        struct vms_channel *ch;

        exec_lock(&proc->chan_lock);
        ch = chan_find_locked(proc, args.chan);
        if (ch)
            dev = ch->dev;
        exec_unlock(&proc->chan_lock);

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

    exec_lock(&vms_device_list_lock);
    dev = devtab_lookup_locked(devnam);
    if (dev)
        devinfo_fill(dev, &args.info);
    exec_unlock(&vms_device_list_lock);

    if (!dev) {
        memset(&args.info, 0, sizeof(args.info));
        args.status = SS__NOSUCHDEV;
    } else {
        args.status = SS__NORMAL;
    }

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
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
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&vms_device_list_lock);
    exec_list_for_each_entry(dev, &vms_device_list, list) {
        if (i == args.index) {
            found = dev;
            break;
        }
        i++;
    }
    if (found)
        devinfo_fill(found, &args.info);
    exec_unlock(&vms_device_list_lock);

    if (!found) {
        args.status = SS__NOMOREDEV;
    } else {
        args.index++;
        args.status = SS__NORMAL;
    }

    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * Record the calling process's TERMINAL in the executive's process
 * table (vms-d0b).
 *
 * WHAT THIS REPLACES, and why the shape matters more than the code.
 * Until this existed the job's terminal was carried in a VMS_TERMINAL
 * environment variable that PID 1 set on its login child -- deleted by
 * vms-fb9 as the rejected VMS_PRCNAM cheat (CLAUDE.md Rule 10, worked
 * example 2). Deleting it left SHOW TERMINAL with nothing to read,
 * which was the honest state but not the finished one. The binding now
 * lives where prcnam and username live, so a SECOND process can see
 * which terminal a job is on, which is the whole difference between a
 * system facility and a self-description (Rule 11).
 *
 * THE CALLER SUPPLIES A CHANNEL AND NO TEXT. The executive resolves
 * the channel in the calling process's own channel list, takes the
 * device it is assigned to, and copies THAT device's name. So the set
 * of names a process can bind itself to is exactly the set of devices
 * the executive issued it a channel for -- it cannot name a terminal
 * it never opened, and it cannot name a device that does not exist.
 * An "OVMX-set-my-terminal-by-name" ioctl would have been the
 * environment variable with more ceremony.
 *
 * The two refusals are the two IO$_SETMODE already returns for the
 * same two mistakes, one function up: a channel this process does not
 * hold is SS$_IVCHAN, and a channel to something that is not a
 * terminal is SS$_IVDEVNAM.
 */
long vms_ioctl_setterm(struct vms_proc *proc, unsigned long arg)
{
    struct vms_setterm_args args;
    struct vms_channel *ch;
    struct vms_device *dev = NULL;
    char devnam[VMS_DEVNAM_SIZE];

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->chan_lock);
    ch = chan_find_locked(proc, args.chan);
    if (ch)
        dev = ch->dev;
    exec_unlock(&proc->chan_lock);

    if (!dev) {
        args.status = SS__IVCHAN;
        goto out;
    }

    if (dev->devclass != DC__TERM) {
        args.status = SS__IVDEVNAM;
        goto out;
    }

    exec_lock(&dev->lock);
    strscpy(devnam, dev->devnam, sizeof(devnam));
    exec_unlock(&dev->lock);

    /*
     * Written under vms_proc_hash_lock -- the process table's lock, and
     * the one proc_fill_info() holds while it reads this field for a
     * $GETJPI or $PROCSCAN row. The name is stored under the same lock
     * every reader takes.
     */
    exec_lock(&vms_proc_hash_lock);
    strscpy(proc->terminal, devnam, sizeof(proc->terminal));

    /*
     * BINDING A TERMINAL MAKES THIS PROCESS AN INTERACTIVE JOB ROOT
     * (vms-01f).
     *
     * SETTERM records "THIS JOB's terminal" (see the vms_kif_setterm call
     * in src/ovmx_job_control/ovmx_job_control.c, and vms_ioctl.h) -- a
     * process that owns a terminal is, by definition, the master process
     * of an interactive job. On OpenVMS an interactive login created by the
     * job controller is the top of its OWN job, not a member of the job
     * controller's job (System Services Reference, $CREPRC / job trees).
     *
     * OVMX derives job_id from Linux task ancestry at registration
     * (vms_proc_parent_job_id, src/kernel/vms_module.c): a task whose real
     * parent already has a PCB inherits that parent's job. That rule is
     * correct for a SPAWNed subprocess (its parent IS the interactive DCL),
     * but WRONG for the console login: JOB_CONTROL is a real, registered,
     * DETACHED process (test_job_control_console.sh proves SHOW SYSTEM
     * lists it), so its fork()ed login child inherited JOB_CONTROL's job_id
     * and became a "subprocess" of a terminal-LESS root -- which
     * proc_fill_info() classifies OTHER, which SHOW USERS filters out. The
     * whole interactive session, and every subprocess it later SPAWNs
     * (they inherit ITS job), then vanished from SHOW USERS / SHOW USERS
     * /FULL -- "Total number of users = 0" on a live console login
     * (vms-01f). The mechanism was proven only by test_syssvc_spawn_users.c,
     * which SIDESTEPS the defect by keeping its creator UNregistered so the
     * session child becomes a job root for free -- exactly the condition the
     * real boot does not meet.
     *
     * Promoting here, keyed on the OBSERVABLE fact that this process bound
     * its own terminal, is faithful and forge-safe: job membership grants
     * no identity or privilege (unlike UIC/username/privs, which stay
     * derived, never asserted), it only scopes LNM$JOB (vms_lnm.c) and the
     * SHOW USERS classification -- both of which are correct for a terminal
     * owner to root. A detached process (JOB_CONTROL) never calls SETTERM
     * and stays a terminal-less root; a subprocess (SPAWN) never calls it
     * and stays in its creator's job. It also fixes a latent LNM$JOB bug:
     * before this, every console login shared JOB_CONTROL's job table, so
     * one session's SYS$LOGIN was visible to all; now each login job is its
     * own LNM$JOB scope, as on VMS.
     */
    proc->job_id = proc->vms_pid;
    exec_unlock(&vms_proc_hash_lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

long vms_ioctl_ttsetmode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_setmode_args args;
    struct vms_channel *ch;
    struct vms_device *dev = NULL;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    /*
     * Characteristics are set THROUGH A CHANNEL, as VMS sets them
     * ($QIO IO$_SETMODE). No channel, no change -- a process cannot
     * redefine a terminal it never opened.
     */
    exec_lock(&proc->chan_lock);
    ch = chan_find_locked(proc, args.chan);
    if (ch)
        dev = ch->dev;
    exec_unlock(&proc->chan_lock);

    if (!dev) {
        args.status = SS__IVCHAN;
        goto out;
    }

    if (dev->devclass != DC__TERM) {
        /* IO$_SETMODE terminal function on a non-terminal device. */
        args.status = SS__IVDEVNAM;
        goto out;
    }

    exec_lock(&dev->lock);
    if (args.flags & VMS_TTSET_CHAR) {
        dev->devchar |= args.setchar;
        dev->devchar &= ~args.clrchar;
    }
    if (args.flags & VMS_TTSET_WIDTH)
        dev->width = args.width;
    if (args.flags & VMS_TTSET_PAGE)
        dev->page = args.page;
    dev->opcnt++;
    exec_unlock(&dev->lock);

    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
