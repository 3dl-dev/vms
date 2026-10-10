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
#include "exec_list.h"
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
	uint32_t port_busy;        /* port ops in flight (vms_tt_set_port waits) */
	uint32_t port_gen;         /* bumped by every vms_tt_set_port */
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
	int rd_ldisc;              /* the read is the line's own read(2) */
	uint32_t ldisc_kick;       /* vms_tt_kick_ldisc generation */
	uint32_t rd_flags;
	uint32_t rd_cap;           /* bytes this read may assemble */
	uint32_t rd_mask[8];       /* terminator mask in force */
	uint64_t rd_dc;            /* device characteristics at read start */
	uint8_t  prompt[VMS_TT_PROMPT_MAX];
	uint32_t promptsz;
	uint8_t  line[VMS_TT_LINE_MAX];
	uint32_t len;
	uint32_t cur;              /* line editing: the cursor, 0..len (rd vms-eda8) */
	uint32_t rd_pcol;          /* the column the line starts at (after the prompt) */
	uint32_t rd_width;         /* the terminal's width at read start */
	int      ovs;              /* overstrike (^A toggles; INSERT_EDITING default) */
	int      hc_del;           /* hardcopy: inside a \...\ rubout run */
	int      esc;              /* inside an escape sequence: 1 after ESC, 2 after CSI/SS3 */
	struct vms_tt_read_result res;

	/* echo bytes waiting for the port (handed over outside the lock) */
	uint8_t  obuf[VMS_TT_OBUF];
	uint32_t olen;

	/* WHERE THE CURSOR IS, as the carriage-control rules need it (rd vms-fc4;
	 * see tt_nl()). Updated for every byte the driver emits -- echo, prompts
	 * and writes alike -- under tt->lock. */
	int      pos;              /* TT_POS_FRESH / TT_POS_CR / TT_POS_MID */
	uint8_t  last;             /* the last byte emitted */
	int      rec_open;         /* a cooked write's record awaits its '\n' */

	/* OUT-OF-BAND ASTs (rd vms-f0fb), armed through a channel by $QIO
	 * IO$_SETMODE!IO$M_CTRLYAST / CTRLCAST / OUTBAND. `proc` is valid while
	 * the arming channel exists: deassigning it (vms_tt_chan_gone, under
	 * tt->lock) clears the entry before the process can be freed. */
	struct tt_oob {
		struct vms_proc *proc;     /* NULL: not armed */
		pid_t    owner;            /* the arming channel: its process ... */
		uint32_t chan;             /* ... and number */
		uint64_t astadr, astprm;
		uint8_t  acmode;
		uint32_t mask;             /* OUTBAND: control characters */
	} oob[3];                      /* VMS_TT_OOB_CTRLY / _CTRLC / _OUTBAND, minus 1 */
	int      ctrlo;            /* CTRL/O: output is being discarded */
	int      rd_astpend;       /* an AST for the reader's process is queued */
	int      rd_disturbed;     /* output broke through the read: redisplay */
};

/* ------------------------------------------------------------------ */

/*
 * PORT OPS ARE BRACKETED. The port behind an instance can be REPLACED while it
 * lives (vms_tt_set_port: the Linux console's line discipline is re-opened when
 * a session leader exits). Every port op is called between tt_port_enter() and
 * tt_port_exit(), which pin the (ops, port) pair for the call; the swap waits
 * until no call is in flight. Never called with tt->lock held.
 */
struct tt_portref {
	const struct vms_tt_port_ops *ops;
	void *port;
	uint32_t gen;
};

static int tt_port_enter(struct vms_tt *tt, struct tt_portref *pr)
{
	exec_lock(&tt->lock);
	if (tt->detached) {
		exec_unlock(&tt->lock);
		return 0;
	}
	tt->port_busy++;
	pr->ops = tt->ops;
	pr->port = tt->port;
	pr->gen = tt->port_gen;
	exec_unlock(&tt->lock);
	return 1;
}

static void tt_port_exit(struct vms_tt *tt)
{
	exec_lock(&tt->lock);
	if (--tt->port_busy == 0)
		exec_cv_broadcast(&tt->cv);
	exec_unlock(&tt->lock);
}

static uint32_t tt_devwidth(struct vms_device *dev)
{
	uint32_t w;

	exec_lock(&dev->lock);
	w = dev->width;
	exec_unlock(&dev->lock);
	return w ? w : 80;
}

static uint64_t tt_devchar(struct vms_device *dev)
{
	uint64_t dc;

	exec_lock(&dev->lock);
	dc = dev->devchar;
	exec_unlock(&dev->lock);
	return dc;
}

/*
 * CARRIAGE CONTROL (rd vms-fc4). What a VMS terminal shows around records and
 * prompts is not the bytes a program wrote but the terminal driver's rendering
 * of a NEW LINE, which depends on where the cursor is. Measured on the real
 * VAX V7.3 console with probe cases (docs/oracle/keystroke-probes/), three
 * positions matter:
 *
 *   TT_POS_FRESH  column 0 of a line nothing has been written on -- just after
 *                 a read's echoed CR terminator ("CR LF"), *EXIT*, ^U.
 *   TT_POS_CR     column 0 of a line that HAS text: a record was written and
 *                 ended with its carriage return; the line feed is OWED.
 *   TT_POS_MID    anywhere else (after a prompt, mid-line).
 *
 * and a new line is rendered:  FRESH -> CR,  CR -> LF,  MID -> CR LF  (tt_nl).
 * A record (one line of a program's output: implied carriage control) is a
 * new line, the text, and a carriage return that leaves the line feed owed.
 * A read that ECHOES first pays an owed line feed; a NOECHO read does not --
 * so on the real console a prompt after a record overprints the record's line
 * when the terminal is SET TERMINAL/NOECHO (probe CC.MIX N3). A prompt that
 * begins with CR LF (DCL's "$ ", INQUIRE's) starts with a new line.
 * Clean-room: observed console bytes only (Rule 8).
 */
#define TT_POS_FRESH 0
#define TT_POS_CR    1
#define TT_POS_MID   2
/* column 0 of an empty line the driver itself opened with an out-of-band
 * notice (*OUTPUT ON*): the next new line costs nothing (OOB.CTRLO OW2:
 * "*OUTPUT ON*<CR><LF>LINE 17") */
#define TT_POS_CLEAN 3

static void tt_track(struct vms_tt *tt, uint8_t c)
{
	switch (c) {
	case CH_CR:
		if (tt->pos != TT_POS_FRESH && tt->pos != TT_POS_CLEAN)
			tt->pos = TT_POS_CR;
		break;
	case CH_LF:
		if (tt->pos == TT_POS_CR)
			tt->pos = TT_POS_FRESH;
		break;
	case 0:
		break;                       /* a fill character moves nothing */
	default:
		tt->pos = TT_POS_MID;
		break;
	}
	tt->last = c;
}

static void tt_out(struct vms_tt *tt, const void *p, uint32_t n)
{
	const uint8_t *b = p;
	uint32_t i;

	if (n > VMS_TT_OBUF - tt->olen)
		n = VMS_TT_OBUF - tt->olen;   /* echo overflow: the tail is lost,
					       * as an echo is never worth a stall */
	memcpy(tt->obuf + tt->olen, p, n);
	tt->olen += n;
	for (i = 0; i < n; i++)
		tt_track(tt, b[i]);
}

static void tt_out1(struct vms_tt *tt, uint8_t c) { tt_out(tt, &c, 1); }

/* A new line, rendered for where the cursor is (see CARRIAGE CONTROL). */
static void tt_nl(struct vms_tt *tt)
{
	switch (tt->pos) {
	case TT_POS_FRESH: tt_out1(tt, CH_CR);       break;
	case TT_POS_CR:    tt_out1(tt, CH_LF);       break;
	case TT_POS_CLEAN:                           break;
	default:           tt_out(tt, "\r\n", 2);    break;
	}
}

/* A prompt (or its redisplay): a leading CR LF is a new line. */
static void tt_prompt_out(struct vms_tt *tt, const uint8_t *p, uint32_t n)
{
	if (n >= 2 && p[0] == CH_CR && p[1] == CH_LF) {
		tt_nl(tt);
		p += 2;
		n -= 2;
	}
	tt_out(tt, p, n);
}

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
		{
			struct tt_portref pr;

			if (tt_port_enter(tt, &pr)) {
				if (pr.ops->xmit)
					pr.ops->xmit(pr.port, buf, n);
				tt_port_exit(tt);
			}
		}
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
	tt_nl(tt);
	if (tt->promptsz)
		tt_prompt_out(tt, tt->prompt, tt->promptsz);
	if (with_line && tt_echoing(tt))
		tt_out(tt, tt->line, tt->len);
}

static void tt_bs(struct vms_tt *tt, uint32_t n);
static int tt_line_wrapped(const struct vms_tt *tt);
static void tt_echo_tail(struct vms_tt *tt, uint32_t from, uint32_t pad);
static void tt_redraw_in_place(struct vms_tt *tt);
static void tt_prompt_cr(struct vms_tt *tt);

/* Rub out the line (^X during a read: all of it; ^U: from the cursor back to
 * the start). */
static void tt_kill_line(struct vms_tt *tt)
{
	uint32_t i, keep = tt->len - tt->cur;

	tt_hc_close(tt);
	if (tt_echoing(tt) && tt->cur) {
		if (tt_hardcopy(tt)) {
			memmove(tt->line, tt->line + tt->cur, keep);
			tt->len = keep;
			tt->cur = 0;
			tt_redisplay(tt, keep != 0);
			tt_bs(tt, keep);
			return;
		}
		/* a screen: the prompt again in place, what is left after it,
		 * the rest of the line erased (LE.CTRLU / LE.CTRLX). A line that
		 * has wrapped cannot be reached above its last row: that row is
		 * cleared and the cursor left at its start (WRAP.LONG U). */
		(void)i;
		if (keep == 0 && tt_line_wrapped(tt)) {
			uint32_t w = tt->rd_width - 1;

			tt_out1(tt, CH_CR);
			for (i = 0; i < w; i++)
				tt_out1(tt, ' ');
			tt_bs(tt, w);
			tt->len = 0;
			tt->cur = 0;
			return;
		}
		memmove(tt->line, tt->line + tt->cur, keep);
		tt->len = keep;
		tt->cur = 0;
		tt_redraw_in_place(tt);
		return;
	}
	memmove(tt->line, tt->line + tt->cur, keep);
	tt->len = keep;
	tt->cur = 0;
}

/*
 * LINE EDITING (rd vms-eda8). The read keeps a cursor inside its line; the
 * editing keys move it and the echo keeps the terminal in step using only
 * what every terminal does -- print, backspace, carriage return -- as the VAX
 * V7.3 console shows (keystroke cases LE.CURSOR, LE.OVERSTRIKE, LE.CTRLJ):
 *   ^H     to the start: CR and the prompt again ("<CR><NUL>$ ")
 *   ^E     to the end: the rest of the line printed
 *   ^D, <- one left: BS          ^F, -> one right: the character printed
 *   ^A     insert/overstrike for the rest of this read
 *   ^J     delete the word left of the cursor
 * Inserting mid-line prints the character, the rest of the line, and
 * backspaces to just after it ("23<BS>").
 */
static void tt_bs(struct vms_tt *tt, uint32_t n)
{
	while (n--)
		tt_out1(tt, CH_BS);
}

/*
 * ERASE TO THE END OF THE LINE, the way the VAX V7.3 driver does on a video
 * terminal it knows no escape sequences for (OPA0:, "Unknown" type; keystroke
 * LE.* captured with SET TERMINAL/NOHARDCOPY): blanks out to the last column
 * but one, then back. `col` is where the cursor stands; returns the blanks
 * written.
 */
/*
 * WRAP (rd vms-cef). With the terminal's WRAP characteristic a line longer
 * than the width continues on the next row: the driver writes CR LF before the
 * character that would fall past the last column (keystroke WRAP.LONG, VAX
 * V7.3 at 80 columns: "$ WRITE ...<60 digits><CR><LF><the rest>").
 */
static int tt_wraps(const struct vms_tt *tt)
{
	return (tt->rd_dc & VMS_TTC_WRAP) && tt->rd_width;
}

/* Echo line[i] at its place in the line, wrapping first if it starts a row. */
static void tt_out_linech(struct vms_tt *tt, uint32_t i)
{
	uint32_t at = tt->rd_pcol + i;

	if (tt_wraps(tt) && at && at % tt->rd_width == 0)
		tt_out(tt, "\r\n", 2);
	tt_out1(tt, tt->line[i]);
}

static void tt_out_line(struct vms_tt *tt, uint32_t from, uint32_t to)
{
	uint32_t i;

	for (i = from; i < to; i++)
		tt_out_linech(tt, i);
}

/* The column the line's position `i` is shown at (on its row). */
static uint32_t tt_col(const struct vms_tt *tt, uint32_t i)
{
	uint32_t at = tt->rd_pcol + i;

	if (!tt_wraps(tt))
		return at;
	return at && at % tt->rd_width == 0 ? tt->rd_width : at % tt->rd_width;
}

/* Does the line occupy more than its first row? */
static int tt_line_wrapped(const struct vms_tt *tt)
{
	return tt_wraps(tt) && tt->rd_pcol + tt->len > tt->rd_width;
}

static uint32_t tt_erase_eol(struct vms_tt *tt, uint32_t col)
{
	uint32_t last = tt->rd_width ? tt->rd_width - 1 : 79;
	uint32_t n = col < last ? last - col : 0, i;

	for (i = 0; i < n; i++)
		tt_out1(tt, ' ');
	return n;
}

/* Show line[from..len) then come back to the cursor; on paper, `pad` blanks
 * cover characters removed from the end, on a screen the rest of the line is
 * erased. */
static void tt_echo_tail(struct vms_tt *tt, uint32_t from, uint32_t pad)
{
	uint32_t i;

	if (!tt_echoing(tt))
		return;
	tt_out_line(tt, from, tt->len);
	if (!tt_hardcopy(tt)) {
		uint32_t e = tt_erase_eol(tt, tt_col(tt, tt->len));
		tt_bs(tt, e + tt->len - tt->cur);
		return;
	}
	for (i = 0; i < pad; i++)
		tt_out1(tt, ' ');
	tt_bs(tt, tt->len - tt->cur + pad);
}

/* The prompt again from the start of its line, without its new line (^H, and
 * a screen's ^U / ^X / ^R). */
static void tt_prompt_cr(struct vms_tt *tt)
{
	const uint8_t *pp = tt->prompt;
	uint32_t pn = tt->promptsz;

	tt_out1(tt, CH_CR);
	if (pn >= 2 && pp[0] == CH_CR && pp[1] == CH_LF) {
		pp += 2;
		pn -= 2;
	}
	tt_out(tt, pp, pn);
}

/* A screen's redisplay: the line drawn again in place, the rest erased, the
 * cursor put back (^R; ^U and ^X with what is left of the line). */
static void tt_redraw_in_place(struct vms_tt *tt)
{
	uint32_t e;

	tt_prompt_cr(tt);
	tt_out_line(tt, 0, tt->len);
	e = tt_erase_eol(tt, tt_col(tt, tt->len));
	tt_bs(tt, e + tt->len - tt->cur);
}

/* Remove line[at..at+n) with the cursor at `at` (already moved there on the
 * terminal), and show the result. */
static void tt_delete_at(struct vms_tt *tt, uint32_t at, uint32_t n)
{
	memmove(tt->line + at, tt->line + at + n, tt->len - at - n);
	tt->len -= n;
	tt->cur = at;
	if (tt_hardcopy(tt) && at == tt->len)
		return;                     /* paper: nothing to erase */
	tt_echo_tail(tt, at, n);
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
		if (c >= 0x40 && c < 0x7F) {
			/* the final byte ends the sequence: <- and -> move the
			 * cursor (^D / ^F); the others belong to recall */
			if (c == 'D' && tt->cur > 0) {
				tt_hc_close(tt);
				tt->cur--;
				if (tt_echoing(tt))
					tt_out1(tt, CH_BS);
			} else if (c == 'C' && tt->cur < tt->len) {
				tt_hc_close(tt);
				if (tt_echoing(tt))
					tt_out1(tt, tt->line[tt->cur]);
				tt->cur++;
			} else if (c == 'A' && tt_is_term(tt, TT_CTRL('B'))) {
				/* up arrow: the recall key, as CTRL/B (RC.RECALL U1
				 * shows the same <02> echo for both) */
				tt_hc_close(tt);
				if (tt_echoing(tt))
					tt_out1(tt, TT_CTRL('B'));
				tt_complete(tt, SS__NORMAL, TT_CTRL('B'), 1);
			} else if (c == 'B' && tt_is_term(tt, 0x1B)) {
				/* down arrow: ends the read, the sequence reported
				 * as the terminator (the caller walks its recall
				 * list forward; RC.RECALL DN echoes nothing) */
				tt_hc_close(tt);
				tt_complete(tt, SS__NORMAL, 0x1B, 3);
				tt->res.term = 0x1B | ((uint32_t)'B' << 8);
			}
			return;
		}
		/* not a sequence after all: treat c normally */
	}
	if (filter && c == 0x1B && !(tt->rd_flags & VMS_TT_RD_TERMMASK)) {
		tt->esc = 1;
		return;
	}

	if (filter) {
		switch (c) {
		case CH_DEL:                        /* rub out the character left of the cursor */
			if (!tt->cur)
				return;
			if (tt->cur < tt->len) {
				tt_hc_close(tt);
				if (tt_echoing(tt))
					tt_out1(tt, CH_BS);
				tt_delete_at(tt, tt->cur - 1, 1);
				return;
			}
			tt->len--;
			tt->cur--;
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
			if (tt_hardcopy(tt)) {
				tt_redisplay(tt, 1);
				if (tt_echoing(tt))
					tt_bs(tt, tt->len - tt->cur);
			} else if (tt_echoing(tt)) {
				tt_redraw_in_place(tt);
			}
			return;
		case CH_BS:                         /* ^H: to the start of the line */
			tt_hc_close(tt);
			if (tt->cur && tt_echoing(tt))
				tt_prompt_cr(tt);
			tt->cur = 0;
			return;
		case TT_CTRL('E'):                  /* to the end of the line */
			tt_hc_close(tt);
			if (tt_echoing(tt))
				tt_out(tt, tt->line + tt->cur, tt->len - tt->cur);
			tt->cur = tt->len;
			return;
		case TT_CTRL('D'):                  /* one character left */
			tt_hc_close(tt);
			if (tt->cur) {
				tt->cur--;
				if (tt_echoing(tt))
					tt_out1(tt, CH_BS);
			}
			return;
		case TT_CTRL('F'):                  /* one character right */
			tt_hc_close(tt);
			if (tt->cur < tt->len) {
				if (tt_echoing(tt))
					tt_out1(tt, tt->line[tt->cur]);
				tt->cur++;
			}
			return;
		case TT_CTRL('A'):                  /* insert <-> overstrike */
			tt->ovs = !tt->ovs;
			return;
		case CH_LF: {                       /* ^J: delete the word to the left */
			uint32_t at = tt->cur;

			tt_hc_close(tt);
			while (at && tt->line[at - 1] == ' ')
				at--;
			while (at && tt->line[at - 1] != ' ')
				at--;
			if (at == tt->cur)
				return;
			if (tt_echoing(tt))
				tt_bs(tt, tt->cur - at);
			{
				uint32_t n = tt->cur - at;

				tt->cur = at + n;   /* tt_delete_at removes [at, at+n) */
				tt_delete_at(tt, at, n);
			}
			return;
		}
		case TT_CTRL('W'):                  /* refresh: nothing to redraw on this terminal */
			return;
		default:
			break;
		}
	}

	if (c == TT_CTRL('Z') && tt_is_term(tt, c)) {
		/* end of file: the driver says so on the terminal */
		if (tt_echoing(tt) || (tt->rd_dc & VMS_TTC_ECHO))
			{
				tt_out(tt, "*EXIT*", 6);
				tt_nl(tt);
			}
		tt_complete(tt, SS__NORMAL, c, 1);
		return;
	}

	if (tt_is_term(tt, c)) {
		tt_hc_close(tt);
		if (c == CH_CR && !(tt->rd_flags & VMS_TT_RD_TRMNOECHO) &&
		    (tt->rd_dc & VMS_TTC_ECHO))
			tt_nl(tt);             /* the RETURN, echoed */
		else if (c == TT_CTRL('B') && tt_echoing(tt))
			tt_out1(tt, c);        /* CTRL/B is echoed as itself (RC.RECALL B) */
		tt_complete(tt, SS__NORMAL, c, 1);
		return;
	}

	/* a data character, at the cursor */
	if ((tt->rd_flags & VMS_TT_RD_CVTLOW) && c >= 'a' && c <= 'z')
		c = (uint8_t)(c - 'a' + 'A');
	tt_hc_close(tt);
	if (tt->cur < tt->len && tt->ovs) {
		tt->line[tt->cur++] = c;            /* overstrike */
		if (tt_echoing(tt) && (c >= 0x20 || c == CH_TAB))
			tt_out1(tt, c);
		return;
	}
	if (tt->cur < tt->len) {                    /* insert */
		memmove(tt->line + tt->cur + 1, tt->line + tt->cur, tt->len - tt->cur);
		tt->line[tt->cur++] = c;
		tt->len++;
		if (tt_echoing(tt) && (c >= 0x20 || c == CH_TAB)) {
			tt_out1(tt, c);
			tt_echo_tail(tt, tt->cur, 0);
		}
	} else {
		tt->line[tt->len++] = c;
		tt->cur = tt->len;
		if (tt_echoing(tt) && (c >= 0x20 || c == CH_TAB))
			tt_out_linech(tt, tt->len - 1);
	}
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
	memset(tt->oob, 0, sizeof(tt->oob)); /* no line, no out-of-band */
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
 * Queue an AST for `proc` -- the executive's own AST queue, exactly as a
 * mailbox write-attention or a lock completion does (vms_mbx.c, vms_lock.c):
 * atomic allocation (tt->lock is held), proc->ast[mode].lock innermost, the
 * arrival wake after it is dropped. A full queue drops it (the quota is the
 * process's). Returns 1 when queued.
 */
static int tt_queue_ast(struct vms_proc *proc, uint64_t astadr, uint64_t astprm,
			uint8_t acmode)
{
	struct vms_ast_entry *ast = exec_zalloc_atomic(sizeof(*ast));
	struct vms_ast_state *st;

	if (!ast)
		return 0;
	ast->astadr = astadr;
	ast->astprm = astprm;
	ast->acmode = acmode;
	st = &proc->ast[acmode];
	exec_lock(&st->lock);
	if (st->count >= VMS_AST_MAX_PER_MODE) {
		exec_unlock(&st->lock);
		exec_free(ast);
		return 0;
	}
	exec_list_add_tail(&ast->list, &st->pending);
	st->count++;
	exec_unlock(&st->lock);
	vms_ast_notify_arrival(proc);
	return 1;
}

/* Fire out-of-band entry `o`, under tt->lock. A process waiting in this
 * terminal's read is let go to deliver it (the read stays outstanding). */
static void tt_oob_fire(struct vms_tt *tt, struct tt_oob *o, uint64_t astprm)
{
	if (!tt_queue_ast(o->proc, o->astadr, astprm, o->acmode))
		return;
	if (tt->rd_busy && tt->rd_owner == (const void *)o->proc) {
		tt->rd_astpend = 1;
		exec_cv_broadcast(&tt->cv);
	}
}

/* *INTERRUPT* -- what the driver shows for CTRL/Y and CTRL/C, with an AST
 * armed or not (probes OB.PROMPT Y1/Y3/C2/N2, VAX V7.3): a new line, the word,
 * CR LF; the line after it is not a fresh one (the next prompt starts with its
 * own CR LF). */
static void tt_echo_interrupt(struct vms_tt *tt)
{
	tt_nl(tt);
	tt_out(tt, "*INTERRUPT*\r\n", 13);
	tt->pos = TT_POS_MID;
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
				/*
				 * CTRL/Y, CTRL/C (rd vms-f0fb). The driver echoes
				 * *INTERRUPT* and discards the type-ahead. With a
				 * CTRL/C AST armed a CTRL/C fires it; otherwise, and
				 * for CTRL/Y, the CTRL/Y AST. A fired AST is spent
				 * (re-armed by its owner) and the read in progress
				 * ends SS$_ABORT: its line is gone. With NO AST
				 * armed nothing is interrupted -- the character ends
				 * the read like a terminator, its line intact (probe
				 * OB.PROMPT N2: SET NOCONTROL=Y, "AB" ^Y -> DCL runs
				 * AB).
				 */
				struct tt_oob *o = NULL;

				tt_echo_interrupt(tt);
				tt_ta_purge(tt);
				if (c == TT_CTRL('C') && tt->oob[VMS_TT_OOB_CTRLC - 1].proc)
					o = &tt->oob[VMS_TT_OOB_CTRLC - 1];
				else if (tt->oob[VMS_TT_OOB_CTRLY - 1].proc)
					o = &tt->oob[VMS_TT_OOB_CTRLY - 1];
				if (o) {
					struct tt_oob fire = *o;

					o->proc = NULL;          /* spent */
					if (tt->rd_active)
						tt_complete(tt, SS__ABORT, 0, 0);
					tt_oob_fire(tt, &fire, fire.astprm);
					/* the substrate interrupt still stops an
					 * image the AST owner is waiting on */
					if (nintr < sizeof(intr))
						intr[nintr++] = c;
				} else if (tt->rd_active) {
					tt_complete(tt, SS__NORMAL, c, 1);
				}
				continue;
			}
			if (c == TT_CTRL('O')) {
				/*
				 * CTRL/O: discard output until the next CTRL/O or
				 * read (VAX V7.3, OOB.CTRLO: "<LF>*OUTPUT OFF*<CR>
				 * <LF>" while a procedure writes, "*OUTPUT ON*<CR>
				 * <LF>" and the output resumes). At a read there is
				 * no output to stop: it is not data and shows
				 * nothing (OB.PROMPT O1, OOB.PROMPT O/O2).
				 */
				if (!tt->rd_active) {
					tt->ctrlo = !tt->ctrlo;
					tt_nl(tt);
					if (tt->ctrlo)
						tt_out(tt, "*OUTPUT OFF*\r\n", 14);
					else
						tt_out(tt, "*OUTPUT ON*\r\n", 13);
					tt->pos = TT_POS_CLEAN;
				}
				continue;
			}
			if (c < 0x20 && tt->oob[VMS_TT_OOB_OUTBAND - 1].proc &&
			    (tt->oob[VMS_TT_OOB_OUTBAND - 1].mask & (1u << c))) {
				/* an out-of-band character: never data, never
				 * echoed; its AST carries it (stays armed) */
				tt_oob_fire(tt, &tt->oob[VMS_TT_OOB_OUTBAND - 1], c);
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
					tt->cur = tt->len;   /* all of it */
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
	if (nflow || nintr) {
		struct tt_portref pr;

		if (tt_port_enter(tt, &pr)) {
			for (i = 0; i < nflow; i++)
				if (pr.ops->flow)
					pr.ops->flow(pr.port, flow[i] == TT_CTRL('S'));
			for (i = 0; i < nintr; i++)
				if (pr.ops->interrupt)
					pr.ops->interrupt(pr.port, intr[i]);
			tt_port_exit(tt);
		}
	}
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
	tt->rd_astpend = 0;
	tt->rd_disturbed = 0;
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
	uint32_t kick0;

	memset(res, 0, sizeof(*res));
	if (req->bufsz == 0) {
		res->status = SS__NORMAL;      /* a zero-length read completes */
		return 0;
	}

	exec_lock(&tt->lock);
	tt->refs++;
	kick0 = tt->ldisc_kick;

	if (tt->rd_busy && tt->rd_suspended && tt->rd_owner == req->owner &&
	    req->owner != NULL) {
		tt->rd_suspended = 0;            /* the same read, resumed */
		resumed = 1;
	}

	/* one read at a time: later readers queue */
	while (!resumed && tt->rd_busy && !tt->detached &&
	       !(req->ldisc && tt->ldisc_kick != kick0)) {
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
	if (!intr && !tt->detached && req->ldisc && tt->ldisc_kick != kick0) {
		res->status = SS__HANGUP;     /* the line's session hung up */
		exec_unlock(&tt->lock);
		tt_put(tt);
		return 0;
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
		tt->rd_ldisc = req->ldisc;
		tt->rd_flags = req->flags;
		tt->rd_cap = req->bufsz < VMS_TT_LINE_MAX ? req->bufsz : VMS_TT_LINE_MAX;
		tt->rd_dc = dc;
		tt->rd_width = tt_devwidth(tt->dev);
		tt->len = 0;
		tt->cur = 0;
		tt->ovs = !(dc & VMS_TTC_INSERT_EDITING);
		tt->hc_del = 0;
		tt->esc = 0;
		memset(&tt->res, 0, sizeof(tt->res));
		if (req->flags & VMS_TT_RD_TERMMASK)
			memcpy(tt->rd_mask, req->termmask, sizeof(tt->rd_mask));
		else
			tt_std_mask(tt->rd_mask);
		tt->promptsz = 0;
		tt->rd_pcol = 0;
		if (req->prompt && req->promptsz) {
			uint32_t q;

			tt->promptsz = req->promptsz < VMS_TT_PROMPT_MAX ? req->promptsz
									 : VMS_TT_PROMPT_MAX;
			memcpy(tt->prompt, req->prompt, tt->promptsz);
			for (q = 0; q < tt->promptsz; q++) {
				uint8_t pc = tt->prompt[q];
				if (pc == CH_CR || pc == CH_LF)
					tt->rd_pcol = 0;
				else if (pc >= 0x20)
					tt->rd_pcol++;
			}
		}
		tt->rd_deadline = 0;
		if (req->flags & VMS_TT_RD_TIMED)
			tt->rd_deadline = exec_ticks_ms() + (uint64_t)req->timeout_s * 1000u;

		tt->ctrlo = 0;               /* a read ends CTRL/O */
		if (req->flags & VMS_TT_RD_PURGE)
			tt_ta_purge(tt);
		/* a record's owed line feed is paid by a read that echoes, before
		 * its prompt; a NOECHO read leaves the line where it is (probes
		 * CC.MIX A2 vs N3) */
		tt->rec_open = 0;
		if (tt->pos == TT_POS_CR && tt_echoing(tt))
			tt_out1(tt, CH_LF);
		/* the prompt is written BEFORE any type-ahead is consumed and echoed */
		if (tt->promptsz)
			tt_prompt_out(tt, tt->prompt, tt->promptsz);
		/* TRM$_INISTRNG: the line starts with these characters, shown
		 * after the prompt, the cursor after them (a recalled command,
		 * RC.RECALL U1) */
		if (req->inistr && req->inisz) {
			uint32_t ni = req->inisz < tt->rd_cap ? req->inisz : tt->rd_cap;

			memcpy(tt->line, req->inistr, ni);
			tt->len = tt->cur = ni;
			if (tt_echoing(tt))
				tt_out(tt, tt->line, ni);
		}

		tt->rd_active = 1;
		tt_drain_typeahead(tt);
		if (tt->passall && tt->rd_active && tt->len)
			tt_complete(tt, SS__NORMAL, 0, 0);
	}

	while (!tt->rd_done && !tt->detached) {
		uint32_t olen = tt->olen;

		if (tt->rd_astpend && req->owner && tt->rd_owner == req->owner) {
			/* an AST the driver queued for this process (an out-of-
			 * band character): let the caller deliver it, keeping the
			 * read outstanding -- the same owner's next read resumes
			 * it, as after a signal (rd vms-f0fb) */
			tt->rd_astpend = 0;
			tt->rd_suspended = 1;
			tt->rd_susp_ms = exec_ticks_ms();
			exec_unlock(&tt->lock);
			tt_flush(tt);
			tt_put(tt);
			res->status = SS__NORMAL;
			return VMS_TT_READ_ASTPEND;
		}

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
 * vms_tt_write - output through the class driver. `cooked`: the bytes are a
 * program's records, '\n'-terminated (a writer arriving on the port's own
 * write path); otherwise they are written as they are ($QIO).
 */
int vms_tt_write(struct vms_tt *tt, const uint8_t *buf, size_t n, int cooked)
{
	uint8_t chunk[128];
	size_t i = 0;

	tt_flush(tt);                     /* echo queued before this write first */
	if (READ_ONCE_TT(tt->ctrlo))
		return 0;                    /* CTRL/O: the output is discarded */
	while (i < n) {
		size_t k = 0;
		int rc;

		if (READ_ONCE_TT(tt->detached))
			return -EIO;             /* the line went away */

		/*
		 * Build the chunk under the lock: the cursor position is driver
		 * state shared with echo. A COOKED write (a program's output
		 * through its terminal's descriptor: printf) is a run of
		 * RECORDS, one per '\n' -- implied carriage control, as the
		 * CRTL's output is on VMS: a new line before each record
		 * (rendered for where the cursor is, tt_nl), and a carriage
		 * return after it that leaves the line feed owed. A raw write
		 * ($QIO IO$_WRITEVBLK, no carriage control) is the bytes.
		 */
		exec_lock(&tt->lock);
		while (i < n && k < sizeof(chunk) - 4) {
			uint8_t c = buf[i];

			if (cooked) {
				if (!tt->rec_open) {
					uint32_t o0 = tt->olen;

					tt->rec_open = 1;
					tt_nl(tt);   /* into obuf: move it here */
					while (tt->olen > o0) {
						chunk[k++] = tt->obuf[o0];
						memmove(tt->obuf + o0, tt->obuf + o0 + 1,
							--tt->olen - o0);
					}
				}
				if (c == CH_LF) {
					if (tt->last != CH_CR) {
						chunk[k++] = CH_CR;
						tt_track(tt, CH_CR);
					}
					tt->pos = TT_POS_CR;   /* the LF is owed */
					tt->rec_open = 0;
					i++;
					continue;
				}
			}
			chunk[k++] = c;
			tt_track(tt, c);
			i++;
		}
		exec_unlock(&tt->lock);
		for (;;) {
			struct tt_portref pr;
			int swapped;

			if (!tt_port_enter(tt, &pr))
				return -EIO;     /* the line went away */
			if (pr.ops->write) {
				rc = pr.ops->write(pr.port, chunk, k);
			} else {
				pr.ops->xmit(pr.port, chunk, k);
				rc = 0;
			}
			tt_port_exit(tt);
			/* the port was replaced under this write (its old line
			 * re-opened): the chunk goes to the new one */
			exec_lock(&tt->lock);
			swapped = (rc == -EIO && tt->port_gen != pr.gen && !tt->detached);
			exec_unlock(&tt->lock);
			if (!swapped)
				break;
		}
		if (rc)
			return rc;
	}

	/*
	 * WRITE BREAKTHROUGH (rd vms-f0fb, vms-53a). Output written while a read
	 * is outstanding -- a CTRL/T status line, a broadcast, another process's
	 * write -- lands on the read's line; the driver then shows the read
	 * again: the owed line feed, its prompt, what was typed so far (probe
	 * OB.PROMPT T2, VAX V7.3: <CR><LF>status<CR><LF><CR><NUL>$ ABC).
	 */
	exec_lock(&tt->lock);
	if (tt->rd_busy && !tt->rd_done && !tt->passall && n) {
		if (tt->pos == TT_POS_CR && tt_echoing(tt)) {
			tt_out1(tt, CH_LF);
			tt->rec_open = 0;
		}
		if (tt->promptsz)
			tt_prompt_out(tt, tt->prompt, tt->promptsz);
		if (tt_echoing(tt))
			tt_out(tt, tt->line, tt->len);
	}
	exec_unlock(&tt->lock);
	tt_flush(tt);
	return 0;
}

/*
 * vms_tt_kick_ldisc - the line's SESSION hung up (not the line): a read the
 * substrate's own read(2) path is blocked in -- holding the substrate's line-
 * discipline reference, which the hangup must take -- ends SS$_HANGUP (read(2)
 * returns 0, as for any hung-up terminal), and so does one queued behind
 * another read. A $QIO read is the unit's, not the session's: it goes on.
 */
void vms_tt_kick_ldisc(struct vms_tt *tt)
{
	exec_lock(&tt->lock);
	tt->ldisc_kick++;
	if (tt->rd_active && tt->rd_ldisc)
		tt_complete(tt, SS__HANGUP, 0, 0);
	exec_cv_broadcast(&tt->cv);
	exec_unlock(&tt->lock);
}

/*
 * vms_tt_set_port - the instance's line was re-opened under it: carry on with
 * the new port. A VMS terminal unit does not hang up because one session on
 * its line ended -- the Linux console's line discipline is closed and re-opened
 * whenever a session leader whose controlling terminal it was exits
 * (__tty_hangup), and OPA0: must survive that with its type-ahead, its
 * outstanding reads and its binding intact. Waits until no op on the old port
 * is in flight, then releases it (ops->release). Process context.
 */
void vms_tt_set_port(struct vms_tt *tt, const struct vms_tt_port_ops *ops, void *port)
{
	const struct vms_tt_port_ops *old_ops;
	void *old;

	exec_lock(&tt->lock);
	while (tt->port_busy) {
		int to = 0;
		(void)exec_cv_wait_timeout(&tt->cv, &tt->lock, 50, &to);
	}
	old_ops = tt->ops;
	old = tt->port;
	tt->ops = ops;
	tt->port = port;
	tt->port_gen++;
	exec_unlock(&tt->lock);
	if (old_ops && old_ops->release)
		old_ops->release(old);
	tt_flush(tt);                     /* echo queued meanwhile */
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
	uint8_t *line = NULL, *prompt = NULL, *ini = NULL;
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
	if ((a.flags & VMS_TT_RD_INISTR) && a.inistr && a.inisz) {
		rq.inisz = a.inisz < rq.bufsz ? a.inisz : rq.bufsz;
		ini = rq.inisz ? exec_alloc(rq.inisz) : NULL;
		if (rq.inisz && !ini) {
			a.status = SS__INSFMEM;
			goto out_rel;
		}
		if (rq.inisz && exec_copyin(ini, (const void *)(uintptr_t)a.inistr, rq.inisz)) {
			a.status = SS__ACCVIO;
			goto out_rel;
		}
		rq.inistr = ini;
	}
	line = exec_alloc(VMS_TT_LINE_MAX);
	if (!line) {
		a.status = SS__INSFMEM;
		goto out_rel;
	}

	/* a signal suspends the read; this process's re-entry resumes it */
	rq.owner = proc;
	{
		int rrc = vms_tt_read(tt, &rq, line, &r);

		if (rrc == -ERESTARTSYS) {
			vms_tt_release(tt);
			exec_free(line);
			if (prompt)
				exec_free(prompt);
			if (ini)
				exec_free(ini);
			return -ERESTARTSYS;    /* no status written: re-enter */
		}
		a.oflags = 0;
		if (rrc == VMS_TT_READ_ASTPEND) {
			/* still outstanding: deliver the AST, read again */
			a.oflags = VMS_TT_RDO_ASTPEND;
			a.status = SS__NORMAL;
			a.count = 0;
			goto out_rel;
		}
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
	if (ini)
		exec_free(ini);
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
		/* a record (VMS_TT_WR_RECORD): the cooked path's new line before
		 * and carriage return after; the text itself carries no '\n' */
		if (vms_tt_write(tt, chunk, k, (a.flags & VMS_TT_WR_RECORD) != 0)) {
			a.status = SS__ABORT;
			break;
		}
		done += k;
	}
	if ((a.flags & VMS_TT_WR_RECORD) && a.status == SS__NORMAL && a.len &&
	    vms_tt_write(tt, (const uint8_t *)"\n", 1, 1))
		a.status = SS__ABORT;
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

/*
 * VMS_IOCTL_TT_OOBAST (rd vms-f0fb): $QIO IO$_SETMODE!IO$M_CTRLYAST /
 * IO$M_CTRLCAST / IO$M_OUTBAND on a channel to a terminal -- arm (astadr) or
 * disarm (0) an out-of-band AST (I/O User's Reference, "Terminal Driver").
 * The AST is delivered at the less privileged of the requested mode and the
 * caller's. Re-arming replaces the previous entry, whoever armed it.
 */
long vms_ioctl_tt_oobast(struct vms_proc *proc, unsigned long arg)
{
	struct vms_tt_oobast_args a;
	struct vms_tt *tt;
	struct tt_oob *o;
	uint32_t st = SS__NORMAL;
	uint8_t mode;

	memset(&a, 0, sizeof(a));
	if (exec_copyin(&a, (const void *)arg, sizeof(a)))
		return -EFAULT;
	if (a.which < VMS_TT_OOB_CTRLY || a.which > VMS_TT_OOB_OUTBAND || a.acmode > 3) {
		a.status = SS__BADPARAM;
		goto out;
	}
	tt = tt_from_chan(proc, a.chan, &st);
	if (!tt) {
		a.status = st;
		goto out;
	}
	mode = (uint8_t)a.acmode;
	exec_lock(&proc->mode_lock);
	if (mode < proc->current_mode)
		mode = proc->current_mode;
	exec_unlock(&proc->mode_lock);

	exec_lock(&tt->lock);
	o = &tt->oob[a.which - 1];
	if (a.astadr == 0) {
		memset(o, 0, sizeof(*o));
	} else {
		o->proc = proc;
		o->owner = proc->linux_pid;
		o->chan = a.chan;
		o->astadr = a.astadr;
		o->astprm = a.astprm;
		o->acmode = mode;
		o->mask = a.which == VMS_TT_OOB_OUTBAND ? a.mask : 0;
	}
	exec_unlock(&tt->lock);
	vms_tt_release(tt);
	a.status = SS__NORMAL;
out:
	if (exec_copyout((void *)arg, &a, sizeof(a)))
		return -EFAULT;
	return 0;
}

void vms_tt_chan_gone(struct vms_device *dev, pid_t owner_linux_pid, uint32_t chan)
{
	struct vms_tt *tt = vms_tt_of(dev);
	int i;

	if (!tt)
		return;
	exec_lock(&tt->lock);
	for (i = 0; i < 3; i++)
		if (tt->oob[i].proc && tt->oob[i].owner == owner_linux_pid &&
		    tt->oob[i].chan == chan)
			memset(&tt->oob[i], 0, sizeof(tt->oob[i]));
	exec_unlock(&tt->lock);
	vms_tt_release(tt);
}

/*
 * VMS_IOCTL_TT_BRKTHRU (rd vms-53a): a broadcast written to terminal `devnam`
 * through its class driver. A read in progress there is broken through and then
 * shown again (vms_tt_write's breakthrough redisplay; keystroke BC.READ W).
 * Writing to a terminal is OPER, decided here (vms_prot.h). A row no port is
 * attached to answers SS$_DEVOFFLINE.
 */
long vms_ioctl_tt_brkthru(struct vms_proc *proc, unsigned long arg)
{
	struct vms_tt_brkthru_args *a;
	struct vms_tt *tt;
	long rc = 0;

	a = exec_alloc(sizeof(*a));
	if (!a)
		return -ENOMEM;
	if (exec_copyin(a, (const void *)arg, sizeof(*a))) {
		exec_free(a);
		return -EFAULT;
	}
	a->devnam[sizeof(a->devnam) - 1] = '\0';
	a->status = vms_prot_require_priv(proc->cur_privs, VMS_PRV_M_OPER);
	if (!(a->status & 1))
		goto out;
	if (a->len > sizeof(a->msg)) {
		a->status = SS__BADPARAM;
		goto out;
	}
	tt = vms_devtab_tt_by_name(a->devnam);
	if (!tt) {
		a->status = SS__DEVOFFLINE;
		goto out;
	}
	a->status = vms_tt_write(tt, (const uint8_t *)a->msg, a->len, 0) ? SS__DEVOFFLINE
									    : SS__NORMAL;
	vms_tt_release(tt);
out:
	if (exec_copyout((void *)arg, a, sizeof(*a)))
		rc = -EFAULT;
	exec_free(a);
	return rc;
}
