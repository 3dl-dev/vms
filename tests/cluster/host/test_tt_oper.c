/*
 * test_tt_oper.c - an executive operator line never splits a write request
 * going out on the terminal (rd vms-896d; src/kernel-core/vms_tt_oper.h).
 *
 * THE INTERLEAVE THIS REPRODUCES (P0 proof ae84d1fc6, OVMXB console,
 * page-order-throttled4 r5). OPCOM wrote its operator-status message to
 * OPA0: as one write request; the class driver sends a request out in
 * pieces; "%CNXMAN,  now a VAXcluster member -- system OVMXB" arrived between
 * two of them and went to the console at once, with no line end after it:
 *
 *     LICENSE, OPER1, ... OPER8, OPER9, OP
 *     %CNXMAN,  now a VAXcluster member -- system OVMXBER10,
 *     OPER11, OPER12
 *
 * Below, a terminal is driven the way src/kernel-core/vms_tt.c drives it
 * (vms_tt_write_begin / pieces / vms_tt_write_end; vms_tt_oper_line between
 * them) with OPCOM's real message, and the line is made to arrive at EVERY
 * byte of the request. The console must read OPCOM's whole record, then the
 * executive line on its own, CR LF-ended -- byte for byte. The source checks
 * at the end hold the shipping class driver and the NetBSD console path to
 * that same sequence.
 *
 * negctl: tt-oper-line-splices-write (tests/cluster/host/host_defects.sh).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* memmem */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cluster_test.h"
#include "vms_tt_oper.h"

/* ---- a terminal, driven as vms_tt.c drives one ------------------------- */

struct term {
	unsigned char screen[8192];      /* every byte the port was given */
	unsigned int  len;
	unsigned int  wr_reqs;
	struct vms_tt_oper oper;
};

/* tt_at_fresh_line: column 0 of an empty line */
static int term_fresh(const struct term *t)
{
	return t->len == 0 ||
	       (t->len >= 2 && t->screen[t->len - 2] == '\r' && t->screen[t->len - 1] == '\n');
}

static void term_put(struct term *t, const void *p, unsigned int n)
{
	memcpy(t->screen + t->len, p, n);
	t->len += n;
}

static void term_begin(struct term *t) { t->wr_reqs++; }

static void term_end(struct term *t)
{
	if (t->wr_reqs > 0 && --t->wr_reqs == 0 && t->oper.len)
		t->len += vms_tt_oper_release(&t->oper, term_fresh(t), t->screen + t->len,
					      (unsigned int)sizeof(t->screen) - t->len);
}

/* vms_tt_oper_line: 1 taken (held or written), 0 refused */
static int term_oper_line(struct term *t, const char *line)
{
	int r = vms_tt_oper_submit(&t->oper, t->wr_reqs != 0, term_fresh(t), line,
				   t->screen + t->len,
				   (unsigned int)sizeof(t->screen) - t->len);

	if (r > 0)
		t->len += (unsigned int)r;
	return r >= 0;
}

/* ---- OPCOM's message, as the OVMXB console carried it ------------------ */

static const char opcom[] =
	"\r\n%%%%%%%%%%%  OPCOM   3-NOV-2026 10:18:05.38  %%%%%%%%%%%\r\n"
	"Operator status for operator _OVMXB$OPA0:\r\n"
	"CENTRAL, PRINTER, TAPES, DISKS, DEVICES, CARDS, NETWORK, CLUSTER, SECURITY,\r\n"
	"LICENSE, OPER1, OPER2, OPER3, OPER4, OPER5, OPER6, OPER7, OPER8, OPER9, OPER10,\r\n"
	"OPER11, OPER12\r\n";
static const char member[] = "%CNXMAN,  now a VAXcluster member -- system OVMXB";

/* OPCOM's request with the executive line arriving after byte `cut`. */
static int run_cut(struct term *t, unsigned int cut, const char *line)
{
	unsigned int n = (unsigned int)strlen(opcom);
	int taken;

	memset(t, 0, sizeof(*t));
	term_begin(t);                      /* the $QIO write */
	term_put(t, opcom, cut);            /* its pieces so far */
	taken = term_oper_line(t, line);
	term_put(t, opcom + cut, n - cut);  /* the rest of the request */
	term_end(t);
	return taken;
}

static void test_capture_cut(void)
{
	struct term t;
	char want[1024];
	unsigned int cut = (unsigned int)(strstr(opcom, "ER10,") - opcom);

	printf("--- the capture: the line arrives at 'OPER9, OP|ER10,' ---\n");
	ct_check(run_cut(&t, cut, member), "the class driver takes the line");
	snprintf(want, sizeof want, "%s%s\r\n", opcom, member);
	/* negctl: tt-oper-line-splices-write */
	ct_check(t.len == strlen(want) && memcmp(t.screen, want, t.len) == 0,
		 "OPCOM's whole record, then the executive line on a line of its own, CR LF after it");
	ct_check(memmem(t.screen, t.len, "OVMXBER10", 9) == NULL,
		 "nothing of OPCOM's record is glued to the executive line (no 'OVMXBER10')");
	ct_check(memmem(t.screen, t.len, "OPER9, OPER10,", 14) != NULL,
		 "OPCOM's record is not split ('OPER9, OPER10,' intact)");
}

static void test_every_cut(void)
{
	struct term t;
	char want[1024];
	unsigned int n = (unsigned int)strlen(opcom), cut, bad = 0;

	printf("--- the line arriving at every byte of the request ---\n");
	snprintf(want, sizeof want, "%s%s\r\n", opcom, member);
	for (cut = 0; cut <= n; cut++) {
		if (!run_cut(&t, cut, member) || t.len != strlen(want) || memcmp(t.screen, want, t.len) != 0)
			bad++;
	}
	/* negctl: tt-oper-line-splices-write */
	ct_check_eq_u32(bad, 0, "cuts at which the console differs from record-then-line");
}

static void test_between_writes(void)
{
	struct term t;
	const char *want;

	printf("--- between writes: written at once, framed for the cursor ---\n");
	memset(&t, 0, sizeof t);
	term_begin(&t);
	term_put(&t, "\r\nUsername: ", 12);         /* a prompt: mid-line */
	term_end(&t);
	ct_check(term_oper_line(&t, member), "taken");
	want = "\r\nUsername: \r\n%CNXMAN,  now a VAXcluster member -- system OVMXB\r\n";
	ct_check(t.len == strlen(want) && memcmp(t.screen, want, t.len) == 0,
		 "mid-line: a new line first, the text, CR LF");

	memset(&t, 0, sizeof t);
	term_put(&t, "ABC\r\n", 5);                 /* column 0 of an empty line */
	ct_check(term_oper_line(&t, "%CNXMAN,  sending VAXcluster membership request to system VAXC\n"),
		 "taken");
	want = "ABC\r\n%CNXMAN,  sending VAXcluster membership request to system VAXC\r\n";
	ct_check(t.len == strlen(want) && memcmp(t.screen, want, t.len) == 0,
		 "at a fresh line: no blank line, the caller's own line end not doubled");
}

static void test_held_lines(void)
{
	struct term t;
	const char *want;
	unsigned int i, held = 0;
	unsigned char small[40];

	printf("--- held lines: in order, after the write, never lost ---\n");
	memset(&t, 0, sizeof t);
	term_begin(&t);
	term_put(&t, "$ ", 2);                      /* the write leaves the cursor mid-line */
	ct_check(term_oper_line(&t, "%CNXMAN,  completing VAXcluster state transition"), "taken");
	ct_check(term_oper_line(&t, "%CNXMAN,  now a VAXcluster member -- system B"), "taken");
	ct_check_eq_u32(t.len, 2, "nothing written while the request goes out");
	term_put(&t, "X", 1);
	term_end(&t);
	want = "$ X\r\n%CNXMAN,  completing VAXcluster state transition\r\n"
	       "%CNXMAN,  now a VAXcluster member -- system B\r\n";
	ct_check(t.len == strlen(want) && memcmp(t.screen, want, t.len) == 0,
		 "both lines follow the write, in arrival order, each on its own line");

	memset(&t, 0, sizeof t);
	term_begin(&t);
	term_begin(&t);                             /* a piece inside the request */
	ct_check(term_oper_line(&t, member), "taken");
	term_end(&t);
	ct_check_eq_u32(t.len, 0, "a piece ending is not the request ending");
	term_end(&t);
	ct_check(t.len > 0, "the request ending releases the line");

	memset(&t, 0, sizeof t);
	for (i = 0; i < 64; i++)
		held += (unsigned int)(vms_tt_oper_submit(&t.oper, 1, 1, member, NULL, 0) == 0);
	ct_check(held > 0 && held < 64, "the hold is bounded");
	ct_check(vms_tt_oper_submit(&t.oper, 1, 1, member, NULL, 0) == -1,
		 "a line the hold cannot take is refused (the caller writes it), not dropped");

	memset(&t, 0, sizeof t);
	(void)vms_tt_oper_submit(&t.oper, 1, 1, member, NULL, 0);
	ct_check_eq_u32(vms_tt_oper_release(&t.oper, 1, small, sizeof small), 0,
			"a release with no room for the line writes nothing");
	ct_check(t.oper.len > 0, "... and keeps the line held");
}

/* ---- the shipping code drives it the same way --------------------------- */

static char *slurp(const char *rel, size_t *len)
{
	char path[1024];
	FILE *f;
	char *buf;
	long n;

	snprintf(path, sizeof path, "%s/%s", OVMX_SOURCE_DIR, rel);
	f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1u);
	if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	buf[n] = '\0';
	*len = (size_t)n;
	return buf;
}

/* `a` occurs in `src` after the start of function `fn`, and `b` after `a`. */
static int in_order(const char *src, const char *fn, const char *a, const char *b)
{
	const char *f = strstr(src, fn), *p;

	if (f == NULL || (p = strstr(f, a)) == NULL)
		return 0;
	return b == NULL || strstr(p + strlen(a), b) != NULL;
}

static void test_source(void)
{
	size_t n;
	char *tt = slurp("src/kernel-core/vms_tt.c", &n);
	char *nb = slurp("src/kernel-netbsd/vms_tt_netbsd.c", &n);
	char *kb = slurp("src/kernel-netbsd/exec_kbackend_netbsd.h", &n);

	printf("--- the shipping class driver and console path ---\n");
	ct_check(tt && in_order(tt, "\nint vms_tt_write(", "vms_tt_write_begin(tt);",
				"vms_tt_write_end(tt);"),
		 "vms_tt_write brackets its pieces as one request");
	ct_check(tt && in_order(tt, "\nlong vms_ioctl_tt_write(", "vms_tt_write_begin(tt);",
				"vms_tt_write_end(tt);"),
		 "a $QIO write is ONE request, however many pieces");
	ct_check(tt && in_order(tt, "\nvoid vms_tt_write_end(", "vms_tt_oper_release(", NULL),
		 "the request's end releases held lines");
	ct_check(tt && in_order(tt, "\nint vms_tt_oper_line(", "vms_tt_oper_submit(&tt->oper, tt->wr_reqs != 0,", NULL),
		 "an executive line is held while a request goes out");
	ct_check(nb && in_order(nb, "\nvtt_write(", "vms_tt_write_begin(tt);",
				"vms_tt_write_end(tt);"),
		 "a write(2) on the NetBSD line is ONE request");
	ct_check(kb && in_order(kb, "\nexec_console_printf(", "vms_tt_oper_line(line)",
				"exec_console_opa0_put(\"\\n%s\", line);"),
		 "the NetBSD console line is offered to OPA0:'s class driver before any direct write");
	free(tt);
	free(nb);
	free(kb);
}

int main(void)
{
	printf("=== test_tt_oper (rd vms-896d) ===\n");
	test_capture_cut();
	test_every_cut();
	test_between_writes();
	test_held_lines();
	test_source();
	return ct_summary("test_tt_oper");
}
