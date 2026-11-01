/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cluster_opa0.c - which executive operator lines go on OPA0:, and the
 * VMS console form of the connection manager's events (rd vms-c56).
 *
 * Exercises the REAL src/kernel-core/vms_cluster_opa0.h -- the one decision
 * both substrates' exec_console_printf() make -- and grounds it three ways:
 *
 *   1. ROUTING. The V0.7-13 chatter the demo consoles showed (every line
 *      quoted in rd vms-c56) is a diagnostic: NOT a console line. The real
 *      VAX's own console lines ARE console lines, and so is every form the
 *      translator composes.
 *   2. THE ORACLE. Every console form is found, verbatim, on a real VMS
 *      console in this repository (the demo's real VAX, the VAX lab
 *      captures). A form nobody has seen VMS print fails here.
 *   3. THE SOURCE. Each diagnostic the translator keys on is still the exact
 *      text the connection manager composes (vms_cnxman_*.c / vms_cnxman.c):
 *      reword one there and this test, not a silent console, says so.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vms_cluster_opa0.h"
#include "cluster_test.h"

#ifndef OVMX_SOURCE_DIR
#error "OVMX_SOURCE_DIR must name the repository root"
#endif

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

/* A check label is one line: newlines and tabs in quoted text become spaces. */
static const char *one_line(char *s)
{
	char *p;

	for (p = s; *p != '\0'; p++)
		if (*p == '\n' || *p == '\t')
			*p = ' ';
	return s;
}

/* memmem, spelled out (consoles carry NULs, so no strstr). */
static int contains(const char *hay, size_t hlen, const char *needle)
{
	size_t nlen = strlen(needle), i;

	if (nlen == 0u || nlen > hlen)
		return 0;
	for (i = 0; i + nlen <= hlen; i++)
		if (memcmp(hay + i, needle, nlen) == 0)
			return 1;
	return 0;
}

/* Real VMS consoles: the demo's real VAX and VAX lab captures. */
static const char *const oracle_files[] = {
	"tests/lab/captures/vms-c56-console-chatter-20261021/C.console.txt",
	"tests/lab/captures/vms-52d-0b01-oracle-20261013/armA-vax2-console.log",
	"tests/lab/captures/vms-ci6-evac-20261009/vax2.log",
	"tests/lab/captures/vms-fcb-lockdirwt-20261004/L2-vax2.console.log",
	"tests/lab/captures/vms-dfe-blackout-recovery-20260925/main-wedges/VAXC.console.log",
};

static void test_routing(void)
{
	/* The V0.7-13 chatter, verbatim from the demo consoles. */
	static const char *const chatter[] = {
		"%CNXMAN, no coordinator CSID seen yet: this node's CSID was NOT learned\n",
		"%CNXMAN, lock directory weight vector, by member (compare with SDA SHOW CLUSTER on a VMS member):\n",
		"%CNXMAN,   csid=00010001 slot=1 lockdirwt=0 entries=1 [not this implementation]\n",
		"%MSCP_CL, the member answered NOT PRESENT HERE for MSCP$DISK: it serves no disks",
		"%CNXMAN, the executive already holds this pair's VMS$VAXcluster connection: this node opens none of its own and drives its admission on that one\n",
		"%DLM, refusing a lock message (cat 0x02 op 0x01) from system 0x10001: it has not proved it runs this implementation\n",
		"%PEA0, channel verified",
		"%PEA0, virtual circuit open",
		"%PEA0, cluster HELLO multicast group 257 (CLUSTER_AUTHORIZE)",
		"%CNXMAN, this node is now a VAXcluster member\n",
		"%CNXMAN, system 00000000000007c3 was added to the cluster\n",
		"%CNXMAN, completed VAXcluster state transition\n",
		"%CNXMAN, sending VAXcluster membership request to system VAXC\n",
		"%CNXMAN, waiting to form or join an OpenVMS Cluster\n",
		"vms: SCS path lost to system 0:1988 -> SS$ 8364\n",
	};
	/* The real VAX's own console lines (C.console.txt, vax2.log). */
	static const char *const vms[] = {
		"%CNXMAN,  proposing formation of a VAXcluster\n",
		"%CNXMAN,  now a VAXcluster member -- system VAXC\n",
		"%CNXMAN,  completing VAXcluster state transition\n",
		"%CNXMAN,  received VAXcluster membership request from system OVMXA\n",
		"%CNXMAN,  proposing addition of system OVMXA\n",
		"%CNXMAN,  lost connection to system OVMXE\n",
		"%PEA0, Port has Closed Virtual Circuit - REMOTE NODE OVMXE          \n",
		"\n%CNXMAN,  sending VAXcluster membership request to system VAX1",
	};
	unsigned int i;
	char what[256];

	printf("--- routing: chatter -> operator log, VMS forms -> OPA0: ---\n");
	for (i = 0; i < sizeof(chatter) / sizeof(chatter[0]); i++) {
		snprintf(what, sizeof what, "diagnostic, not OPA0: %.60s", chatter[i]);
		ct_check(!vms_opa0_is_console_line(chatter[i]), one_line(what));
	}
	for (i = 0; i < sizeof(vms) / sizeof(vms[0]); i++) {
		snprintf(what, sizeof what, "OPA0: %.60s", vms[i] + (vms[i][0] == '\n'));
		ct_check(vms_opa0_is_console_line(vms[i]), one_line(what));
	}
	ct_check(!vms_opa0_is_console_line(NULL), "NULL is not a console line");
	ct_check(!vms_opa0_is_console_line(""), "an empty line is not a console line");
}

static void check_form(const char *diag, const char *self, unsigned int self_len,
		       const char *want, const char *what)
{
	char out[VMS_OPA0_LINE_MAX];
	int got = vms_opa0_cnxman_form(diag, self, self_len, out, sizeof out);

	if (want == NULL) {
		ct_check(got == 0, what);
		return;
	}
	if (!got || strcmp(out, want) != 0)
		printf("    got %d \"%s\" want \"%s\"\n", got, out, want);
	ct_check(got && strcmp(out, want) == 0, what);
	ct_check(vms_opa0_is_console_line(out), "  ... and the composed line routes to OPA0:");
}

static void test_translation(void)
{
	printf("--- the connection manager's events in VMS console form ---\n");
	check_form("%CNXMAN, this node is now a VAXcluster member", "OVMXA", 5,
		   "%CNXMAN,  now a VAXcluster member -- system OVMXA",
		   "joined: now a VAXcluster member -- system <own SCSNODE>");
	check_form("%CNXMAN, sending VAXcluster membership request to system VAXC",
		   "OVMXB", 5,
		   "%CNXMAN,  sending VAXcluster membership request to system VAXC",
		   "membership request names the member it is sent to");
	check_form("%CNXMAN, the systems this node can see have quorum: "
		   "forming an OpenVMS Cluster", "OVMXA", 5,
		   "%CNXMAN,  proposing formation of a VAXcluster",
		   "founding: proposing formation of a VAXcluster");
	check_form("%CNXMAN, quorum lost, blocking activity", "OVMXA", 5,
		   "%CNXMAN,  quorum lost, blocking activity", "quorum lost");
	check_form("%CNXMAN, quorum regained, resuming activity", "OVMXA", 5,
		   "%CNXMAN,  quorum regained, resuming activity", "quorum regained");

	/* No name, no line (INV-6): never a placeholder on OPA0:. */
	check_form("%CNXMAN, this node is now a VAXcluster member", NULL, 0, NULL,
		   "joined, but no SCSNODE: no console line");
	check_form("%CNXMAN, this node is now a VAXcluster member", "", 0, NULL,
		   "joined, empty SCSNODE: no console line");
	check_form("%CNXMAN, sending VAXcluster membership request to the selected member",
		   "OVMXA", 5, NULL, "request to an unnamed member: no console line");
	check_form("%CNXMAN, sending VAXcluster membership request to system vaxc",
		   "OVMXA", 5, NULL, "a name VMS would not print: no console line");
	check_form("%CNXMAN, this node is now a VAXcluster member, or so it says",
		   "OVMXA", 5, NULL, "not the exact diagnostic: no console line");
	/* The diagnostics with no VMS console line for this role. */
	check_form("%CNXMAN, completed VAXcluster state transition", "OVMXA", 5, NULL,
		   "completed (role unknown here): no console line");
	check_form("%CNXMAN, system 00000000000007c4 was added to the cluster", "OVMXA",
		   5, NULL, "a member was added: VMS prints nothing on a non-coordinator");
	check_form("%CNXMAN, no coordinator CSID seen yet: this node's CSID was NOT learned",
		   "OVMXA", 5, NULL, "a diagnostic: no console line");

	{
		char tiny[8];
		ct_check(vms_opa0_cnxman_form("%CNXMAN, quorum lost, blocking activity",
					      "OVMXA", 5, tiny, sizeof tiny) == 0,
			 "a line that does not fit is not printed truncated");
	}
}

static void test_oracle(void)
{
	char *text[sizeof(oracle_files) / sizeof(oracle_files[0])];
	size_t len[sizeof(oracle_files) / sizeof(oracle_files[0])];
	unsigned int i, f;
	char what[256];

	printf("--- every console form is one a real VMS console printed ---\n");
	for (f = 0; f < sizeof(oracle_files) / sizeof(oracle_files[0]); f++) {
		text[f] = slurp(oracle_files[f], &len[f]);
		snprintf(what, sizeof what, "oracle capture readable: %s", oracle_files[f]);
		ct_check(text[f] != NULL, what);
	}
	for (i = 0; i < (unsigned int)VMS_OPA0_N_FORMS; i++) {
		const char *form = vms_opa0_form(i);
		int seen = 0;

		for (f = 0; f < sizeof(oracle_files) / sizeof(oracle_files[0]); f++)
			if (text[f] != NULL && contains(text[f], len[f], form))
				seen = 1;
		snprintf(what, sizeof what, "seen on a real VMS console: \"%s\"", form);
		ct_check(seen, what);
	}
	ct_check(vms_opa0_form(VMS_OPA0_N_FORMS) == NULL, "no form past the last");
	for (f = 0; f < sizeof(oracle_files) / sizeof(oracle_files[0]); f++)
		free(text[f]);
}

static void test_source(void)
{
	static const struct { const char *file, *text; } keyed[] = {
		{ "src/kernel-core/vms_cnxman_join_fsm.c",
		  "\"%CNXMAN, this node is now a VAXcluster member\"" },
		{ "src/kernel-core/vms_cnxman_coord_fsm.c",
		  "\"%CNXMAN, this node is now a VAXcluster member\"" },
		{ "src/kernel-core/vms_cnxman_join_fsm.c",
		  "\"%CNXMAN, sending VAXcluster membership \"\n\t\t\t\t\"request to \"" },
		{ "src/kernel-core/vms_cnxman_join_fsm.c", "\"system \"" },
		{ "src/kernel-core/vms_cnxman_coord_fsm.c",
		  "\"%CNXMAN, the systems this node can see have quorum: \"\n\t\t     \"forming an OpenVMS Cluster\"" },
		{ "src/kernel-core/vms_cnxman.c",
		  "\"%CNXMAN, quorum lost, blocking activity\"" },
		{ "src/kernel-core/vms_cnxman.c",
		  "\"%CNXMAN, quorum regained, resuming activity\"" },
	};
	unsigned int i;
	char what[256];

	printf("--- the diagnostics the translator keys on are the FSMs' own text ---\n");
	for (i = 0; i < sizeof(keyed) / sizeof(keyed[0]); i++) {
		size_t n = 0;
		char *src = slurp(keyed[i].file, &n);

		snprintf(what, sizeof what, "%s still composes %.70s", keyed[i].file,
			 keyed[i].text);
		ct_check(src != NULL && contains(src, n, keyed[i].text), one_line(what));
		free(src);
	}
}

int main(void)
{
	printf("=== test_cluster_opa0 (rd vms-c56) ===\n");
	test_routing();
	test_translation();
	test_oracle();
	test_source();
	return ct_summary("test_cluster_opa0");
}
