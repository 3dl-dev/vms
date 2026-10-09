// SPDX-License-Identifier: GPL-2.0
/*
 * vms_tt.c - the executive terminal CLASS driver (rd vms-f8c, epic vms-4eba).
 *
 * See vms_tt.h for the split (class driver here, port driver = the substrate
 * tty behind the executive's line discipline) and docs/design-terminal-
 * driver.md for the design. This file is substrate-free: it names no
 * <linux/...> or NetBSD symbol, only the exec_* shim, so the NetBSD port
 * binding compiles the same source.
 *
 * WHAT A PERSON AT THE KEYBOARD SEES (the keystroke oracle, rd vms-370,
 * docs/oracle/keystroke/): on VMS a character typed while nobody is reading
 * the terminal is NOT echoed -- it waits in the type-ahead buffer, and is
 * echoed only when a read consumes it, after that read's prompt. OVMX used
 * to echo it on receipt (the substrate's n_tty), the first thing anyone who
 * knows VMS noticed. Here a received byte is echoed in exactly one place:
 * tt_consume(), which only runs on behalf of an outstanding read.
 *
 * LOCKING. tt->lock (exec_lock_t) guards all class-driver state. It is taken
 * from the port's receive path and from process context, never held across a
 * sleep except inside exec_cv_wait*, and never held while calling the port:
 * echo bytes are queued in tt->obuf under the lock and handed to the port
 * after it is dropped (tt_flush). The device row's own lock (dev->lock) is
 * only taken to snapshot its characteristics, and never with tt->lock held.
 *
 * Clean-room (AGENTS.md Rule 8): from the public OpenVMS I/O User's Reference
 * Manual ("Terminal Driver": type-ahead, read function modifiers, terminators,
 * line editing, out-of-band characters) and the observed console bytes of real
 * OpenVMS VAX V7.3 (docs/oracle/keystroke/<CASE>/vax73.ks.txt). No VSI/HPE source
 * or binary.
 */
#include "vms_internal.h"
#include "exec_kbackend.h"
#include "vms_tt.h"
#include "vms_prot.h"

#define VMS_DC_TERM 66   /* DC$_TERM (dcdef.h; vms_devtab.c DC__TERM) */
#define TT_CTRL(c)  ((uint8_t)((c) & 0x1F))
#define CH_DEL      0x7F
#define CH_CR       0x0D
#define CH_LF       0x0A
#define CH_TAB      0x09
#define CH_BS       0x08
#define CH_BEL      0x07
/* a racy peek is enough: a write that misses the flag reaches a port whose
 * own op is a no-op once the line is closed */
#define READ_ONCE_TT(x) (*(volatile __typeof__(x) *)&(x))

struct vms_tt {
	exec_lock_t lock;
	exec_cv_t   cv;            /* read completion / read slot / detach */
	struct vms_device *dev;
	const struct vms_tt_port_ops *ops;
	void *port;
	int refs;                  /* the attach + every caller in vms_tt_read */
	int detached;
	int passall;

	/* the type-ahead buffer: a ring of received, unconsumed, unechoed bytes */
	uint8_t  ta[VMS_TT_TYPAHD_MAX];
	uint32_t ta_head, ta_len;

	/* the read slot: one read at a time, later readers queue on cv */
	int rd_busy;               /* a reader owns the slot */
	int rd_active;             /* it is consuming input */
	int rd_done;
	int rd_suspended;          /* a signal sent its reader back to userspace */
	const void *rd_owner;      /* who may resume it */
	uint64_t rd_susp_ms;
	uint64_t rd_deadline;      /* IO$M_TIMED, absolute */
	uint32_t rd_flags;
	uint32_t rd_cap;           /* bytes this read may assemble */
	uint32_t rd_mask[8];       /* terminator mask in force */
	uint64_t rd_dc;            /* device characteristics at read start */
	uint8_t  prompt[VMS_TT_PROMPT_MAX];
	uint32_t promptsz;
	uint8_t  line[VMS_TT_LINE_MAX];
	uint32_t len;
	int      hc_del;           /* hardcopy: inside a \...\ rubout run */
	int      esc;              /* inside an escape sequence: 1 after ESC, 2 after CSI/SS3 */
	struct vms_tt_read_result res;

	/* echo bytes waiting for the port (handed over outside the lock) */
	uint8_t  obuf[VMS_TT_OBUF];
	uint32_t olen;
};

/* ------------------------------------------------------------------ */

static uint64_t tt_devchar(struct vms_device *dev)
{
	uint64_t dc;

	exec_lock(&dev->lock);
	dc = dev->devchar;
	exec_unlock(&dev->lock);
	return dc;
}

static void tt_out(struct vms_tt *tt, const void *p, uint32_t n)
{
	if (n > VMS_TT_OBUF - tt->olen)
		n = VMS_TT_OBUF - tt->olen;   /* echo overflow: the tail is lost,
					       * as an echo is never worth a stall */
	memcpy(tt->obuf + tt->olen, p, n);
	tt->olen += n;
}

static void tt_out1(struct vms_tt *tt, uint8_t c) { tt_out(tt, &c, 1); }

/* Hand queued echo to the port. Called WITHOUT tt->lock. In small slices so
 * no large buffer sits on the kernel stack. */
static void tt_flush(struct vms_tt *tt)
{
	uint8_t buf[128];
	uint32_t n;

	for (;;) {
		exec_lock(&tt->lock);
		if (tt->detached)
			tt->olen = 0;            /* the port is gone */
		n = tt->olen < sizeof(buf) ? tt->olen : (uint32_t)sizeof(buf);
		memcpy(buf, tt->obuf, n);
		memmove(tt->obuf, tt->obuf + n, tt->olen - n);
		tt->olen -= n;
		exec_unlock(&tt->lock);
		if (!n)
			return;
		if (tt->ops->xmit)
			tt->ops->xmit(tt->port, buf, n);
	}
}

/* The standard terminator set, used when a read names no mask (P4 = 0): every
 * control character except LF, VT, FF, TAB and BS (I/O User's Reference,
 * "Terminators"). With line editing several of those controls are editing keys
 * instead -- tt_consume() acts on an editing key before it asks the mask. */
static int tt_is_term(const struct vms_tt *tt, uint8_t c)
{
	return (tt->rd_mask[c >> 5] >> (c & 31)) & 1;
}

static void tt_std_mask(uint32_t m[8])
{
	memset(m, 0, 8 * sizeof(m[0]));
	m[0] = 0xFFFFFFFFu & ~((1u << CH_LF) | (1u << 0x0B) | (1u << 0x0C) |
			       (1u << CH_TAB) | (1u << CH_BS));
	m[CH_DEL >> 5] &= ~(1u << (CH_DEL & 31));
}

static int tt_echoing(const struct vms_tt *tt)
{
	return (tt->rd_dc & VMS_TTC_ECHO) && !(tt->rd_flags & VMS_TT_RD_NOECHO);
}

static int tt_hardcopy(const struct vms_tt *tt)
{
	return (tt->rd_dc & VMS_TTC_HARDCOPY) != 0;
}

/* Close a hardcopy rubout run: "\deleted\" (the trailing backslash). */
static void tt_hc_close(struct vms_tt *tt)
{
	if (tt->hc_del) {
		if (tt_echoing(tt))
			tt_out1(tt, '\\');
		tt->hc_del = 0;
	}
}

/* Redisplay the prompt and the line so far, on a fresh line (^R; and ^U / ^X
 * on a hardcopy terminal, which cannot erase what it printed). */
static void tt_redisplay(struct vms_tt *tt, int with_line)
{
	tt_out(tt, "\r\n", 2);
	if (tt->promptsz)
		tt_out(tt, tt->prompt, tt->promptsz);
	if (with_line && tt_echoing(tt))
		tt_out(tt, tt->line, tt->len);
}

/* Rub out the whole line (^U; ^X during a read). */
static void tt_kill_line(struct vms_tt *tt)
{
	uint32_t i;

	tt_hc_close(tt);
	if (tt_echoing(tt) && tt->len) {
		if (tt_hardcopy(tt)) {
			tt->len = 0;
			tt_redisplay(tt, 0);
			return;
		}
		for (i = 0; i < tt->len; i++)
			tt_out(tt, "\b \b", 3);
	}
	tt->len = 0;
}

static void tt_complete(struct vms_tt *tt, uint32_t status, uint8_t term, uint32_t termsz)
{
	tt_hc_close(tt);
	tt->res.status = status;
	tt->res.count = tt->len;
	tt->res.term = term;
	tt->res.termsz = termsz;
	tt->rd_active = 0;
	tt->rd_done = 1;
	exec_cv_broadcast(&tt->cv);
}

/*
 * tt_consume - ONE character, consumed by the outstanding read. The only
 * place a received character is ever echoed.
 */
static void tt_consume(struct vms_tt *tt, uint8_t c)
{
	int filter = !(tt->rd_flags & VMS_TT_RD_NOFILTR);

	if (tt->passall) {
		tt->line[tt->len++] = c;
		if (tt->len >= tt->rd_cap)
			tt_complete(tt, SS__NORMAL, 0, 0);
		return;
	}

	/* An escape sequence (a cursor key: ESC [ A, ESC O A, ...) is one key,
	 * not a run of data characters, and it does not end the read unless the
	 * caller's own terminator mask asks for ESC. The cursor keys' editing
	 * meanings -- recall, cursor motion -- belong to the line editor and to
	 * DCL's recall (rd vms-eda8 / vms-eb3d); until then the key is consumed
	 * and has no effect, rather than ending the read (which would run the
	 * half-typed command) or landing in the line as "^[[A". */
	if (tt->esc) {
		if (tt->esc == 1 && (c == '[' || c == 'O')) {
			tt->esc = 2;
			return;
		}
		if (tt->esc == 2 && c >= 0x20 && c < 0x40)
			return;                  /* parameter / intermediate bytes */
		tt->esc = 0;
		if (c >= 0x40 && c < 0x7F)
			return;                  /* the final byte ends the sequence */
		/* not a sequence after all: treat c normally */
	}
	if (filter && c == 0x1B && !(tt->rd_flags & VMS_TT_RD_TERMMASK)) {
		tt->esc = 1;
		return;
	}

	if (filter) {
		switch (c) {
		case CH_DEL:                        /* rub out the last character */
			if (!tt->len)
				return;
			tt->len--;
			if (!tt_echoing(tt))
				return;
			if (tt_hardcopy(tt)) {
				if (!tt->hc_del) {
					tt_out1(tt, '\\');
					tt->hc_del = 1;
				}
				tt_out1(tt, tt->line[tt->len]);
			} else {
				tt_out(tt, "\b \b", 3);
			}
			return;
		case TT_CTRL('U'):                  /* delete to start of line */
			tt_kill_line(tt);
			return;
		case TT_CTRL('R'):                  /* redisplay prompt + line */
			tt_hc_close(tt);
			tt_redisplay(tt, 1);
			return;
		default:
			break;
		}
	}

	if (c == TT_CTRL('Z') && tt_is_term(tt, c)) {
		/* end of file: the driver says so on the terminal */
		if (tt_echoing(tt) || (tt->rd_dc & VMS_TTC_ECHO))
			tt_out(tt, "*EXIT*\r\n", 8);
		tt_complete(tt, SS__NORMAL, c, 1);
		return;
	}

	if (tt_is_term(tt, c)) {
		tt_hc_close(tt);
		if (c == CH_CR && !(tt->rd_flags & VMS_TT_RD_TRMNOECHO) &&
		    (tt->rd_dc & VMS_TTC_ECHO))
			tt_out(tt, "\r\n", 2);
		tt_complete(tt, SS__NORMAL, c, 1);
		return;
	}

	/* a data character */
	if ((tt->rd_flags & VMS_TT_RD_CVTLOW) && c >= 'a' && c <= 'z')
		c = (uint8_t)(c - 'a' + 'A');
	tt_hc_close(tt);
	tt->line[tt->len++] = c;
	if (tt_echoing(tt) && (c >= 0x20 || c == CH_TAB))
		tt_out1(tt, c);
	if (tt->len >= tt->rd_cap)              /* buffer full ends the read */
		tt_complete(tt, SS__NORMAL, 0, 0);
}

static void tt_ta_purge(struct vms_tt *tt)
{
	tt->ta_head = tt->ta_len = 0;
}

static int tt_ta_get(struct vms_tt *tt, uint8_t *c)
{
	if (!tt->ta_len)
		return 0;
	*c = tt->ta[tt->ta_head];
	tt->ta_head = (tt->ta_head + 1) % VMS_TT_TYPAHD_MAX;
	tt->ta_len--;
	return 1;
}

static void tt_ta_put(struct vms_tt *tt, uint8_t c, uint64_t dc)
{
	uint32_t cap = (dc & VMS_TTC_ALTYPEAHD) ? VMS_TT_ALTYPAHD : VMS_TT_TYPAHDSZ;

	if (tt->ta_len >= cap) {
		/* full: the character is lost and the terminal is told so */
		tt_out1(tt, CH_BEL);
		return;
	}
	tt->ta[(tt->ta_head + tt->ta_len) % VMS_TT_TYPAHD_MAX] = c;
	tt->ta_len++;
}

/* Feed the outstanding read from the type-ahead buffer until it completes or
 * the buffer is empty. Caller holds tt->lock. */
static void tt_drain_typeahead(struct vms_tt *tt)
{
	uint8_t c;

	while (tt->rd_active && tt_ta_get(tt, &c))
		tt_consume(tt, c);
}

/* ------------------------------------------------------------------ */

/*
 * LIFETIME. A class-driver instance is created by vms_tt_bind (one reference:
 * the attachment) and referenced by every caller inside vms_tt_read and every
 * vms_tt_of/vms_tt_get holder. vms_tt_detach (the port's close or hangup) ends
 * the attachment: no $QIO can find it through its row any more, outstanding
 * reads end SS$_HANGUP, and no port op is called again. The PORT's own memory
 * and the device row both live until the LAST reference is dropped, so a read
 * that wakes after the line went away touches nothing freed: the port is
 * released through ops->release, the row through vms_devtab_tt_release.
 */
static void tt_put(struct vms_tt *tt)
{
	int last;

	exec_lock(&tt->lock);
	last = (--tt->refs == 0);
	exec_unlock(&tt->lock);
	if (last) {
		if (tt->ops->release)
			tt->ops->release(tt->port);
		vms_devtab_tt_release(tt->dev);
		exec_cv_destroy(&tt->cv);
		exec_lock_destroy(&tt->lock);
		exec_free(tt);
	}
}

uint32_t vms_tt_bind(struct vms_proc *proc, const char *devnam,
                     const struct vms_tt_port_ops *ops, void *port,
                     struct vms_tt **out)
{
	struct vms_device *dev;
	struct vms_tt *tt;
	uint32_t st;

	*out = NULL;
	if (!proc || !ops || !devnam)
		return SS__NOPRIV;          /* no executive identity, no privilege */
	/* Connecting a line to a terminal unit is SYSGEN CONNECT's business on
	 * VMS: CMKRNL. STARTUP (OPA0:) and LOGINOUT (its session terminal) run
	 * as SYSTEM and hold it; a user process does not. */
	st = vms_prot_require_priv(proc->cur_privs, VMS_PRV_M_CMKRNL);
	if (!(st & 1))
		return st;

	tt = exec_zalloc(sizeof(*tt));
	if (!tt)
		return SS__INSFMEM;
	exec_lock_init(&tt->lock);
	exec_cv_init(&tt->cv);
	tt->ops = ops;
	tt->port = port;
	tt->refs = 1;

	st = vms_devtab_tt_attach(devnam, tt, &dev);
	if (!(st & 1)) {
		exec_cv_destroy(&tt->cv);
		exec_lock_destroy(&tt->lock);
		exec_free(tt);
		return st;
	}
	tt->dev = dev;
	*out = tt;
	return SS__NORMAL;
}

void vms_tt_detach(struct vms_tt *tt)
{
	if (!tt)
		return;
	vms_devtab_tt_detached(tt->dev, tt);

	exec_lock(&tt->lock);
	tt->detached = 1;
	tt->olen = 0;                        /* nowhere to send it */
	if (tt->rd_active)
		tt_complete(tt, SS__HANGUP, 0, 0);
	exec_cv_broadcast(&tt->cv);
	exec_unlock(&tt->lock);
	tt_put(tt);
}

struct vms_tt *vms_tt_of(struct vms_device *dev)
{
	struct vms_tt *tt;

	exec_lock(&dev->lock);
	tt = dev->tt;
	if (tt)
		vms_tt_get(tt);
	exec_unlock(&dev->lock);
	return tt;
}

void vms_tt_get(struct vms_tt *tt)
{
	exec_lock(&tt->lock);
	tt->refs++;
	exec_unlock(&tt->lock);
}

void vms_tt_release(struct vms_tt *tt)
{
	if (tt)
		tt_put(tt);
}

void vms_tt_set_passall(struct vms_tt *tt, int on)
{
	exec_lock(&tt->lock);
	tt->passall = !!on;
	exec_unlock(&tt->lock);
}

/*
 * vms_tt_receive - the port hands over received bytes.
 */
void vms_tt_receive(struct vms_tt *tt, const uint8_t *buf, size_t n)
{
	uint64_t dc = tt_devchar(tt->dev);
	uint8_t intr[8], flow[8];
	uint32_t nintr = 0, nflow = 0;
	size_t i;

	exec_lock(&tt->lock);
	for (i = 0; i < n; i++) {
		uint8_t c = buf[i];

		if (!tt->passall) {
			/* OUT-OF-BAND: acted on when TYPED, read or no read. */
			if (c == TT_CTRL('Y') || c == TT_CTRL('C')) {
				/* rd vms-f0fb delivers these as ASTs; until then the
				 * port raises the substrate interrupt it always did.
				 * The type-ahead is discarded, and a read in progress
				 * ends SS$_ABORT here, deterministically, rather than
				 * by the signal that follows. */
				tt_ta_purge(tt);
				if (tt->rd_active)
					tt_complete(tt, SS__ABORT, 0, 0);
				if (nintr < sizeof(intr))
					intr[nintr++] = c;
				continue;
			}
			if ((dc & VMS_TTC_TTSYNC) &&
			    (c == TT_CTRL('S') || c == TT_CTRL('Q'))) {
				/* TTSYNC: ^S holds the terminal's output, ^Q
				 * releases it; neither is ever data (I/O User's
				 * Reference, "Terminal/host synchronization"). */
				if (nflow < sizeof(flow))
					flow[nflow++] = c;
				continue;
			}
			if (c == TT_CTRL('X')) {
				/* ^X: purge the type-ahead buffer, and the line of
				 * a read in progress */
				tt_ta_purge(tt);
				if (tt->rd_active) {
					tt->rd_dc = dc;
					tt_kill_line(tt);
				}
				continue;
			}
		}

		if (tt->rd_active && tt->ta_len == 0) {
			tt_consume(tt, c);
			continue;
		}
		if (!(dc & VMS_TTC_TYPEAHEAD) && !tt->rd_active)
			continue;        /* NOTYPEAHEAD: unsolicited input is lost */
		tt_ta_put(tt, c, dc);
		tt_drain_typeahead(tt);
	}
	if (tt->passall && tt->rd_active && tt->len)
		tt_complete(tt, SS__NORMAL, 0, 0);  /* PASSALL: what has arrived */
	exec_unlock(&tt->lock);

	tt_flush(tt);
	for (i = 0; i < nflow; i++)
		if (tt->ops->flow)
			tt->ops->flow(tt->port, flow[i] == TT_CTRL('S'));
	for (i = 0; i < nintr; i++)
		if (tt->ops->interrupt)
			tt->ops->interrupt(tt->port, intr[i]);
}

int vms_tt_readable(struct vms_tt *tt)
{
	int r;

	exec_lock(&tt->lock);
	r = tt->ta_len > 0 || tt->detached;
	exec_unlock(&tt->lock);
	return r;
}

/*
 * vms_tt_read - one read (IO$_READVBLK / READLBLK / READPROMPT).
 *
 * A SIGNAL DOES NOT END A READ. On VMS what interrupts a waiting process is an
 * AST, which runs while the read stays outstanding: the user keeps typing into
 * the same line and nothing is lost. On the substrate the waiting thread must
 * return to deliver a signal, so the read is SUSPENDED instead of ended: the
 * slot, the line typed so far, the prompt and the deadline all stay in the
 * class driver (input that arrives meanwhile is still consumed and echoed by
 * the receive path), and -ERESTARTSYS goes back to the caller. When the same
 * owner issues its read again (an SA_RESTART restart of the ioctl, or the
 * kif's re-entry -- kif_wait_call), the read RESUMES where it was: no second
 * prompt, no lost characters. A suspended read nobody resumes (its owner died)
 * is abandoned after VMS_TT_SUSPEND_MS by the next reader that needs the slot.
 *
 * What DOES end a read early is the driver's own out-of-band handling: ^Y/^C
 * complete it SS$_ABORT in vms_tt_receive() before the interrupt is raised.
 */
#define VMS_TT_SUSPEND_MS 1000

static void tt_slot_release(struct vms_tt *tt)
{
	tt->rd_busy = 0;
	tt->rd_done = 0;
	tt->rd_suspended = 0;
	tt->rd_owner = NULL;
	exec_cv_broadcast(&tt->cv);               /* the next queued reader */
}

int vms_tt_read(struct vms_tt *tt, const struct vms_tt_read_req *req,
                uint8_t *out, struct vms_tt_read_result *res)
{
	uint64_t dc = tt_devchar(tt->dev);
	int intr = 0, timed_out = 0, resumed = 0;

	memset(res, 0, sizeof(*res));
	if (req->bufsz == 0) {
		res->status = SS__NORMAL;      /* a zero-length read completes */
		return 0;
	}

	exec_lock(&tt->lock);
	tt->refs++;

	if (tt->rd_busy && tt->rd_suspended && tt->rd_owner == req->owner &&
	    req->owner != NULL) {
		tt->rd_suspended = 0;            /* the same read, resumed */
		resumed = 1;
	}

	/* one read at a time: later readers queue */
	while (!resumed && tt->rd_busy && !tt->detached) {
		if (tt->rd_suspended &&
		    exec_ticks_ms() - tt->rd_susp_ms >= VMS_TT_SUSPEND_MS) {
			/* its owner never came back for it */
			tt->rd_active = 0;
			tt_slot_release(tt);
			break;
		}
		if (tt->rd_suspended) {
			int to = 0;
			intr = exec_cv_wait_timeout(&tt->cv, &tt->lock, 100, &to);
		} else {
			intr = exec_cv_wait(&tt->cv, &tt->lock);
		}
		if (intr)
			break;
	}
	if (intr || tt->detached) {
		res->status = tt->detached ? SS__HANGUP : SS__ABORT;
		exec_unlock(&tt->lock);
		tt_put(tt);
		return intr ? -ERESTARTSYS : 0;
	}

	if (!resumed) {
		tt->rd_busy = 1;
		tt->rd_done = 0;
		tt->rd_owner = req->owner;
		tt->rd_flags = req->flags;
		tt->rd_cap = req->bufsz < VMS_TT_LINE_MAX ? req->bufsz : VMS_TT_LINE_MAX;
		tt->rd_dc = dc;
		tt->len = 0;
		tt->hc_del = 0;
		tt->esc = 0;
		memset(&tt->res, 0, sizeof(tt->res));
		if (req->flags & VMS_TT_RD_TERMMASK)
			memcpy(tt->rd_mask, req->termmask, sizeof(tt->rd_mask));
		else
			tt_std_mask(tt->rd_mask);
		tt->promptsz = 0;
		if (req->prompt && req->promptsz) {
			tt->promptsz = req->promptsz < VMS_TT_PROMPT_MAX ? req->promptsz
									 : VMS_TT_PROMPT_MAX;
			memcpy(tt->prompt, req->prompt, tt->promptsz);
		}
		tt->rd_deadline = 0;
		if (req->flags & VMS_TT_RD_TIMED)
			tt->rd_deadline = exec_ticks_ms() + (uint64_t)req->timeout_s * 1000u;

		if (req->flags & VMS_TT_RD_PURGE)
			tt_ta_purge(tt);
		/* the prompt is written BEFORE any type-ahead is consumed and echoed */
		if (tt->promptsz)
			tt_out(tt, tt->prompt, tt->promptsz);

		tt->rd_active = 1;
		tt_drain_typeahead(tt);
		if (tt->passall && tt->rd_active && tt->len)
			tt_complete(tt, SS__NORMAL, 0, 0);
	}

	while (!tt->rd_done && !tt->detached) {
		uint32_t olen = tt->olen;

		if (olen) {                       /* show the prompt/echo first */
			exec_unlock(&tt->lock);
			tt_flush(tt);
			exec_lock(&tt->lock);
			continue;
		}
		if (tt->rd_flags & VMS_TT_RD_TIMED) {
			uint64_t now = exec_ticks_ms();

			if (now >= tt->rd_deadline) {
				timed_out = 1;
				break;
			}
			intr = exec_cv_wait_timeout(&tt->cv, &tt->lock,
						    (unsigned int)(tt->rd_deadline - now),
						    &timed_out);
			timed_out = 0;            /* re-tested at the loop top */
		} else {
			intr = exec_cv_wait(&tt->cv, &tt->lock);
		}
		if (intr && !tt->rd_done) {
			/* a signal: suspend, do not end (see above) */
			tt->rd_suspended = 1;
			tt->rd_susp_ms = exec_ticks_ms();
			exec_unlock(&tt->lock);
			tt_flush(tt);
			tt_put(tt);
			res->status = SS__ABORT;
			return -ERESTARTSYS;
		}
	}

	if (!tt->rd_done) {
		if (tt->detached)
			tt_complete(tt, SS__HANGUP, 0, 0);
		else if (timed_out)
			tt_complete(tt, SS__TIMEOUT, 0, 0);
		else
			tt_complete(tt, SS__ABORT, 0, 0);
	}

	*res = tt->res;
	memcpy(out, tt->line, res->count);
	tt_slot_release(tt);
	exec_unlock(&tt->lock);
	tt_flush(tt);
	tt_put(tt);
	return 0;
}

/*
 * vms_tt_write - output through the class driver. `cooked` converts LF to
 * CR LF for a Unix-style writer arriving on the port's own write path.
 */
int vms_tt_write(struct vms_tt *tt, const uint8_t *buf, size_t n, int cooked)
{
	uint8_t chunk[128];
	size_t i = 0;

	while (i < n) {
		size_t k = 0;
		int rc;

		if (READ_ONCE_TT(tt->detached))
			return -EIO;             /* the line went away */

		while (i < n && k < sizeof(chunk) - 1) {
			if (cooked && buf[i] == CH_LF)
				chunk[k++] = CH_CR;
			chunk[k++] = buf[i++];
		}
		if (tt->ops->write)
			rc = tt->ops->write(tt->port, chunk, k);
		else {
			tt->ops->xmit(tt->port, chunk, k);
			rc = 0;
		}
		if (rc)
			return rc;
	}
	return 0;
}

/* ================================================================
 * The /dev/vms surface: $QIO on a channel to a terminal.
 * ================================================================ */

/* The class driver behind `chan`, referenced; or a status saying why not. */
static struct vms_tt *tt_from_chan(struct vms_proc *proc, uint32_t chan,
                                   uint32_t *status)
{
	struct vms_device *dev = vms_devtab_chan_device(proc, chan);
	struct vms_tt *tt;

	if (!dev) {
		*status = SS__IVCHAN;
		return NULL;
	}
	if (dev->devclass != VMS_DC_TERM) {
		*status = SS__IVDEVNAM;
		return NULL;
	}
	tt = vms_tt_of(dev);
	if (!tt) {
		/* a terminal row no port is attached to: nothing can carry
		 * its bytes, and the executive says so (Rule 9) */
		*status = SS__DEVOFFLINE;
		return NULL;
	}
	return tt;
}

long vms_ioctl_tt_read(struct vms_proc *proc, unsigned long arg)
{
	struct vms_tt_read_args a;
	struct vms_tt_read_req rq;
	struct vms_tt_read_result r;
	struct vms_tt *tt;
	uint8_t *line = NULL, *prompt = NULL;
	uint32_t st = SS__NORMAL;

	memset(&a, 0, sizeof(a));
	if (exec_copyin(&a, (const void *)arg, sizeof(a)))
		return -EFAULT;
	tt = tt_from_chan(proc, a.chan, &st);
	if (!tt) {
		a.status = st;
		goto out;
	}

	memset(&rq, 0, sizeof(rq));
	rq.flags = a.flags;
	rq.bufsz = a.bufsz < VMS_TT_LINE_MAX ? a.bufsz : VMS_TT_LINE_MAX;
	rq.timeout_s = a.timeout;
	memcpy(rq.termmask, a.termmask, sizeof(rq.termmask));
	if (a.prompt && a.promptsz) {
		rq.promptsz = a.promptsz < VMS_TT_PROMPT_MAX ? a.promptsz : VMS_TT_PROMPT_MAX;
		prompt = exec_alloc(rq.promptsz);
		if (!prompt) {
			a.status = SS__INSFMEM;
			goto out_rel;
		}
		if (exec_copyin(prompt, (const void *)(uintptr_t)a.prompt, rq.promptsz)) {
			a.status = SS__ACCVIO;
			goto out_rel;
		}
		rq.prompt = prompt;
	}
	line = exec_alloc(VMS_TT_LINE_MAX);
	if (!line) {
		a.status = SS__INSFMEM;
		goto out_rel;
	}

	/* a signal suspends the read; this process's re-entry resumes it */
	rq.owner = proc;
	if (vms_tt_read(tt, &rq, line, &r) == -ERESTARTSYS) {
		vms_tt_release(tt);
		exec_free(line);
		if (prompt)
			exec_free(prompt);
		return -ERESTARTSYS;            /* no status written: re-enter */
	}
	a.status = r.status;
	a.count = r.count;
	a.term = r.term;
	a.termsz = r.termsz;
	if (r.count && exec_copyout((void *)(uintptr_t)a.buf, line, r.count))
		a.status = SS__ACCVIO;

out_rel:
	vms_tt_release(tt);
out:
	if (line)
		exec_free(line);
	if (prompt)
		exec_free(prompt);
	if (exec_copyout((void *)arg, &a, sizeof(a)))
		return -EFAULT;
	return 0;
}

long vms_ioctl_tt_write(struct vms_proc *proc, unsigned long arg)
{
	struct vms_tt_write_args a;
	struct vms_tt *tt;
	uint8_t chunk[128];
	uint32_t st = SS__NORMAL, done = 0;

	memset(&a, 0, sizeof(a));
	if (exec_copyin(&a, (const void *)arg, sizeof(a)))
		return -EFAULT;
	tt = tt_from_chan(proc, a.chan, &st);
	if (!tt) {
		a.status = st;
		goto out;
	}
	a.status = SS__NORMAL;
	while (done < a.len) {
		uint32_t k = a.len - done < sizeof(chunk) ? a.len - done : (uint32_t)sizeof(chunk);

		if (exec_copyin(chunk, (const void *)(uintptr_t)(a.buf + done), k)) {
			a.status = SS__ACCVIO;
			break;
		}
		if (vms_tt_write(tt, chunk, k, 0)) {
			a.status = SS__ABORT;
			break;
		}
		done += k;
	}
	vms_tt_release(tt);
out:
	if (exec_copyout((void *)arg, &a, sizeof(a)))
		return -EFAULT;
	return 0;
}

long vms_ioctl_tt_setmode(struct vms_proc *proc, unsigned long arg)
{
	struct vms_tt_mode_args a;
	struct vms_tt *tt;
	uint32_t st = SS__NORMAL;

	memset(&a, 0, sizeof(a));
	if (exec_copyin(&a, (const void *)arg, sizeof(a)))
		return -EFAULT;
	tt = tt_from_chan(proc, a.chan, &st);
	if (!tt) {
		a.status = st;
		goto out;
	}
	vms_tt_set_passall(tt, (a.mode & VMS_TT_MODE_PASSALL) != 0);
	vms_tt_release(tt);
	a.status = SS__NORMAL;
out:
	if (exec_copyout((void *)arg, &a, sizeof(a)))
		return -EFAULT;
	return 0;
}

/*
 * VMS_IOCTL_TT_SENSE (rd vms-f8c): what terminal `devnam`'s class driver is
 * doing, for a network port relaying its reads (the DECnet CTERM host). The
 * echo answer is the driver's own decision -- the read's IO$M_NOECHO, the
 * terminal's ECHO characteristic -- so a remote asked to echo echoes exactly
 * what the driver would, and a Password: read is never echoed remotely. CMKRNL,
 * like the bind: this is the port side of the class/port interface.
 */
long vms_ioctl_tt_sense(struct vms_proc *proc, unsigned long arg)
{
	struct vms_tt_sense_args a;
	struct vms_tt *tt;
	uint64_t dc;

	memset(&a, 0, sizeof(a));
	if (exec_copyin(&a, (const void *)arg, sizeof(a)))
		return -EFAULT;
	a.devnam[sizeof(a.devnam) - 1] = '\0';
	a.state = 0;
	a.status = vms_prot_require_priv(proc->cur_privs, VMS_PRV_M_CMKRNL);
	if (!(a.status & 1))
		goto out;
	tt = vms_devtab_tt_by_name(a.devnam);
	if (!tt) {
		a.status = SS__NORMAL;          /* no port: state 0 */
		goto out;
	}
	dc = tt_devchar(tt->dev);
	exec_lock(&tt->lock);
	if (!tt->detached) {
		a.state |= VMS_TT_SENSE_BOUND;
		if (tt->passall)
			a.state |= VMS_TT_SENSE_PASSALL;
		if (tt->rd_active) {
			a.state |= VMS_TT_SENSE_READING;
			if (!tt->passall && tt_echoing(tt))
				a.state |= VMS_TT_SENSE_ECHOING;
		} else if (!tt->passall && (dc & VMS_TTC_ECHO)) {
			a.state |= VMS_TT_SENSE_ECHOING;
		}
	}
	exec_unlock(&tt->lock);
	vms_tt_release(tt);
	a.status = SS__NORMAL;
out:
	if (exec_copyout((void *)arg, &a, sizeof(a)))
		return -EFAULT;
	return 0;
}
