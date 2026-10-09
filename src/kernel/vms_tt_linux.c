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
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>

#include "vms_internal.h"
#include "vms_ioctl.h"
#include "vms_tt.h"
#include "vms_prot.h"

#define VMS_N_TT        N_DEVELOPMENT
#define VMS_PORT_RING   8192

/*
 * One per tty running the executive's line discipline. Two holders keep it
 * alive: the tty (from open until close) and, while the line is bound, the
 * class-driver instance (released through vms_ttport_ops.release when its last
 * reference goes -- a $QIO read can outlive the line). `dead` (under olock)
 * says the tty is closed or hung up: from then on no op touches it.
 */
struct vms_ttport {
	struct tty_struct *tty;
	struct vms_tt *tt;          /* NULL until VMS_TTIOC_BIND; under bind_lock */
	char devnam[VMS_DEVNAM_SIZE]; /* the unit it is bound to */
	struct mutex bind_lock;
	refcount_t refs;
	spinlock_t olock;
	int dead;                   /* under olock */
	int hungup;                 /* hangup came first: a console re-open follows */
	u8 ring[VMS_PORT_RING];
	u32 head, len;
	wait_queue_head_t owq;      /* writers waiting for ring room */
	struct work_struct tx_work; /* write_wakeup -> push, outside the
				     * driver's own lock (write_wakeup may be
				     * called with the uart port lock held) */
};

static void port_put(struct vms_ttport *p)
{
	if (refcount_dec_and_test(&p->refs))
		kfree(p);
}

/* The bound class driver, referenced, or NULL. */
static struct vms_tt *port_tt_get(struct vms_ttport *p)
{
	struct vms_tt *tt;

	mutex_lock(&p->bind_lock);
	tt = p->tt;
	if (tt)
		vms_tt_get(tt);
	mutex_unlock(&p->bind_lock);
	return tt;
}

/*
 * THE CONSOLE OUTLIVES ITS SESSIONS. When a session leader whose controlling
 * terminal is the console exits, Linux hangs the console up and RE-OPENS its
 * line discipline (__tty_hangup -> tty_ldisc_hangup(tty, reinit)): our close,
 * then our open, on the same tty. A VMS terminal unit does not hang up because
 * one session on it ended, so for a line that is not a pty the class-driver
 * instance is PARKED across that close (still bound to its unit, its
 * type-ahead and outstanding reads intact) and the next open of the same line
 * -- or the next bind naming the unit -- takes it back (vms_tt_set_port).
 * A pty's hangup is the far end going away: that ends the binding.
 */
#define VMS_TT_PARK 8
static struct {
	struct tty_driver *driver;
	int index;
	struct vms_tt *tt;          /* referenced */
	char devnam[VMS_DEVNAM_SIZE];
} vms_tt_parked[VMS_TT_PARK];
static DEFINE_MUTEX(vms_tt_park_lock);

static int port_is_pty(struct tty_struct *tty)
{
	return tty->driver->type == TTY_DRIVER_TYPE_PTY;
}

/* The unit name as the table keys it: no leading underscore. */
static const char *port_unit(const char *devnam)
{
	return devnam[0] == '_' ? devnam + 1 : devnam;
}

/* Park `tt` (the attachment's reference moves into the lot). 0 or -ENOSPC. */
static int port_park(struct tty_struct *tty, struct vms_tt *tt, const char *devnam)
{
	int i, rc = -ENOSPC;

	mutex_lock(&vms_tt_park_lock);
	for (i = 0; i < VMS_TT_PARK; i++) {
		if (!vms_tt_parked[i].tt) {
			vms_tt_parked[i].driver = tty->driver;
			vms_tt_parked[i].index = tty->index;
			vms_tt_parked[i].tt = tt;
			strscpy(vms_tt_parked[i].devnam, port_unit(devnam), VMS_DEVNAM_SIZE);
			rc = 0;
			break;
		}
	}
	mutex_unlock(&vms_tt_park_lock);
	return rc;
}

/* Take back what was parked for this line (by_tty) or for unit `devnam`. */
static struct vms_tt *port_unpark(struct tty_struct *tty, const char *devnam,
				  char *devnam_out)
{
	struct vms_tt *tt = NULL;
	int i;

	mutex_lock(&vms_tt_park_lock);
	for (i = 0; i < VMS_TT_PARK; i++) {
		if (!vms_tt_parked[i].tt)
			continue;
		if ((!tty && !devnam) ||
		    (tty && vms_tt_parked[i].driver == tty->driver &&
		     vms_tt_parked[i].index == tty->index) ||
		    (devnam && strncasecmp(vms_tt_parked[i].devnam, port_unit(devnam),
					   VMS_DEVNAM_SIZE) == 0)) {
			tt = vms_tt_parked[i].tt;
			if (devnam_out)
				strscpy(devnam_out, vms_tt_parked[i].devnam, VMS_DEVNAM_SIZE);
			vms_tt_parked[i].tt = NULL;
			break;
		}
	}
	mutex_unlock(&vms_tt_park_lock);
	return tt;
}

/* Push ring bytes into the tty while it has room. Caller holds olock. */
static void port_push_locked(struct vms_ttport *p)
{
	struct tty_struct *tty = p->tty;

	if (p->dead) {
		p->len = 0;
		return;
	}
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

/* Append what fits; returns how many bytes were taken (all of them, dropped,
 * once the line is dead). */
static size_t port_queue(struct vms_ttport *p, const u8 *buf, size_t n)
{
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&p->olock, flags);
	if (p->dead) {
		spin_unlock_irqrestore(&p->olock, flags);
		return n;
	}
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
		size_t k;

		if (READ_ONCE(p->dead))
			return -EIO;
		k = port_queue(p, buf, n);
		buf += k;
		n -= k;
		if (n && wait_event_interruptible(p->owq,
				READ_ONCE(p->len) < VMS_PORT_RING || READ_ONCE(p->dead)))
			return -EINTR;
	}
	return 0;
}

/* An out-of-band ^Y / ^C with no AST armed (rd vms-f0fb lands the ASTs): the
 * substrate interrupt OVMX's DCL has always taken them as -- SIGINT for ^Y,
 * SIGQUIT for ^C (dcl_main.c) -- to the line's foreground process group.
 * Called only from the receive path, while the tty is alive. */
static void port_interrupt(void *port, uint8_t ch)
{
	struct vms_ttport *p = port;
	struct pid *pgrp;

	if (READ_ONCE(p->dead))
		return;
	pgrp = tty_get_pgrp(p->tty);
	if (pgrp) {
		kill_pgrp(pgrp, ch == 0x19 ? SIGINT : SIGQUIT, 1);
		put_pid(pgrp);
	}
}

/* TTSYNC ^S / ^Q: the substrate tty's own output stop (stop_tty/start_tty). */
static void port_flow(void *port, int stop)
{
	struct vms_ttport *p = port;

	if (READ_ONCE(p->dead))
		return;
	if (stop)
		stop_tty(p->tty);
	else
		start_tty(p->tty);
}

static void port_release(void *port)
{
	port_put(port);
}

static void vms_ttport_tx_work(struct work_struct *w);

static const struct vms_tt_port_ops vms_ttport_ops = {
	.xmit      = port_xmit,
	.write     = port_write,
	.interrupt = port_interrupt,
	.flow      = port_flow,
	.release   = port_release,
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
	refcount_set(&p->refs, 1);           /* the tty's */
	spin_lock_init(&p->olock);
	init_waitqueue_head(&p->owq);
	INIT_WORK(&p->tx_work, vms_ttport_tx_work);
	tty->disc_data = p;
	tty->receive_room = 65536;

	/* a console line re-opened after a session hangup: same unit */
	if (!port_is_pty(tty)) {
		struct vms_tt *tt = port_unpark(tty, NULL, p->devnam);

		if (tt) {
			refcount_inc(&p->refs);          /* the class driver's hold */
			p->tt = tt;
			vms_tt_set_port(tt, &vms_ttport_ops, p);
		}
	}
	return 0;
}

/* The line is going away (close) or has gone (hangup): it carries nothing
 * more, and the class driver lets go of it -- outstanding reads end
 * SS$_HANGUP. Idempotent. */
static void port_kill(struct vms_ttport *p)
{
	unsigned long flags;
	struct vms_tt *tt;

	spin_lock_irqsave(&p->olock, flags);
	p->dead = 1;
	p->len = 0;
	spin_unlock_irqrestore(&p->olock, flags);
	wake_up_interruptible(&p->owq);

	mutex_lock(&p->bind_lock);
	tt = p->tt;
	p->tt = NULL;
	mutex_unlock(&p->bind_lock);
	if (!tt)
		return;
	if (p->hungup && !port_is_pty(p->tty) && port_park(p->tty, tt, p->devnam) == 0)
		return;                      /* the re-open takes it back */
	vms_tt_detach(tt);                   /* drops the class driver's hold
					      * on p via port_release, later */
}

static void vms_ldisc_close(struct tty_struct *tty)
{
	struct vms_ttport *p = tty->disc_data;

	if (!p)
		return;
	port_kill(p);
	cancel_work_sync(&p->tx_work);
	tty->disc_data = NULL;
	port_put(p);                         /* the tty's */
}

static void vms_ldisc_hangup(struct tty_struct *tty)
{
	struct vms_ttport *p = tty->disc_data;

	if (!p)
		return;
	if (!port_is_pty(tty)) {        /* a session ended, not the line */
		/* the close (and re-open) that follows parks and recovers the
		 * binding. A read(2) blocked on the line holds the line-discipline
		 * reference that close must take: end it, as a hangup ends it. */
		struct vms_tt *tt = port_tt_get(p);

		p->hungup = 1;
		if (tt) {
			vms_tt_kick_ldisc(tt);
			vms_tt_release(tt);
		}
		return;
	}
	port_kill(p);
}

static void port_receive(struct vms_ttport *p, struct tty_struct *tty,
			 const u8 *cp, const u8 *fp, size_t count)
{
	struct vms_tt *tt = port_tt_get(p);
	size_t i, start = 0;

	if (!tt)
		return;                    /* unbound: nobody owns the line yet */
	if (!fp) {
		vms_tt_receive(tt, cp, count);
	} else {
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
	}
	vms_tt_release(tt);
	wake_up_interruptible_poll(&tty->read_wait, EPOLLIN | EPOLLRDNORM);
}

static size_t vms_ldisc_receive_buf2(struct tty_struct *tty, const u8 *cp,
				     const u8 *fp, size_t count)
{
	struct vms_ttport *p = tty->disc_data;

	if (p)
		port_receive(p, tty, cp, fp, count);
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
	int rc;

	(void)file; (void)cookie; (void)offset;
	if (!p || nr == 0)
		return 0;
	tt = port_tt_get(p);
	if (!tt)
		return -EIO;               /* no terminal row owns this line */
	line = kmalloc(VMS_TT_LINE_MAX, GFP_KERNEL);
	if (!line) {
		vms_tt_release(tt);
		return -ENOMEM;
	}

	memset(&rq, 0, sizeof(rq));
	/* leave room for the LF that stands for the RETURN */
	rq.bufsz = nr > 1 ? min_t(size_t, nr - 1, VMS_TT_LINE_MAX) : 1;
	rq.owner = current;               /* a signal suspends; a restart resumes */
	rq.ldisc = 1;
	rc = vms_tt_read(tt, &rq, line, &r);
	vms_tt_release(tt);
	if (rc == -ERESTARTSYS) {
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
	tt = port_tt_get(p);
	if (tt) {
		rc = vms_tt_write(tt, buf, nr, 1);
		vms_tt_release(tt);
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
	if (rc == -EINTR)
		return -ERESTARTSYS;
	return rc ? rc : (ssize_t)nr;
}

static __poll_t vms_ldisc_poll(struct tty_struct *tty, struct file *file,
			       struct poll_table_struct *wait)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt *tt = p ? port_tt_get(p) : NULL;
	__poll_t mask = 0;

	poll_wait(file, &tty->read_wait, wait);
	poll_wait(file, &tty->write_wait, wait);
	if (tty_hung_up_p(file))
		mask |= EPOLLHUP;
	if (tt) {
		if (vms_tt_readable(tt))
			mask |= EPOLLIN | EPOLLRDNORM;
		vms_tt_release(tt);
	}
	if (!p || READ_ONCE(p->len) < VMS_PORT_RING)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static int vms_ldisc_ioctl(struct tty_struct *tty, unsigned int cmd,
			   unsigned long arg)
{
	struct vms_ttport *p = tty->disc_data;
	struct vms_tt_bind_args a;
	struct vms_tt *tt = NULL;

	if (cmd != VMS_TTIOC_BIND)
		return n_tty_ioctl_helper(tty, cmd, arg);
	if (!p)
		return -EIO;
	if (copy_from_user(&a, (void __user *)arg, sizeof(a)))
		return -EFAULT;
	a.devnam[sizeof(a.devnam) - 1] = '\0';

	mutex_lock(&p->bind_lock);
	if (p->tt) {
		a.status = SS__DEVALLOC;        /* this line is already bound */
	} else if (READ_ONCE(p->dead)) {
		a.status = SS__HANGUP;
	} else {
		/* The executive decides: CMKRNL, through vms_prot.h (rd vms-f8c,
		 * Baron's ruling 2) -- never a substrate capability. */
		refcount_inc(&p->refs);       /* the class driver's hold */
		a.status = vms_tt_bind(vms_proc_find_or_err(), a.devnam,
				       &vms_ttport_ops, p, &tt);
		if (a.status == SS__DEVALLOC) {
			/* the unit's port is parked (its line closed by a
			 * session hangup and not re-opened): the bind takes it
			 * over -- with the same privilege bar */
			struct vms_proc *proc = vms_proc_find_or_err();

			if (proc && (vms_prot_require_priv(proc->cur_privs,
							   VMS_PRV_M_CMKRNL) & 1)) {
				tt = port_unpark(NULL, a.devnam, NULL);
				if (tt) {
					vms_tt_set_port(tt, &vms_ttport_ops, p);
					a.status = SS__NORMAL;
				}
			}
		}
		if (tt) {
			p->tt = tt;
			strscpy(p->devnam, a.devnam, VMS_DEVNAM_SIZE);
		} else {
			refcount_dec(&p->refs);
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
	struct vms_tt *tt;

	while ((tt = port_unpark(NULL, NULL, NULL)) != NULL)
		vms_tt_detach(tt);
	tty_unregister_ldisc(&vms_tt_ldisc);
}
