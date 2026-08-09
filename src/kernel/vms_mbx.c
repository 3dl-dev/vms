// SPDX-License-Identifier: GPL-2.0
/*
 * vms_mbx.c - Executive-resident mailboxes (MBAn:, vms-d44)
 *
 * See vms_mbx.h for the full rationale (what this replaces, the residency
 * split, the rendezvous story). In one line: a mailbox is a device one
 * process creates and another opens, so -- following the SAME pattern
 * LNM$SYSTEM (vms_lnm.c) and the device table (vms_devtab.c) already use --
 * its storage lives here, in vms.ko, not in either process's own memory.
 *
 * TABLE SHAPE. Unlike vms_devtab.c's console (created once at module init,
 * because a real terminal driver enters its unit at system init), mailboxes
 * are created ON DEMAND by $CREMBX, so the table starts empty and grows and
 * shrinks as mailboxes are created and deleted. A mailbox is reachable by
 * device name (MBAn:, via $ASSIGN) by any process that knows the name --
 * from a logical name it was given (LNM$SYSTEM, see sys_mailbox.c) or from
 * the name itself, exactly as a real MBAn: unit is.
 *
 * CHANNELS. A channel to a mailbox is NOT a struct vms_channel /
 * struct vms_device pair (vms_devtab.c): a mailbox carries a message queue
 * and a buffer-quota counter a generic device row has no field for, so it
 * gets its own small binding (struct vms_mbx_chan) in its own per-process
 * list (struct vms_proc::mbx_channels). It still draws its channel NUMBER
 * from the SAME counter (proc->next_chan) device channels use, because on
 * real VMS a process's channels are one number space regardless of what
 * kind of device they name -- see vms_mbx.h's comment on that list.
 * $DASSGN (vms_devtab.c's vms_ioctl_dassgn) checks proc->channels first and
 * falls back to vms_mbx_dassgn() below, so there is one $DASSGN ioctl for
 * either kind, exactly as VMS has one $DASSGN system service.
 *
 * DELETION. A TEMPORARY mailbox is freed the instant its last channel is
 * deassigned (OpenVMS System Services Reference, $CREMBX). A PERMANENT one
 * survives that and is freed only once $DELMBX has marked it AND its last
 * channel is then deassigned -- $DELMBX itself never deassigns anything,
 * matching the same reference's $DELMBX description. mbx_put() below is the
 * ONE place both rules are applied, from both vms_mbx_dassgn() (an explicit
 * $DASSGN) and vms_mbx_release_all() (a dying process giving back
 * everything it held, exactly as vms_proc_release_channels() does for
 * device channels).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/ctype.h>
#include <linux/string.h>

#include "vms_internal.h"

/* One queued message. Allocated kmalloc(sizeof(*m) + len) with `data` as a
 * flexible array member -- a mailbox message has no fixed size below the
 * VMS_MBX_IOCTL_MAXLEN ioctl-transfer cap (vms_mbx.h), so there is no
 * reason to always pay for the maximum. */
struct vms_mbx_msg {
    struct list_head list;
    uint32_t len;
    char data[];
};

/* One executive-resident mailbox. */
struct vms_mailbox {
    struct list_head list;      /* in vms_mbx_list */
    uint32_t unit;              /* MBAn: unit number */
    char devnam[VMS_DEVNAM_SIZE]; /* "MBAn:" */
    uint32_t permanent;         /* 1 = created with PRMMBX (needs $DELMBX+last-dassgn) */
    uint32_t maxmsg;            /* per-message cap, bytes */
    uint32_t bufquo;            /* aggregate queued-bytes cap */
    uint32_t bufquo_used;       /* bytes currently queued */
    uint32_t refcnt;            /* channels currently assigned (any process) */
    uint32_t delete_pending;    /* $DELMBX was called */
    /*
     * Creator identity. Informational only (the SHOW-DEVICE-style row a
     * future $GETDVI-for-mailboxes could report) -- NOT consulted by
     * $ASSIGN's lookup. A mailbox that only its own creator could reach
     * would not be the shared IPC object $CREMBX documents; the negative
     * control this item's suite carries (mbx-not-shared,
     * tests/qemu/facility_defects.sh) is exactly the mutation that adds
     * that check back in.
     */
    pid_t    owner_linux_pid;
    uint32_t owner_vms_pid;
    struct list_head msgq;      /* struct vms_mbx_msg, FIFO */
    wait_queue_head_t read_wq;
    spinlock_t lock;            /* guards everything above except `list` */
};

/* One process's channel to a mailbox. */
struct vms_mbx_chan {
    struct list_head list;      /* in proc->mbx_channels */
    uint32_t chan;
    struct vms_mailbox *mbx;
};

static LIST_HEAD(vms_mbx_list);
static DEFINE_SPINLOCK(vms_mbx_list_lock);
static uint32_t vms_mbx_next_unit = 1;

void vms_mbx_init(void)
{
    /* The table starts empty -- mailboxes are created on demand by
     * $CREMBX, unlike vms_devtab.c's console row (a real driver enters its
     * unit at system init; nothing "boots" a mailbox). */
    pr_info("vms: mailbox table initialized\n");
}

void vms_mbx_cleanup(void)
{
    struct vms_mailbox *mbx, *mtmp;
    struct vms_mbx_msg *m, *mmtmp;

    spin_lock(&vms_mbx_list_lock);
    list_for_each_entry_safe(mbx, mtmp, &vms_mbx_list, list) {
        list_del(&mbx->list);
        list_for_each_entry_safe(m, mmtmp, &mbx->msgq, list) {
            list_del(&m->list);
            kfree(m);
        }
        kfree(mbx);
    }
    spin_unlock(&vms_mbx_list_lock);
}

/*
 * mbx_normalize_devnam - fold a caller-supplied name into the canonical
 * "MBA<n>:" form (uppercase, exactly one trailing colon) and verify it
 * actually has that shape. Deliberately narrower than vms_devtab.c's
 * normalize_devnam(): a mailbox device name is always MBA followed by
 * decimal digits, so a name that is not that shape can never match a row
 * in this table and is rejected here rather than compared 512 times.
 */
static uint32_t mbx_normalize_devnam(const char *in, char *out, size_t outsz)
{
    size_t i, n = 0;
    unsigned int unit;
    char digits[VMS_DEVNAM_SIZE];

    if (!in || !out || outsz < 2)
        return SS__BADPARAM;

    if (in[0] == '_')            /* physical-name underscore */
        in++;

    for (i = 0; in[i] != '\0'; i++) {
        char c = in[i];

        if (i >= VMS_DEVNAM_SIZE)
            return SS__IVDEVNAM;
        if (c == ':') {
            if (in[i + 1] != '\0')
                return SS__IVDEVNAM;
            break;
        }
        if (!isalnum((unsigned char)c))
            return SS__IVDEVNAM;
        if (n + 2 >= outsz)
            return SS__IVDEVNAM;
        out[n++] = (char)toupper((unsigned char)c);
    }

    if (n < 4 || out[0] != 'M' || out[1] != 'B' || out[2] != 'A')
        return SS__IVDEVNAM;

    if (n - 3 >= sizeof(digits))
        return SS__IVDEVNAM;
    memcpy(digits, out + 3, n - 3);
    digits[n - 3] = '\0';
    if (kstrtouint(digits, 10, &unit))
        return SS__IVDEVNAM;

    out[n++] = ':';
    out[n] = '\0';
    return SS__NORMAL;
}

/* Caller holds vms_mbx_list_lock. */
static struct vms_mailbox *mbx_find_locked(const char *devnam)
{
    struct vms_mailbox *mbx;

    list_for_each_entry(mbx, &vms_mbx_list, list) {
        if (strcmp(mbx->devnam, devnam) == 0)
            return mbx;
    }
    return NULL;
}

/* Caller holds proc->chan_lock. */
static struct vms_mbx_chan *mbxchan_find_locked(struct vms_proc *proc, uint32_t chan)
{
    struct vms_mbx_chan *ch;

    list_for_each_entry(ch, &proc->mbx_channels, list) {
        if (ch->chan == chan)
            return ch;
    }
    return NULL;
}

/*
 * mbx_priv_check - the privilege gate $CREMBX consults (System Services
 * Reference, $CREMBX: "you need the TMPMBX privilege ... to create a
 * permanent mailbox, you need the PRMMBX privilege" -- also documented in
 * tests/corpus/tier1-examples/sys_crembx.c's own comment). Checked against
 * cur_privs, the ENABLED mask, exactly as vms_lnm.c's lnm_priv_check() and
 * vms_access.c gate every other privileged mutation.
 */
static bool mbx_priv_check(uint32_t permanent, uint64_t cur_privs, uint32_t *status)
{
    bool ok = permanent ? (cur_privs & VMS_PRV_M_PRMMBX) != 0
                         : (cur_privs & VMS_PRV_M_TMPMBX) != 0;

    if (!ok)
        *status = SS__NOPRIV;
    return ok;
}

/*
 * mbx_put - drop one channel's reference to `mbx` and free it if this was
 * the last one and it is due to go (temporary, or $DELMBX-marked). The ONE
 * place the deletion rule (vms_mbx.h / this file's header) is applied, so
 * vms_mbx_dassgn() and vms_mbx_release_all() cannot disagree about it.
 */
static void mbx_put(struct vms_mailbox *mbx)
{
    int free_it = 0;

    spin_lock(&vms_mbx_list_lock);
    spin_lock(&mbx->lock);
    if (mbx->refcnt > 0)
        mbx->refcnt--;
    if (mbx->refcnt == 0 && (!mbx->permanent || mbx->delete_pending)) {
        list_del(&mbx->list);
        free_it = 1;
    }
    spin_unlock(&mbx->lock);
    spin_unlock(&vms_mbx_list_lock);

    if (free_it) {
        struct vms_mbx_msg *m, *tmp;

        list_for_each_entry_safe(m, tmp, &mbx->msgq, list) {
            list_del(&m->list);
            kfree(m);
        }
        kfree(mbx);
    }
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

long vms_ioctl_mbx_create(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mbx_create_args args;
    struct vms_mailbox *mbx;
    struct vms_mbx_chan *ch;
    uint32_t unit;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (!mbx_priv_check(args.permanent, proc->cur_privs, &args.status))
        goto out_copy;

    mbx = kzalloc(sizeof(*mbx), GFP_KERNEL);
    if (!mbx)
        return -ENOMEM;
    ch = kzalloc(sizeof(*ch), GFP_KERNEL);
    if (!ch) {
        kfree(mbx);
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&mbx->msgq);
    init_waitqueue_head(&mbx->read_wq);
    spin_lock_init(&mbx->lock);

    mbx->maxmsg = args.maxmsg ? args.maxmsg : VMS_MBX_DEFAULT_MAXMSG;
    if (mbx->maxmsg > VMS_MBX_IOCTL_MAXLEN)
        mbx->maxmsg = VMS_MBX_IOCTL_MAXLEN;
    mbx->bufquo = args.bufquo ? args.bufquo : VMS_MBX_DEFAULT_BUFQUO;
    if (mbx->bufquo > VMS_MBX_IOCTL_MAXLEN)
        mbx->bufquo = VMS_MBX_IOCTL_MAXLEN;
    mbx->permanent = args.permanent ? 1 : 0;
    mbx->refcnt = 1;
    mbx->owner_linux_pid = proc->linux_pid;
    mbx->owner_vms_pid = proc->vms_pid;

    spin_lock(&vms_mbx_list_lock);
    unit = vms_mbx_next_unit++;
    mbx->unit = unit;
    snprintf(mbx->devnam, sizeof(mbx->devnam), "MBA%u:", unit);
    list_add_tail(&mbx->list, &vms_mbx_list);
    spin_unlock(&vms_mbx_list_lock);

    ch->mbx = mbx;
    spin_lock(&proc->chan_lock);
    ch->chan = ++proc->next_chan;
    list_add_tail(&ch->list, &proc->mbx_channels);
    spin_unlock(&proc->chan_lock);

    args.chan = ch->chan;
    args.unit = unit;
    strscpy(args.devnam, mbx->devnam, sizeof(args.devnam));
    args.status = SS__NORMAL;

out_copy:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $ASSIGN to an existing mailbox by device name -- the rendezvous path an
 * UNRELATED process uses to reach a mailbox it did not create (having
 * learned MBAn: from a logical name, see sys_mailbox.c). The lookup is by
 * device name ALONE: no ownership or creator check gates it, because a
 * mailbox anyone but its creator can never open is not the IPC object VMS
 * documents (see mbx-not-shared in tests/qemu/facility_defects.sh, this
 * item's negative control for exactly that regression).
 */
long vms_ioctl_mbx_assign(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mbx_assign_args args;
    struct vms_mailbox *mbx;
    struct vms_mbx_chan *ch;
    char devnam[VMS_DEVNAM_SIZE];
    uint32_t status;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.devnam[VMS_DEVNAM_SIZE - 1] = '\0';

    status = mbx_normalize_devnam(args.devnam, devnam, sizeof(devnam));
    if (status != SS__NORMAL) {
        args.status = status;
        goto out;
    }

    ch = kzalloc(sizeof(*ch), GFP_KERNEL);
    if (!ch)
        return -ENOMEM;

    spin_lock(&vms_mbx_list_lock);
    mbx = mbx_find_locked(devnam);
    if (!mbx) {
        spin_unlock(&vms_mbx_list_lock);
        kfree(ch);
        args.status = SS__NOSUCHDEV;
        goto out;
    }
    spin_lock(&mbx->lock);
    mbx->refcnt++;
    spin_unlock(&mbx->lock);
    spin_unlock(&vms_mbx_list_lock);

    ch->mbx = mbx;
    spin_lock(&proc->chan_lock);
    ch->chan = ++proc->next_chan;
    list_add_tail(&ch->list, &proc->mbx_channels);
    spin_unlock(&proc->chan_lock);

    args.chan = ch->chan;
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $DELMBX - mark the mailbox for deletion. Does NOT touch the caller's own
 * channel (System Services Reference, $DELMBX: deletion happens "when all
 * channels ... have been deassigned", which this call does not do) --
 * mbx_put() is what actually frees it, the next time a channel (any
 * process's) is deassigned and finds refcnt reaching zero.
 */
long vms_ioctl_mbx_delmbx(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mbx_delmbx_args args;
    struct vms_mbx_chan *ch;
    struct vms_mailbox *mbx;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->chan_lock);
    ch = mbxchan_find_locked(proc, args.chan);
    mbx = ch ? ch->mbx : NULL;
    spin_unlock(&proc->chan_lock);

    if (!mbx) {
        args.status = SS__IVCHAN;
        goto out;
    }

    spin_lock(&mbx->lock);
    mbx->delete_pending = 1;
    spin_unlock(&mbx->lock);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $QIO IO$_WRITEVBLK-equivalent: one message, moved whole. Reserves the
 * buffer-quota bytes BEFORE allocating the message (rather than check then
 * allocate then re-check), so two concurrent writers on the same mailbox
 * cannot both pass the quota check for bytes only one of them can have.
 */
long vms_ioctl_mbx_write(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mbx_write_args *a;
    struct vms_mbx_chan *ch;
    struct vms_mailbox *mbx;
    struct vms_mbx_msg *m;
    long ret = 0;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    if (copy_from_user(a, (void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    spin_lock(&proc->chan_lock);
    ch = mbxchan_find_locked(proc, a->chan);
    mbx = ch ? ch->mbx : NULL;
    spin_unlock(&proc->chan_lock);

    if (!mbx) {
        a->status = SS__IVCHAN;
        goto out_copy;
    }

    if (a->len > VMS_MBX_IOCTL_MAXLEN) {
        a->status = SS__EXQUOTA;
        goto out_copy;
    }

    spin_lock(&mbx->lock);
    if (a->len > mbx->maxmsg || mbx->bufquo_used + a->len > mbx->bufquo) {
        spin_unlock(&mbx->lock);
        /*
         * SS$_EXQUOTA for both "too big for this mailbox" and "no room
         * left in it": real VMS's SS$_MBFULL is not yet oracle-pinned
         * against any reference lab (see vms_internal.h's SS__EXQUOTA
         * comment), so this reuses an already-pinned status rather than
         * inventing one this tree cannot cite (CLAUDE.md Rule 8).
         */
        a->status = SS__EXQUOTA;
        goto out_copy;
    }
    mbx->bufquo_used += a->len;
    spin_unlock(&mbx->lock);

    m = kmalloc(sizeof(*m) + a->len, GFP_KERNEL);
    if (!m) {
        spin_lock(&mbx->lock);
        mbx->bufquo_used -= a->len;
        spin_unlock(&mbx->lock);
        ret = -ENOMEM;
        goto out_free;
    }
    m->len = a->len;
    memcpy(m->data, a->data, a->len);

    spin_lock(&mbx->lock);
    list_add_tail(&m->list, &mbx->msgq);
    spin_unlock(&mbx->lock);
    wake_up_interruptible(&mbx->read_wq);

    a->status = SS__NORMAL;

out_copy:
    if (copy_to_user((void __user *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    kfree(a);
    return ret;
}

/*
 * $QIO IO$_READVBLK-equivalent: block until a message is queued, then hand
 * back exactly what was written (record-oriented -- a mailbox never
 * coalesces or splits messages).
 *
 * INTERRUPTED WAITS: THERE IS NO SUCH OUTCOME, same rule and same reasoning
 * as vms_eflag.c's WAITFR (see that file's header note in full). VMS's
 * mailbox read blocks until a message arrives or the channel is otherwise
 * disposed of; there is no "your read was interrupted" condition value to
 * report. So a signal here returns -ERESTARTSYS with NO status written,
 * and libvmssys' vms_kif_mbx_read() re-enters the wait via kif_wait_call()
 * -- the exact helper WAITFR/WFLOR/WFLAND already use.
 */
long vms_ioctl_mbx_read(struct vms_proc *proc, unsigned long arg)
{
    struct vms_mbx_read_args *a;
    struct vms_mbx_chan *ch;
    struct vms_mailbox *mbx;
    struct vms_mbx_msg *m;
    long ret = 0;
    uint32_t n;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    if (copy_from_user(a, (void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    spin_lock(&proc->chan_lock);
    ch = mbxchan_find_locked(proc, a->chan);
    mbx = ch ? ch->mbx : NULL;
    spin_unlock(&proc->chan_lock);

    if (!mbx) {
        a->status = SS__IVCHAN;
        goto out_copy;
    }

    for (;;) {
        int wret = wait_event_interruptible(mbx->read_wq, !list_empty(&mbx->msgq));

        if (wret) {
            ret = wret;
            goto out_free;
        }

        spin_lock(&mbx->lock);
        m = list_first_entry_or_null(&mbx->msgq, struct vms_mbx_msg, list);
        if (m) {
            list_del(&m->list);
            mbx->bufquo_used -= m->len;
        }
        spin_unlock(&mbx->lock);

        if (m)
            break;
        /* Lost the race to another reader on the same channel/mailbox;
         * wait for the next message instead of fabricating an answer. */
    }

    n = m->len;
    if (n > a->bufsz)
        n = a->bufsz;
    if (n > VMS_MBX_IOCTL_MAXLEN)
        n = VMS_MBX_IOCTL_MAXLEN;
    memcpy(a->data, m->data, n);
    a->len = m->len;
    kfree(m);
    a->status = SS__NORMAL;

out_copy:
    if (copy_to_user((void __user *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    kfree(a);
    return ret;
}

/* ================================================================
 * $DASSGN fallback and process teardown -- called from vms_devtab.c
 * ================================================================ */

int vms_mbx_dassgn(struct vms_proc *proc, uint32_t chan)
{
    struct vms_mbx_chan *ch;

    spin_lock(&proc->chan_lock);
    ch = mbxchan_find_locked(proc, chan);
    if (ch)
        list_del(&ch->list);
    spin_unlock(&proc->chan_lock);

    if (!ch)
        return -ENOENT;

    mbx_put(ch->mbx);
    kfree(ch);
    return 0;
}

void vms_mbx_release_all(struct vms_proc *proc)
{
    struct vms_mbx_chan *ch, *tmp;
    LIST_HEAD(doomed);

    spin_lock(&proc->chan_lock);
    list_for_each_entry_safe(ch, tmp, &proc->mbx_channels, list)
        list_move(&ch->list, &doomed);
    spin_unlock(&proc->chan_lock);

    list_for_each_entry_safe(ch, tmp, &doomed, list) {
        list_del(&ch->list);
        mbx_put(ch->mbx);
        kfree(ch);
    }
}
