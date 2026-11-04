/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_tt_oper.h - an EXECUTIVE OPERATOR LINE on a terminal that another writer
 * is part-way through (rd vms-896d).
 *
 * THE DEFECT THIS EXISTS FOR (P0 proof ae84d1fc6, OVMXB console, 2026-11-03).
 * OPCOM's operator-status message was going out on OPA0: -- one write, three
 * lines -- and the executive's "%CNXMAN,  now a VAXcluster member -- system
 * OVMXB" was written straight to the console between two of that write's
 * pieces. The console showed
 *
 *     LICENSE, OPER1, ... OPER8, OPER9, OP
 *     %CNXMAN,  now a VAXcluster member -- system OVMXBER10,
 *     OPER11, OPER12
 *
 * The executive line started on a new line but ended without one, and OPCOM's
 * record was cut in two with the rest of it glued to the executive's text.
 *
 * WHAT VMS DOES (OpenVMS I/O User's Reference Manual, "Terminal Driver":
 * broadcast messages and write breakthrough; System Services Reference,
 * $BRKTHRU). Output to a terminal is a queue of I/O requests the terminal
 * driver performs one at a time: a message for the terminal does not cut into
 * a write request that is going out -- it is written when that write
 * completes, on a line of its own (a new line before it, CR LF after it), and
 * a read in progress is then shown again. So the console carries OPCOM's whole
 * record, then the executive's line, then whatever comes next on a fresh line.
 *
 * THE RULE, here: the class driver (src/kernel-core/vms_tt.c) counts the write
 * requests going out on the terminal. An executive line that arrives while one
 * is going out is HELD; the write's end releases it. One that arrives between
 * writes goes out at once. Either way it is framed the same: a new line unless
 * the cursor already stands at the start of an empty line, the text, CR LF.
 * Nothing is dropped: a line that does not fit the hold is refused, and the
 * caller writes it itself (the substrate's console path) -- the kernel log
 * keeps it in every case.
 *
 * Pure: no includes, no library calls -- the same text compiles into vms.ko,
 * the NetBSD module and the host test (tests/cluster/host/test_tt_oper.c).
 * Clean-room (AGENTS.md Rule 8): public documentation and observed console
 * bytes only.
 */
#ifndef OVMX_VMS_TT_OPER_H
#define OVMX_VMS_TT_OPER_H

/* Executive lines held behind one write request: a handful at most (a cluster
 * transition announces two or three); each oracle line is under 80 bytes. */
#define VMS_TT_OPER_HOLD 512

struct vms_tt_oper {
	unsigned char held[VMS_TT_OPER_HOLD];  /* lines, each ended by '\n' */
	unsigned int  len;
};

/* The text of `line` without the line ends a caller may have put on it. */
static inline unsigned int vms_tt_oper_textlen(const char *line)
{
	unsigned int n = 0;

	while (line[n] != '\0')
		n++;
	while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
		n--;
	return n;
}

/*
 * One line, framed as the terminal shows it: CR LF first unless `fresh` (the
 * cursor is at column 0 of an empty line), the text, CR LF. The bytes go to
 * `out`; returns how many, or 0 if they do not all fit (nothing is written).
 */
static inline unsigned int vms_tt_oper_frame(int fresh, const unsigned char *text,
					     unsigned int n, unsigned char *out,
					     unsigned int outsz)
{
	unsigned int need = n + 2u + (fresh ? 0u : 2u), at = 0, i;

	if (need > outsz)
		return 0;
	if (!fresh) {
		out[at++] = '\r';
		out[at++] = '\n';
	}
	for (i = 0; i < n; i++)
		out[at++] = text[i];
	out[at++] = '\r';
	out[at++] = '\n';
	return at;
}

/*
 * An executive line for the terminal. `writing`: write requests going out on
 * it right now. Returns
 *    > 0  the framed bytes are in `out` -- write them now;
 *    0    held, to go out when the write ends (vms_tt_oper_release);
 *   -1    refused (no room to hold or to frame it) -- the caller writes it.
 */
static inline int vms_tt_oper_submit(struct vms_tt_oper *q, int writing, int fresh,
				     const char *line, unsigned char *out,
				     unsigned int outsz)
{
	unsigned int n = vms_tt_oper_textlen(line), i, k;

	if (writing) {
		if (q->len + n + 1u > VMS_TT_OPER_HOLD)
			return -1;
		for (i = 0; i < n; i++)
			q->held[q->len + i] = (unsigned char)line[i];
		q->held[q->len + n] = '\n';
		q->len += n + 1u;
		return 0;
	}
	k = vms_tt_oper_frame(fresh, (const unsigned char *)line, n, out, outsz);
	return k ? (int)k : -1;
}

/*
 * The last write request on the terminal has ended: every held line, framed
 * and in arrival order, into `out`. The first line's new line is rendered for
 * where the write left the cursor (`fresh`); every later one starts where the
 * one before it ended, at column 0 of an empty line. Returns the bytes
 * written; what does not fit stays held for the next release.
 */
static inline unsigned int vms_tt_oper_release(struct vms_tt_oper *q, int fresh,
					       unsigned char *out, unsigned int outsz)
{
	unsigned int at = 0, used = 0, i;

	while (used < q->len) {
		unsigned int n = 0, k;

		while (q->held[used + n] != '\n')
			n++;
		k = vms_tt_oper_frame(fresh, q->held + used, n, out + at, outsz - at);
		if (k == 0)
			break;
		at += k;
		used += n + 1u;
		fresh = 1;
	}
	for (i = used; i < q->len; i++)
		q->held[i - used] = q->held[i];
	q->len -= used;
	return at;
}

#endif /* OVMX_VMS_TT_OPER_H */
