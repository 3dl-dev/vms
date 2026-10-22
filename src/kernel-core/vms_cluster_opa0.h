/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_opa0.h - WHICH executive operator lines go on OPA0:, and the
 * VMS console form of the cluster events a VMS node announces there (rd
 * vms-c56).
 *
 * THE DEFECT THIS EXISTS FOR (measured 2026-10-21, live 3-node demo, V0.7-13).
 * Every line the executive's cluster stack composed -- ~200 distinct %CNXMAN,
 * %PEA0, %DLM and %MSCP_CL diagnostics ("no coordinator CSID seen yet",
 * "lock directory weight vector, by member", "refusing a lock message (cat
 * 0x02 op 0x01)") -- went to the console through exec_console_printf(), with
 * Linux printk uptime stamps, onto the Username: line and the DCL prompt. The
 * real OpenVMS VAX V7.3 in the same cluster printed three short lines.
 *
 * THE RULE. A line goes on OPA0: only when it is a console message VMS itself
 * prints, in the form VMS prints it. Everything else the executive says is a
 * DIAGNOSTIC and goes to the operator log (OPERATOR.LOG through the substrate's
 * kernel-log bridge: src/ovmx_init/opcom_kmsg.c on Linux, the /dev/klog reader
 * in src/ovmx_init/ovmx_boot_netbsd.c on NetBSD). Nothing is deleted; it is
 * re-routed. The decision is made HERE, once, for both substrates: each
 * substrate's exec_console_printf() asks vms_opa0_is_console_line() and sends
 * the line to the console or to the log accordingly. A new diagnostic needs
 * nothing from this file -- it lands in the log by default.
 *
 * THE ORACLE. Every console form below is copied from a real VMS console in
 * this repository: the real VAX's own console in the demo
 * (tests/lab/captures/vms-c56-console-chatter-20261021/C.console.txt) and the
 * VAX lab captures (tests/lab/captures/ * /VAXC.console.log, vax1.log, vax2.log,
 * armA-vax2-console.log ...). Note the TWO spaces after "%CNXMAN," -- that is
 * how VMS prints it, and it is what separates a console form from an OVMX
 * diagnostic, which carries one. Clean-room (Rule 8): observed console text,
 * nothing else.
 *
 * Pure: no includes, no library calls, plain char -- the same text compiles
 * into vms.ko, the NetBSD module and the host tests
 * (tests/cluster/host/test_cluster_opa0.c).
 */
#ifndef OVMX_VMS_CLUSTER_OPA0_H
#define OVMX_VMS_CLUSTER_OPA0_H

/* Longest console line this file ever composes or needs to classify. Every
 * oracle form is under 80 characters; a node name is at most 6. */
#define VMS_OPA0_LINE_MAX 128

/*
 * The console forms, as VMS prints them. A line whose text starts with one of
 * these prefixes is a console line; the "system <name>" ones carry the name
 * after the prefix. (A function, not a table, so a TU that includes this
 * header and never classifies anything carries no unused object.)
 */
enum vms_opa0_form {
	/* A joining node (armA-vax2-console.log 57-58, console-vax2.log 57-58). */
	VMS_OPA0_SENDING_REQUEST,
	VMS_OPA0_NOW_MEMBER,
	/* The founder / coordinator (C.console.txt 28-30, 76-77). */
	VMS_OPA0_PROPOSING_FORMATION,
	VMS_OPA0_COMPLETING,
	VMS_OPA0_RECEIVED_REQUEST,
	VMS_OPA0_PROPOSING_ADDITION,
	VMS_OPA0_PROPOSING_RECONFIG,
	VMS_OPA0_ABORTING,
	/* Losing and regaining a member (vms-ci6-evac-20261009/vax1.log, vax2.log). */
	VMS_OPA0_LOST_CONNECTION,
	VMS_OPA0_TIMED_OUT,
	VMS_OPA0_REMOVED,
	VMS_OPA0_RE_ESTABLISHED,
	VMS_OPA0_QUORUM_LOST,
	VMS_OPA0_QUORUM_REGAINED,
	VMS_OPA0_VC_CLOSED,
	VMS_OPA0_N_FORMS
};

static inline const char *vms_opa0_form(unsigned int f)
{
	switch (f) {
	case VMS_OPA0_SENDING_REQUEST:
		return "%CNXMAN,  sending VAXcluster membership request to system ";
	case VMS_OPA0_NOW_MEMBER:
		return "%CNXMAN,  now a VAXcluster member -- system ";
	case VMS_OPA0_PROPOSING_FORMATION:
		return "%CNXMAN,  proposing formation of a VAXcluster";
	case VMS_OPA0_COMPLETING:
		return "%CNXMAN,  completing VAXcluster state transition";
	case VMS_OPA0_RECEIVED_REQUEST:
		return "%CNXMAN,  received VAXcluster membership request from system ";
	case VMS_OPA0_PROPOSING_ADDITION:
		return "%CNXMAN,  proposing addition of system ";
	case VMS_OPA0_PROPOSING_RECONFIG:
		return "%CNXMAN,  proposing reconfiguration of the VAXcluster";
	case VMS_OPA0_ABORTING:
		return "%CNXMAN,  aborting VAXcluster state transition";
	case VMS_OPA0_LOST_CONNECTION:
		return "%CNXMAN,  lost connection to system ";
	case VMS_OPA0_TIMED_OUT:
		return "%CNXMAN,  timed-out lost connection to system ";
	case VMS_OPA0_REMOVED:
		return "%CNXMAN,  removed from VAXcluster system ";
	case VMS_OPA0_RE_ESTABLISHED:
		return "%CNXMAN,  re-established connection to system ";
	case VMS_OPA0_QUORUM_LOST:
		return "%CNXMAN,  quorum lost, blocking activity";
	case VMS_OPA0_QUORUM_REGAINED:
		return "%CNXMAN,  quorum regained, resuming activity";
	case VMS_OPA0_VC_CLOSED:
		return "%PEA0, Port has Closed Virtual Circuit - REMOTE NODE ";
	default:
		return 0;
	}
}

/* Does `s` begin with `prefix`? */
static inline int vms_opa0_starts(const char *s, const char *prefix)
{
	while (*prefix != '\0') {
		if (*s != *prefix)
			return 0;
		s++;
		prefix++;
	}
	return 1;
}

/*
 * Is this executive line one VMS prints on its console? A leading newline (the
 * VMS convention of starting a console message on a fresh line) is ignored.
 * 1 = OPA0:, 0 = the operator log.
 */
static inline int vms_opa0_is_console_line(const char *line)
{
	unsigned int i;

	if (line == 0)
		return 0;
	while (*line == '\r' || *line == '\n')
		line++;
	for (i = 0; i < (unsigned int)VMS_OPA0_N_FORMS; i++)
		if (vms_opa0_starts(line, vms_opa0_form(i)))
			return 1;
	return 0;
}

/* Is `s` exactly `t`? */
static inline int vms_opa0_eq(const char *s, const char *t)
{
	return vms_opa0_starts(s, t) && vms_opa0_starts(t, s);
}

/* Append `s` to out[*at], bounded; returns 0 if it did not fit. */
static inline int vms_opa0_put(char *out, unsigned int outsz, unsigned int *at,
			       const char *s, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n && s[i] != '\0'; i++) {
		if (*at + 1u >= outsz)
			return 0;
		out[(*at)++] = s[i];
	}
	out[*at] = '\0';
	return 1;
}

/* A node name as VMS prints it: 1..6 characters, upper case letters, digits,
 * '$' and '_'. Anything else is not a name this file will print. */
static inline unsigned int vms_opa0_name_len(const char *s, unsigned int max)
{
	unsigned int n = 0;

	while (n < max && s[n] != '\0' &&
	       ((s[n] >= 'A' && s[n] <= 'Z') || (s[n] >= '0' && s[n] <= '9') ||
		s[n] == '$' || s[n] == '_'))
		n++;
	return (n >= 1u && n <= 6u) ? n : 0u;
}

/*
 * THE CONNECTION MANAGER'S CONSOLE LINES. Given one line the connection
 * manager logged (vms_cnxman.c cnxman_ops_log), compose the console line VMS
 * prints for the SAME event into `out`, and return 1; return 0 when VMS prints
 * nothing on its console for that event (the line is a diagnostic only).
 *
 * `self`/`self_len` is this node's SCSNODE, read from the executive's own
 * cluster parameters -- never a placeholder: with no SCSNODE there is no name
 * to print, and no console line (INV-6).
 *
 * Only events whose console form the oracle shows, and whose OVMX diagnostic
 * names the same act, are mapped:
 *
 *   OVMX diagnostic                                VMS console
 *   ---------------------------------------------  -------------------------------
 *   sending VAXcluster membership request to       sending VAXcluster membership
 *     system <NODE>                                  request to system <NODE>
 *   this node is now a VAXcluster member           now a VAXcluster member --
 *                                                    system <SCSNODE>
 *   the systems this node can see have quorum:     proposing formation of a
 *     forming an OpenVMS Cluster                     VAXcluster
 *   quorum lost, blocking activity                 quorum lost, blocking activity
 *   quorum regained, resuming activity             quorum regained, resuming activity
 *
 * The coordinator-side and loss events (received/proposing addition of system
 * <NODE>, completing, lost connection to system <NODE>, removed from
 * VAXcluster) need the subject node's name and the coordinator role at the
 * event site; the diagnostics this table sees do not carry them, so those
 * events print nothing on OPA0: rather than a guess (rd vms-c56 follow-up).
 */
static inline int vms_opa0_cnxman_form(const char *diag, const char *self,
				       unsigned int self_len, char *out,
				       unsigned int outsz)
{
	static const char req_diag[] =
		"%CNXMAN, sending VAXcluster membership request to system ";
	unsigned int at = 0, n, f;

	if (diag == 0 || out == 0 || outsz == 0u)
		return 0;
	out[0] = '\0';

	if (vms_opa0_starts(diag, req_diag)) {
		const char *name = diag + sizeof(req_diag) - 1u;

		n = vms_opa0_name_len(name, 7u);
		if (n == 0u || name[n] != '\0')
			return 0;
		return vms_opa0_put(out, outsz, &at,
				    vms_opa0_form(VMS_OPA0_SENDING_REQUEST),
				    VMS_OPA0_LINE_MAX) &&
		       vms_opa0_put(out, outsz, &at, name, n);
	}
	if (vms_opa0_eq(diag, "%CNXMAN, this node is now a VAXcluster member")) {
		if (self == 0)
			return 0;
		n = vms_opa0_name_len(self, self_len);
		if (n == 0u || n != self_len)
			return 0;
		return vms_opa0_put(out, outsz, &at,
				    vms_opa0_form(VMS_OPA0_NOW_MEMBER),
				    VMS_OPA0_LINE_MAX) &&
		       vms_opa0_put(out, outsz, &at, self, n);
	}
	if (vms_opa0_eq(diag, "%CNXMAN, the systems this node can see have "
			      "quorum: forming an OpenVMS Cluster"))
		f = VMS_OPA0_PROPOSING_FORMATION;
	else if (vms_opa0_eq(diag, "%CNXMAN, quorum lost, blocking activity"))
		f = VMS_OPA0_QUORUM_LOST;
	else if (vms_opa0_eq(diag, "%CNXMAN, quorum regained, resuming activity"))
		f = VMS_OPA0_QUORUM_REGAINED;
	else
		return 0;
	return vms_opa0_put(out, outsz, &at, vms_opa0_form(f), VMS_OPA0_LINE_MAX);
}

#endif /* OVMX_VMS_CLUSTER_OPA0_H */
