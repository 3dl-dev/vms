// SPDX-License-Identifier: GPL-2.0
/*
 * vms_tt_linux.c - the terminal PORT driver on Linux: the executive's line
 * discipline (rd vms-f8c, epic vms-4eba).
 *
 * VMS's terminal driver is a CLASS driver (TTDRIVER: type-ahead, echo, line
 * editing, terminators) over a PORT driver per line that only moves bytes. The
 * class driver is src/kernel-core/vms_tt.c, substrate-free. This file is the
 * Linux port: a tty LINE DISCIPLINE. Attaching it to a tty (TIOCSETD) REPLACES
 * n_tty on that line -- canonical mode, ECHO, ICANON, ISIG are simply no longer
 * there to be toggled -- and binding it to a terminal row (VMS_TTIOC_BIND on
 * the tty's fd) hands every received byte to the class driver and every byte
 * the class driver emits to the tty.
 *
 * WHAT THE LINE DISCIPLINE ALSO SERVES. A process that still reads the
 * terminal with read(2) on its fd -- an image whose CRTL reads stdin -- is
 * served BY THE CLASS DRIVER: read(2) here is a VMS terminal read (standard
 * terminators, echo as consumed, line editing), returning the line with a LF
 * where the RETURN was, and 0 (end of file) for a ^Z on an empty line. write(2)
 * goes out through the class driver with LF -> CR LF. The executive owns the
 * line either way; $QIO (VMS_IOCTL_TT_READ) is the full interface.
 *
 * OUTPUT. The port keeps a ring the tty is fed from as it has room
 * (write_wakeup). Echo arrives from the receive path and never waits: a full
 * ring loses the echo, never the input. A process-context write waits for room.
 *
 * LINE DISCIPLINE NUMBER. Linux numbers line disciplines from a fixed table
 * (include/uapi/linux/tty.h). OVMX takes N_DEVELOPMENT ("manual out-of-tree
 * testing"), which no OVMX substrate component uses; labelled here so nobody
 * mistakes it for a number Linux assigned to VMS.
 *
 * Clean-room (AGENTS.md Rule 8): OVMX's own code over the public Linux tty
 * line-discipline API (Documentation/driver-api/tty/tty_ldisc.rst). No Linux
 * n_tty source is copied; no VSI/HPE source or binary.
 */
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_ldisc.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>

#include "vms_internal.h"
#include "vms_ioctl.h"
#include "vms_tt.h"

#define VMS_N_TT        N_DEVELOPMENT
#define VMS_PORT_RING   8192

struct vms_ttport {
	struct tty_struct *tty;
	struct vms_tt *tt;          /* NULL until VMS_TTIOC_BIND */
	struct mutex bind_lock;
	spinlock_t olock;
	u8 ring[VMS_PORT_RING];
	u32 head, len;
	wait_queue_head_t owq;      /* writers waiting for ring room */
	struct work_struct tx_work; /* write_wakeup -> push, outside the
				     * driver's own lock (write_wakeup may be
				     * called with the uart port lock held) */
};

/* Push ring bytes into the tty while it has room. Caller holds olock. */
static void port_push_locked(struct vms_ttport *p)
{
	struct tty_struct *tty = p->tty;

	while (p->len) {
		u32 chunk = min_t(u32, p->len, VMS_PORT_RING - p->head);
		int room = tty_write_room(tty);
		int n;

		if (room <= 0)
			break;
		n = tty->ops->write(tty, p->ring + p->head, min_t(u32, chunk, room));
		if (n <= 0)
			break;
		p->head = (p->head + n) % VMS_PORT_RING;
		p->len -= n;
	}
	if (p->len)
		set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
	else
		clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
}

/* Append what fits; returns how many bytes were taken. */
static size_t port_queue(struct vms_ttport *p, const u8 *buf, size_t n)
{
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&p->olock, flags);
	for (i = 0; i < n && p->len < VMS_PORT_RING; i++) {
		p->ring[(p->head + p->len) % VMS_PORT_RING] = buf[i];
		p->len++;
	}
	port_push_locked(p);
	spin_unlock_irqrestore(&p->olock, flags);
	return i;
}

/* class driver -> port: echo; never sleeps */
static void port_xmit(void *port, const uint8_t *buf, size_t n)
{
	(void)port_queue(port, buf, n);
}

/* class driver -> port: process-context output; waits for room */
static int port_write(void *port, const uint8_t *buf, size_t n)
{
	struct vms_ttport *p = port;

	while (n) {
		size_t k = port_queue(p, buf, n);

		buf += k;
		n -= k;
		if (n && wait_event_interruptible(p->owq, READ_ONCE(p->len) < VMS_PORT_RING))
			return -EINTR;
	}
	return 0;
}

/* An out-of-band ^Y / ^C with no AST armed (rd vms-f0fb lands the ASTs): the
 * substrate interrupt OVMX's DCL has always taken them as -- SIGINT for ^Y,
 * SIGQUIT for ^C (dcl_main.c) -- to the line's foreground process group. */
static void port_interrupt(void *port, uint8_t ch)
{
	struct vms_ttport *p = port;
	struct pid *pgrp = tty_get_pgrp(p->tty);

	if (pgrp) {
		kill_pgrp(pgrp, ch == 0x19 ? SIGINT : SIGQUIT, 1);
		put_pid(pgrp);
	}
}

static void vms_ttport_tx_work(struct work_struct *w);

static const struct vms_tt_port_ops vms_ttport_ops = {
	.xmit      = port_xmit,
	.write     = port_write,
	.interrupt = port_interrupt,
};

/* ------------------------------------------------------------------ */

static int vms_ldisc_open(struct tty_struct *tty)
{
	struct vms_ttport *p;

	if (!tty->ops->write)
		return -EOPNOTSUPP;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->tty = tty;
	mutex_init(&p->bind_lock);
	spin_lock_init(&p->olock);
	init_waitqueue_head(&p->owq);
	INIT_WORK(&p->tx_work, vms_ttport_tx_work);
	tty->disc_data = p;
	tty->receive_room = 65536;
	return 0;
}

static void vms_ldisc_close(struct tty_struct *tty)
{
	struct vms_ttport *p = tty->disc_data;

	if (!p)
		return;
	mutex_lock(&p->bind_lock);
	if (p->tt) {
		vms_tt_detach(p->tt);
		p->tt = NULL;
	}
	mutex_unlock(&p->bind_lock);
	cancel_work_sync(&p->tx_work);
	tty->disc_data = NULL;
	kfree(p);
}

static void vms_ldisc_hangup(struct tty_struct *tty)
{
	struct vms_ttport *p = tty->disc_data;

	if (!p)
		return;
	mutex_lock(&p->bind_lock);
	if (p->tt) {
		vms_tt_detach(p->tt);      /* outstanding reads end SS$_HANGUP */
		p->tt = NULL;
	}
	mutex_unlock(&p->bind_lock);
	wake_up_interruptible(&p->owq);
}

static size_t vms_ldisc_receive_buf2(struct tty_struct *tty, const u8 *cp,
				     const u8 *fp, size_t count)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt *tt = p ? READ_ONCE(p->tt) : NULL;
	size_t i, start = 0;

	if (!tt)
		return count;              /* unbound: nobody owns the line yet */
	if (!fp) {
		vms_tt_receive(tt, cp, count);
		return count;
	}
	/* hand over runs of good characters; drop framing/parity errors */
	for (i = 0; i < count; i++) {
		if (fp[i] != TTY_NORMAL) {
			if (i > start)
				vms_tt_receive(tt, cp + start, i - start);
			start = i + 1;
		}
	}
	if (count > start)
		vms_tt_receive(tt, cp + start, count - start);
	return count;
}

static void vms_ldisc_receive_buf(struct tty_struct *tty, const u8 *cp,
				  const u8 *fp, size_t count)
{
	(void)vms_ldisc_receive_buf2(tty, cp, fp, count);
}

static void vms_ttport_tx_work(struct work_struct *w)
{
	struct vms_ttport *p = container_of(w, struct vms_ttport, tx_work);
	unsigned long flags;

	spin_lock_irqsave(&p->olock, flags);
	port_push_locked(p);
	spin_unlock_irqrestore(&p->olock, flags);
	wake_up_interruptible(&p->owq);
}

static void vms_ldisc_write_wakeup(struct tty_struct *tty)
{
	struct vms_ttport *p = tty->disc_data;

	if (p)
		schedule_work(&p->tx_work);
}

/* read(2): a VMS terminal read, served by the class driver. */
static ssize_t vms_ldisc_read(struct tty_struct *tty, struct file *file, u8 *buf,
			      size_t nr, void **cookie, unsigned long offset)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt_read_req rq;
	struct vms_tt_read_result r;
	struct vms_tt *tt;
	u8 *line;
	ssize_t ret;

	(void)file; (void)cookie; (void)offset;
	if (!p || nr == 0)
		return 0;
	tt = READ_ONCE(p->tt);
	if (!tt)
		return -EIO;               /* no terminal row owns this line */
	line = kmalloc(VMS_TT_LINE_MAX, GFP_KERNEL);
	if (!line)
		return -ENOMEM;

	memset(&rq, 0, sizeof(rq));
	/* leave room for the LF that stands for the RETURN */
	rq.bufsz = nr > 1 ? min_t(size_t, nr - 1, VMS_TT_LINE_MAX) : 1;
	if (vms_tt_read(tt, &rq, line, &r) == -EINTR && r.count == 0) {
		kfree(line);
		return -ERESTARTSYS;
	}
	if (r.status == SS__HANGUP) {
		kfree(line);
		return 0;
	}
	ret = r.count;
	memcpy(buf, line, r.count);
	if (r.termsz && r.term == 0x1A && r.count == 0)
		ret = 0;                   /* ^Z on an empty line: end of file */
	else if (r.termsz && (r.term == 0x0D || r.term == 0x0A) && ret < (ssize_t)nr)
		buf[ret++] = '\n';
	kfree(line);
	return ret;
}

/* write(2): out through the class driver, LF -> CR LF. */
static ssize_t vms_ldisc_write(struct tty_struct *tty, struct file *file,
			       const u8 *buf, size_t nr)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt *tt;
	int rc;

	(void)file;
	if (!p)
		return -EIO;
	tt = READ_ONCE(p->tt);
	if (tt) {
		rc = vms_tt_write(tt, buf, nr, 1);
	} else {
		/* unbound (boot, before the console is bound): cooked passthrough */
		size_t i;

		rc = 0;
		for (i = 0; i < nr && !rc; i++) {
			if (buf[i] == '\n')
				rc = port_write(p, (const u8 *)"\r", 1);
			if (!rc)
				rc = port_write(p, buf + i, 1);
		}
	}
	return rc ? -ERESTARTSYS : (ssize_t)nr;
}

static __poll_t vms_ldisc_poll(struct tty_struct *tty, struct file *file,
			       struct poll_table_struct *wait)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt *tt = p ? READ_ONCE(p->tt) : NULL;
	__poll_t mask = 0;

	poll_wait(file, &tty->read_wait, wait);
	poll_wait(file, &tty->write_wait, wait);
	if (tty_hung_up_p(file))
		mask |= EPOLLHUP;
	if (tt && vms_tt_readable(tt))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!p || READ_ONCE(p->len) < VMS_PORT_RING)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static int vms_ldisc_ioctl(struct tty_struct *tty, unsigned int cmd,
			   unsigned long arg)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt_bind_args a;
	struct vms_device *dev;
	struct vms_tt *tt;

	if (cmd != VMS_TTIOC_BIND)
		return n_tty_ioctl_helper(tty, cmd, arg);
	if (!p)
		return -EIO;
	/* Binding a line to a terminal unit is the system's business, not a
	 * user's: the same bar the executive's other terminal-row operations
	 * (VMS_IOCTL_TERM_CREATE) hold their callers to. */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	a.devnam[sizeof(a.devnam) - 1] = '\0';

	mutex_lock(&p->bind_lock);
	dev = vms_devtab_find_terminal(a.devnam);
	if (!dev) {
		a.status = SS__NOSUCHDEV;
	} else if (p->tt) {
		a.status = SS__DEVALLOC;        /* this line is already bound */
	} else {
		tt = vms_tt_attach(dev, &vms_ttport_ops, p);
		if (!tt) {
			a.status = SS__DEVALLOC;    /* that unit already has a port */
		} else {
			WRITE_ONCE(p->tt, tt);
			a.status = SS__NORMAL;
		}
	}
	mutex_unlock(&p->bind_lock);
	if (copy_to_user((void __user *)arg, &a, sizeof(a)))
		return -EFAULT;
	return 0;
}

static struct tty_ldisc_ops vms_tt_ldisc = {
	.owner          = THIS_MODULE,
	.num            = VMS_N_TT,
	.name           = "vms_tt",
	.open           = vms_ldisc_open,
	.close          = vms_ldisc_close,
	.hangup         = vms_ldisc_hangup,
	.read           = vms_ldisc_read,
	.write          = vms_ldisc_write,
	.ioctl          = vms_ldisc_ioctl,
	.poll           = vms_ldisc_poll,
	.receive_buf    = vms_ldisc_receive_buf,
	.receive_buf2   = vms_ldisc_receive_buf2,
	.write_wakeup   = vms_ldisc_write_wakeup,
};

int vms_tt_linux_init(void)
{
	int rc = tty_register_ldisc(&vms_tt_ldisc);

	if (rc)
		pr_err("vms: terminal line discipline %d not registered (%d)\n",
		       VMS_N_TT, rc);
	return rc;
}

void vms_tt_linux_exit(void)
{
	tty_unregister_ldisc(&vms_tt_ldisc);
}
