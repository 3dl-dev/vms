/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_tt_netbsd.c - the terminal PORT driver on NetBSD: the executive's line
 * discipline "vms_tt" (rd vms-f8c, epic vms-4eba). The NetBSD twin of
 * src/kernel/vms_tt_linux.c.
 *
 * VMS's terminal driver is a CLASS driver (TTDRIVER: type-ahead, echo, line
 * editing, terminators) over a PORT driver per line that only moves bytes. The
 * class driver is src/kernel-core/vms_tt.c, the SAME source the Linux vms.ko
 * compiles. This file is the NetBSD port: a tty LINE DISCIPLINE (struct
 * linesw, ttyldisc_attach). Selecting it on a tty (TIOCSLINED "vms_tt")
 * REPLACES termios_disc there -- canonical mode, ECHO, ICANON, ISIG are simply
 * no longer there to be toggled -- and binding it to a terminal row
 * (VMS_TTIOC_BIND on the tty's fd; the executive requires CMKRNL, vms_prot.h)
 * hands every received byte to the class driver and every byte the class
 * driver emits to the tty's output queue.
 *
 * CONTEXTS. A tty driver delivers input through l_rint at interrupt level (the
 * VAX console, gencons, calls it from its receive interrupt), where the class
 * driver's adaptive locks may not be taken. So l_rint only queues the byte in
 * the port (under tty_lock, the tty subsystem's spin mutex) and schedules ONE
 * module-wide soft interrupt; the soft interrupt hands the queued bytes to the
 * class driver in thread context. Output goes onto t_outq under tty_lock and is
 * started with ttstart(), as termios_disc's own ttwrite does.
 *
 * read(2)/write(2) on the line are served BY THE CLASS DRIVER (a VMS terminal
 * read: standard terminators, echo as consumed, line editing; LF for the
 * RETURN, 0 for a ^Z on an empty line). $QIO (VMS_IOCTL_TT_READ on /dev/vms) is
 * the full interface.
 *
 * LIFETIME. Two holders keep a port alive: the tty (l_open..l_close) and, while
 * bound, the class-driver instance (released through ops->release when its last
 * reference goes -- a $QIO read can outlive the line). A port queued for the
 * soft interrupt holds a third reference. `dead' (under tty_lock) says the tty
 * is closed or lost carrier: from then on no op touches it.
 *
 * Clean-room (AGENTS.md Rule 8): OVMX's own code over the public NetBSD
 * line-discipline interface (sys/conf.h struct linesw, tty(4)). No NetBSD
 * termios_disc source is copied; no VSI/HPE source or binary.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/atomic.h>
#include <sys/proc.h>
#include <sys/conf.h>
#include <sys/tty.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/intr.h>
#include <sys/condvar.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/errno.h>
#include <dev/cons.h>          /* cn_tab: which tty is the console */

#include "vms_internal.h"
#include "vms_tt.h"

#define VTT_RX_RING     512          /* bytes queued between rint and the softint */
#define VTT_LDISC_NAME  "vms_tt"

struct vms_ttport_nb {
	struct tty       *tp;
	struct vms_tt    *tt;            /* NULL until bound; under bind_lock */
	kmutex_t          bind_lock;     /* adaptive: bind / kill */
	volatile u_int    refs;
	/* under tty_lock: */
	int               dead;
	int               queued;        /* on vtt_pending */
	int               kill_req;      /* carrier lost at interrupt level */
	u_char            rx[VTT_RX_RING];
	u_int             rx_head, rx_len;
	kcondvar_t        owcv;          /* writers waiting for t_outq room */
	TAILQ_ENTRY(vms_ttport_nb) pend;
	char              devnam[VMS_DEVNAM_SIZE]; /* the unit it is bound to */
};

/*
 * THE CONSOLE OUTLIVES ITS SESSIONS (the Linux port's rule, vms_tt_linux.c).
 * The console's line can be closed under a live system -- a session whose
 * controlling terminal it was ends and its vnode is revoked -- and re-opened
 * by the next one. A VMS console does not hang up because a session ended, so
 * the console's class-driver instance is PARKED across that close and the next
 * open of the console line, or the next bind naming the unit, takes it back
 * (vms_tt_set_port). Only the console: a pty's close is its far end leaving.
 */
static kmutex_t vtt_park_lock;
static struct vms_tt *vtt_parked;              /* referenced */
static char vtt_parked_devnam[VMS_DEVNAM_SIZE];

static int
vtt_is_console(struct tty *tp)
{
	return cn_tab != NULL && tp->t_dev == cn_tab->cn_dev;
}

static TAILQ_HEAD(, vms_ttport_nb) vtt_pending = TAILQ_HEAD_INITIALIZER(vtt_pending);
static void *vtt_sih;
static void vtt_reclaim(struct vms_ttport_nb *p);
static struct vms_tt *vtt_unpark(const char *devnam, char *devnam_out);

static void
port_put(struct vms_ttport_nb *p)
{
	if (atomic_dec_uint_nv(&p->refs) == 0) {
		cv_destroy(&p->owcv);
		mutex_destroy(&p->bind_lock);
		kmem_free(p, sizeof(*p));
	}
}

static struct vms_tt *
port_tt_get(struct vms_ttport_nb *p)
{
	struct vms_tt *tt;

	mutex_enter(&p->bind_lock);
	tt = p->tt;
	if (tt != NULL)
		vms_tt_get(tt);
	mutex_exit(&p->bind_lock);
	return tt;
}

/* Queue `p' for the soft interrupt. Caller holds tty_lock. */
static void
port_pend_locked(struct vms_ttport_nb *p)
{
	if (!p->queued) {
		p->queued = 1;
		atomic_inc_uint(&p->refs);
		TAILQ_INSERT_TAIL(&vtt_pending, p, pend);
	}
}

/* ---- class driver -> port ---------------------------------------- */

/* echo / output that must not sleep: what fits in t_outq, the rest is lost */
static void
port_xmit(void *port, const uint8_t *buf, size_t n)
{
	struct vms_ttport_nb *p = port;
	struct tty *tp;

	mutex_spin_enter(&tty_lock);
	if (!p->dead) {
		tp = p->tp;
		(void)b_to_q(buf, (int)n, &tp->t_outq);
		ttstart(tp);
	}
	mutex_spin_exit(&tty_lock);
}

/* process-context output: waits for room below the high-water mark */
static int
port_write(void *port, const uint8_t *buf, size_t n)
{
	struct vms_ttport_nb *p = port;
	struct tty *tp;
	int error, left, timo = hz / 20 > 0 ? hz / 20 : 1;

	mutex_spin_enter(&tty_lock);
	while (n > 0) {
		if (p->dead) {
			mutex_spin_exit(&tty_lock);
			return -EIO;
		}
		tp = p->tp;
		if (tp->t_outq.c_cc > tp->t_hiwat) {
			ttstart(tp);
			/* the port's own condvar, not t_outcv: the tty may be gone
			 * by the time this wakes (a closed pty), and only `dead'
			 * -- in the port -- may be looked at then. */
			error = cv_timedwait_sig(&p->owcv, &tty_lock, timo);
			if (error != 0 && error != EWOULDBLOCK) {
				mutex_spin_exit(&tty_lock);
				return -EINTR;
			}
			continue;
		}
		left = b_to_q(buf, (int)n, &tp->t_outq);
		buf += n - (size_t)left;
		n = (size_t)left;
		ttstart(tp);
		if (left > 0) {
			error = cv_timedwait_sig(&p->owcv, &tty_lock, timo);
			if (error != 0 && error != EWOULDBLOCK) {
				mutex_spin_exit(&tty_lock);
				return -EINTR;
			}
		}
	}
	mutex_spin_exit(&tty_lock);
	return 0;
}

/* An out-of-band ^Y / ^C with no AST armed (rd vms-f0fb lands the ASTs): the
 * substrate interrupt OVMX's DCL takes them as -- SIGINT for ^Y, SIGQUIT for
 * ^C -- to the line's foreground process group. */
static void
port_interrupt(void *port, uint8_t ch)
{
	struct vms_ttport_nb *p = port;

	mutex_spin_enter(&tty_lock);
	if (!p->dead)
		ttysig(p->tp, TTYSIG_PG1, ch == 0x19 ? SIGINT : SIGQUIT);
	mutex_spin_exit(&tty_lock);
}

/* TTSYNC ^S / ^Q: the tty's output stop flag; ^Q restarts output. */
static void
port_flow(void *port, int stop)
{
	struct vms_ttport_nb *p = port;

	mutex_spin_enter(&tty_lock);
	if (!p->dead) {
		if (stop) {
			SET(p->tp->t_state, TS_TTSTOP);
		} else {
			CLR(p->tp->t_state, TS_TTSTOP);
			ttstart(p->tp);
		}
	}
	mutex_spin_exit(&tty_lock);
}

static void
port_release(void *port)
{
	port_put(port);
}

static const struct vms_tt_port_ops vms_ttport_nb_ops = {
	.xmit      = port_xmit,
	.write     = port_write,
	.interrupt = port_interrupt,
	.flow      = port_flow,
	.release   = port_release,
};

/* The line carries nothing more; the class driver lets go of it (outstanding
 * reads end SS$_HANGUP). Thread context. Idempotent. */
static void
port_kill(struct vms_ttport_nb *p)
{
	struct vms_tt *tt;

	mutex_spin_enter(&tty_lock);
	p->dead = 1;
	p->rx_len = 0;
	cv_broadcast(&p->owcv);
	mutex_spin_exit(&tty_lock);

	mutex_enter(&p->bind_lock);
	tt = p->tt;
	p->tt = NULL;
	mutex_exit(&p->bind_lock);
	if (tt != NULL)
		vms_tt_detach(tt);
}

/* ---- the soft interrupt: received bytes -> class driver ------------ */

static void
vtt_softint(void *arg __unused)
{
	struct vms_ttport_nb *p;
	u_char buf[VTT_RX_RING];
	u_int n, i;
	int kill;

	for (;;) {
		mutex_spin_enter(&tty_lock);
		p = TAILQ_FIRST(&vtt_pending);
		if (p == NULL) {
			mutex_spin_exit(&tty_lock);
			return;
		}
		TAILQ_REMOVE(&vtt_pending, p, pend);
		p->queued = 0;
		kill = p->kill_req;
		p->kill_req = 0;
		n = p->dead ? 0 : p->rx_len;
		for (i = 0; i < n; i++)
			buf[i] = p->rx[(p->rx_head + i) % VTT_RX_RING];
		p->rx_head = (p->rx_head + n) % VTT_RX_RING;
		p->rx_len -= n;
		mutex_spin_exit(&tty_lock);

		if (kill) {
			port_kill(p);
		} else if (n > 0) {
			struct vms_tt *tt = port_tt_get(p);

			if (tt != NULL) {
				vms_tt_receive(tt, buf, n);
				vms_tt_release(tt);
			}
			mutex_spin_enter(&tty_lock);
			if (!p->dead)
				ttwakeup(p->tp);         /* poll/select readers */
			mutex_spin_exit(&tty_lock);
		}
		port_put(p);                         /* the queue's reference */
	}
}

/* ---- struct linesw ----------------------------------------------- */

static int
vtt_open(dev_t dev __unused, struct tty *tp)
{
	struct vms_ttport_nb *p;

	p = kmem_zalloc(sizeof(*p), KM_SLEEP);
	p->tp = tp;
	mutex_init(&p->bind_lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&p->owcv, "vmsttw");
	p->refs = 1;                         /* the tty's */
	mutex_spin_enter(&tty_lock);
	tp->t_sc = p;
	mutex_spin_exit(&tty_lock);
	return 0;
}

/* The console line re-opened: take its parked binding back. Thread context
 * (the open path runs at spltty; the take-back waits, so it runs from the
 * first bind or read instead -- vtt_reclaim). */
static void
vtt_reclaim(struct vms_ttport_nb *p)
{
	struct vms_tt *tt;

	if (!vtt_is_console(p->tp))
		return;
	mutex_enter(&p->bind_lock);
	if (p->tt == NULL && (tt = vtt_unpark(NULL, p->devnam)) != NULL) {
		atomic_inc_uint(&p->refs);   /* the class driver's hold */
		p->tt = tt;
		mutex_exit(&p->bind_lock);
		vms_tt_set_port(tt, &vms_ttport_nb_ops, p);
		return;
	}
	mutex_exit(&p->bind_lock);
}

static int
vtt_close(struct tty *tp, int flag __unused)
{
	struct vms_ttport_nb *p;
	struct vms_tt *tt;

	mutex_spin_enter(&tty_lock);
	p = tp->t_sc;
	tp->t_sc = NULL;
	mutex_spin_exit(&tty_lock);
	if (p == NULL)
		return 0;
	if (vtt_is_console(tp)) {
		/* park the console's binding for its next open */
		mutex_spin_enter(&tty_lock);
		p->dead = 1;
		p->rx_len = 0;
		cv_broadcast(&p->owcv);
		mutex_spin_exit(&tty_lock);
		mutex_enter(&p->bind_lock);
		tt = p->tt;
		p->tt = NULL;
		mutex_exit(&p->bind_lock);
		if (tt != NULL) {
			mutex_enter(&vtt_park_lock);
			if (vtt_parked == NULL) {
				vtt_parked = tt;
				strlcpy(vtt_parked_devnam, p->devnam, sizeof(vtt_parked_devnam));
				tt = NULL;
			}
			mutex_exit(&vtt_park_lock);
			if (tt != NULL)
				vms_tt_detach(tt);
		}
	} else {
		port_kill(p);
	}
	port_put(p);                         /* the tty's */
	return 0;
}

/* Take back the parked console binding (for unit `devnam`, or any if NULL). */
static struct vms_tt *
vtt_unpark(const char *devnam, char *devnam_out)
{
	struct vms_tt *tt = NULL;
	const char *u;

	mutex_enter(&vtt_park_lock);
	if (vtt_parked != NULL) {
		u = devnam != NULL && devnam[0] == '_' ? devnam + 1 : devnam;
		if (u == NULL || strcasecmp(u, vtt_parked_devnam) == 0) {
			tt = vtt_parked;
			vtt_parked = NULL;
			if (devnam_out != NULL)
				strlcpy(devnam_out, vtt_parked_devnam, VMS_DEVNAM_SIZE);
		}
	}
	mutex_exit(&vtt_park_lock);
	return tt;
}

static int
vtt_rint(int c, struct tty *tp)
{
	struct vms_ttport_nb *p;

	if (c & TTY_ERRORMASK)
		return 0;                    /* framing / parity error: dropped */
	mutex_spin_enter(&tty_lock);
	p = tp->t_sc;
	if (p == NULL || p->dead) {
		mutex_spin_exit(&tty_lock);
		return 0;
	}
	if (p->rx_len < VTT_RX_RING) {
		p->rx[(p->rx_head + p->rx_len) % VTT_RX_RING] = (u_char)(c & TTY_CHARMASK);
		p->rx_len++;
	}
	port_pend_locked(p);
	mutex_spin_exit(&tty_lock);
	softint_schedule(vtt_sih);
	return 0;
}

static int
vtt_modem(struct tty *tp, int flag)
{
	struct vms_ttport_nb *p;
	int r = ttymodem(tp, flag);

	if (flag == 0) {
		/* carrier lost: the line is gone (a pty whose master closed). The
		 * detach runs in thread context, through the soft interrupt. */
		mutex_spin_enter(&tty_lock);
		p = tp->t_sc;
		if (p != NULL && !p->dead) {
			p->dead = 1;
			p->rx_len = 0;
			cv_broadcast(&p->owcv);
			p->kill_req = 1;
			port_pend_locked(p);
		}
		mutex_spin_exit(&tty_lock);
		softint_schedule(vtt_sih);
	}
	return r;
}

static int
vtt_start(struct tty *tp)
{
	return ttstart(tp);
}

static int
vtt_read(struct tty *tp, struct uio *uio, int flag __unused)
{
	struct vms_ttport_nb *p = tp->t_sc;
	struct vms_tt_read_req rq;
	struct vms_tt_read_result r;
	struct vms_tt *tt;
	uint8_t *line;
	size_t nr = uio->uio_resid, ret;
	int rc, error;

	if (p == NULL || nr == 0)
		return 0;
	vtt_reclaim(p);
	tt = port_tt_get(p);
	if (tt == NULL)
		return EIO;                  /* no terminal row owns this line */
	line = kmem_alloc(VMS_TT_LINE_MAX + 1, KM_SLEEP);

	memset(&rq, 0, sizeof(rq));
	rq.bufsz = nr > 1 ? (nr - 1 < VMS_TT_LINE_MAX ? nr - 1 : VMS_TT_LINE_MAX) : 1;
	rq.owner = curlwp;                /* a signal suspends; a restart resumes */
	rq.ldisc = 1;
	rc = vms_tt_read(tt, &rq, line, &r);
	vms_tt_release(tt);
	if (rc == -ERESTARTSYS) {
		kmem_free(line, VMS_TT_LINE_MAX + 1);
		return ERESTART;
	}
	if (r.status == SS__HANGUP) {
		kmem_free(line, VMS_TT_LINE_MAX + 1);
		return 0;
	}
	ret = r.count;
	if (r.termsz && r.term == 0x1A && r.count == 0)
		ret = 0;                     /* ^Z on an empty line: end of file */
	else if (r.termsz && (r.term == 0x0D || r.term == 0x0A) && ret < nr)
		line[ret++] = '\n';
	error = ret ? uiomove(line, ret, uio) : 0;
	kmem_free(line, VMS_TT_LINE_MAX + 1);
	return error;
}

static int
vtt_write(struct tty *tp, struct uio *uio, int flag __unused)
{
	struct vms_ttport_nb *p = tp->t_sc;
	struct vms_tt *tt;
	uint8_t chunk[128];
	size_t k;
	int error = 0, rc = 0;

	if (p == NULL)
		return EIO;
	vtt_reclaim(p);
	tt = port_tt_get(p);
	while (uio->uio_resid > 0 && rc == 0) {
		k = uio->uio_resid < sizeof(chunk) ? uio->uio_resid : sizeof(chunk);
		error = uiomove(chunk, k, uio);
		if (error)
			break;
		if (tt != NULL) {
			rc = vms_tt_write(tt, chunk, k, 1);
		} else {
			/* unbound (boot, before the console is bound): cooked */
			size_t i;

			for (i = 0; i < k && rc == 0; i++) {
				if (chunk[i] == '\n')
					rc = port_write(p, (const uint8_t *)"\r", 1);
				if (rc == 0)
					rc = port_write(p, chunk + i, 1);
			}
		}
	}
	if (tt != NULL)
		vms_tt_release(tt);
	if (error)
		return error;
	if (rc == -EINTR)
		return ERESTART;
	return rc ? EIO : 0;
}

static int
vtt_poll(struct tty *tp, int events, struct lwp *l)
{
	struct vms_ttport_nb *p = tp->t_sc;
	struct vms_tt *tt;
	int revents = 0;

	if (p != NULL)
		vtt_reclaim(p);
	tt = p != NULL ? port_tt_get(p) : NULL;

	if (tt != NULL) {
		if ((events & (POLLIN | POLLRDNORM)) && vms_tt_readable(tt))
			revents |= events & (POLLIN | POLLRDNORM);
		vms_tt_release(tt);
	}
	mutex_spin_enter(&tty_lock);
	if ((events & (POLLOUT | POLLWRNORM)) && tp->t_outq.c_cc <= tp->t_lowat)
		revents |= events & (POLLOUT | POLLWRNORM);
	if ((events & POLLHUP) && (p == NULL || p->dead ||
	    (!ISSET(tp->t_state, TS_CARR_ON) && !ISSET(tp->t_cflag, CLOCAL))))
		revents |= POLLHUP;
	if (revents == 0) {
		if (events & (POLLIN | POLLHUP | POLLRDNORM))
			selrecord(l, &tp->t_rsel);
		if (events & (POLLOUT | POLLWRNORM))
			selrecord(l, &tp->t_wsel);
	}
	mutex_spin_exit(&tty_lock);
	return revents;
}

static int
vtt_ioctl(struct tty *tp, u_long cmd, void *data, int flag __unused,
    struct lwp *l __unused)
{
	struct vms_ttport_nb *p = tp->t_sc;
	struct vms_tt_bind_args *a;
	struct vms_tt *tt = NULL;

	if (cmd != VMS_TTIOC_BIND)
		return EPASSTHROUGH;         /* ttioctl: TIOCSLINED & co */
	if (p == NULL)
		return EIO;
	vtt_reclaim(p);
	/* _IOWR: `data' is the framework's kernel copy of the caller's struct */
	a = (struct vms_tt_bind_args *)data;
	a->devnam[sizeof(a->devnam) - 1] = '\0';

	mutex_enter(&p->bind_lock);
	if (p->tt != NULL) {
		a->status = SS__DEVALLOC;    /* this line is already bound */
	} else if (p->dead) {
		a->status = SS__HANGUP;
	} else {
		/* The executive decides: CMKRNL, through vms_prot.h (rd vms-f8c,
		 * Baron's ruling 2) -- never a substrate capability. */
		atomic_inc_uint(&p->refs);  /* the class driver's hold */
		a->status = vms_tt_bind(vms_netbsd_proc_current(), a->devnam,
		    &vms_ttport_nb_ops, p, &tt);
		if (tt != NULL) {
			p->tt = tt;
			strlcpy(p->devnam, a->devnam[0] == '_' ? a->devnam + 1 : a->devnam,
			    sizeof(p->devnam));
		} else {
			atomic_dec_uint(&p->refs);
		}
	}
	mutex_exit(&p->bind_lock);
	return 0;
}

static struct linesw vms_tt_disc = {
	.l_name  = VTT_LDISC_NAME,
	.l_open  = vtt_open,
	.l_close = vtt_close,
	.l_read  = vtt_read,
	.l_write = vtt_write,
	.l_ioctl = vtt_ioctl,
	.l_rint  = vtt_rint,
	.l_start = vtt_start,
	.l_modem = vtt_modem,
	.l_poll  = vtt_poll,
};

int
vms_tt_netbsd_init(void)
{
	int error;

	mutex_init(&vtt_park_lock, MUTEX_DEFAULT, IPL_NONE);
	vtt_sih = softint_establish(SOFTINT_SERIAL | SOFTINT_MPSAFE, vtt_softint, NULL);
	if (vtt_sih == NULL)
		return ENOMEM;
	error = ttyldisc_attach(&vms_tt_disc);
	if (error != 0) {
		softint_disestablish(vtt_sih);
		vtt_sih = NULL;
	}
	return error;
}

void
vms_tt_netbsd_fini(void)
{
	struct vms_tt *tt;

	if (vtt_sih == NULL)
		return;
	if ((tt = vtt_unpark(NULL, NULL)) != NULL)
		vms_tt_detach(tt);
	(void)ttyldisc_detach(&vms_tt_disc);
	softint_disestablish(vtt_sih);
	vtt_sih = NULL;
	mutex_destroy(&vtt_park_lock);
}
