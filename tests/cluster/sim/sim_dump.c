/* SPDX-License-Identifier: GPL-2.0 */
/* sim_dump.c - the SDA-like snapshot. See sim_dump.h, including the flagged
 * gap about the port-level view. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "sim_dump.h"

/* ------------------------------------------------------------------ *
 * The sink
 * ------------------------------------------------------------------ */

void sim_dump_reset(struct sim_dump *d)
{
	d->len = 0u;
	d->overflow = 0;
	d->b[0] = '\0';
}

static void dumpf(struct sim_dump *d, const char *fmt, ...)
{
	va_list ap;
	int n;
	size_t room;

	if (d->overflow)
		return;
	room = SIM_DUMP_MAX - d->len;
	va_start(ap, fmt);
	n = vsnprintf(d->b + d->len, room, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= room) {
		d->overflow = 1;
		return;
	}
	d->len += (size_t)n;
}

void sim_dump_print(const struct sim_dump *d)
{
	fputs(d->b, stdout);
	if (d->overflow)
		printf("  [dump truncated: raise SIM_DUMP_MAX]\n");
}

/* ------------------------------------------------------------------ *
 * Field formatters
 * ------------------------------------------------------------------ */

static void mac_str(const uint8_t m[6], char out[18])
{
	snprintf(out, 18, "%02X-%02X-%02X-%02X-%02X-%02X",
		 m[0], m[1], m[2], m[3], m[4], m[5]);
}

/* A quadword the executive never learned prints blank, not zero (INV-6). */
static void quad_str(uint32_t hi, uint32_t lo, int valid, char out[20])
{
	if (!valid)
		snprintf(out, 20, "%18s", "(not learned)");
	else
		snprintf(out, 20, "%08X%08X", hi, lo);
}

/* ------------------------------------------------------------------ *
 * Channels -- through the SAME projection the diagnostics ioctl uses
 * ------------------------------------------------------------------ */

/*
 * The header and the row use the SAME width specifiers, applied to strings in
 * one and to values in the other, so the columns cannot drift apart. A
 * hand-counted header that is one space out makes every reader mis-attribute a
 * number to the column beside it -- which is exactly the mistake this readout
 * exists to prevent.
 */
#define SIM_CH_ROW_FMT "      %3s %-6s %-17s %9s %6s %11s %8s\n"

static void dump_channel_header(struct sim_dump *d)
{
	dumpf(d, "    CHANNELS\n");
	dumpf(d, SIM_CH_ROW_FMT, "idx", "state", "remote MAC", "rem.SYSID",
	      "pktsz", "hello rx/tx", "last rx");
}

static void dump_one_channel(struct sim_dump *d, uint32_t idx,
			     const struct pe_channel *ch)
{
	struct vms_pe_channel_view v;
	char mac[18], sysid[16], hello[24], num[3][16];

	pe_fsm_channel_project(ch, &v);
	mac_str(v.remote_mac, mac);
	if (v.remote_sysid_valid)
		snprintf(sysid, sizeof(sysid), "%u", v.remote_sysid_lo);
	else
		snprintf(sysid, sizeof(sysid), "-");
	snprintf(hello, sizeof(hello), "%u/%u", v.hello_rx, v.hello_tx);
	snprintf(num[0], sizeof(num[0]), "%u", idx);
	snprintf(num[1], sizeof(num[1]), "%u", v.verified_pktsz);
	snprintf(num[2], sizeof(num[2]), "%u", v.last_rx_ms);

	dumpf(d, SIM_CH_ROW_FMT, num[0],
	      pe_channel_state_name((enum vms_pe_channel_state)v.state), mac,
	      sysid, num[1], hello, num[2]);
}

static void dump_channels(struct sim_node *n, struct sim_dump *d)
{
	uint32_t i, shown = 0u;

	dump_channel_header(d);
	for (i = 0; i < PE_MAX_CHANNELS; i++) {
		struct pe_channel *ch = pe_fsm_channel_at(&n->fsm, i);

		if (ch == NULL)
			continue;
		dump_one_channel(d, i, ch);
		shown++;
	}
	if (shown == 0u)
		dumpf(d, "      (none)\n");
}

/* ------------------------------------------------------------------ *
 * Circuits -- same discipline
 * ------------------------------------------------------------------ */

/* Same discipline as the channel table: one format, two uses. */
#define SIM_VC_ROW_FMT \
	"      %3s %-10s %5s %4s %4s %4s %4s %5s %4s %5s %5s %5s  %s\n"

static void dump_vc_header(struct sim_dump *d)
{
	dumpf(d, "    VIRTUAL CIRCUITS\n");
	dumpf(d, SIM_VC_ROW_FMT, "idx", "state", "peer", "sseq", "rseq",
	      "rack", "pack", "unack", "retx", "opens", "downs", "cr/rc",
	      "incarnation");
}

static void dump_one_vc(struct sim_node *n, struct sim_dump *d, uint32_t idx,
			const struct pe_vc *vc)
{
	struct vms_pe_vc_view v;
	char inc[20], f[11][16];

	pe_fsm_vc_project(&n->fsm, vc, &v);
	snprintf(f[0], sizeof(f[0]), "%u", idx);
	if (vc->peer_sysid_valid)
		snprintf(f[1], sizeof(f[1]), "%u", v.peer_sysid_lo);
	else
		snprintf(f[1], sizeof(f[1]), "-");
	snprintf(f[2], sizeof(f[2]), "%u", v.send_seq);
	snprintf(f[3], sizeof(f[3]), "%u", v.recv_seq);
	snprintf(f[4], sizeof(f[4]), "%u", v.recv_ack);
	snprintf(f[5], sizeof(f[5]), "%u", v.peer_recv_ack);
	snprintf(f[6], sizeof(f[6]), "%u", v.unacked);
	snprintf(f[7], sizeof(f[7]), "%u", v.retransmits);
	snprintf(f[8], sizeof(f[8]), "%u", vc->opens);
	snprintf(f[9], sizeof(f[9]), "%u", vc->downs);
	snprintf(f[10], sizeof(f[10]), "%u/%u", v.credits_send,
		 v.credits_receive);
	quad_str(v.incarnation_hi, v.incarnation_lo, vc->peer_ident_valid, inc);

	dumpf(d, SIM_VC_ROW_FMT, f[0],
	      pe_vc_state_name((enum vms_pe_vc_state)v.state), f[1], f[2], f[3],
	      f[4], f[5], f[6], f[7], f[8], f[9], f[10], inc);
}

static void dump_vcs(struct sim_node *n, struct sim_dump *d)
{
	uint32_t i, shown = 0u;

	dump_vc_header(d);
	for (i = 0; i < SIM_MAX_NODES; i++) {
		struct pe_vc *vc = pe_fsm_vc_at(&n->fsm, i);

		if (vc == NULL)
			continue;
		dump_one_vc(n, d, i, vc);
		shown++;
	}
	if (shown == 0u)
		dumpf(d, "      (none)\n");
}

/* ------------------------------------------------------------------ *
 * The port line, and the honest-absence counters
 * ------------------------------------------------------------------ */

/*
 * Read straight off `struct pe_fsm`, because the frozen `vms_pe_view` is
 * filled by vms_pe.c's glue and this harness links only the pure FSM -- see
 * sim_dump.h's flagged gap. Every field named here is a public counter the FSM
 * increments from a real dispatch.
 */
static void dump_port(struct sim_node *n, struct sim_dump *d)
{
	const struct pe_fsm *f = &n->fsm;

	dumpf(d, "    PORT PEA0:  %s  link %s  channels %u  circuits %u\n",
	      f->running ? "running" : "stopped", f->link_up ? "up" : "down",
	      f->n_channels, f->n_vcs);
	dumpf(d, "      rx frames %u   not-SCA %u  unclassified %u  "
		 "not-for-us %u  parse-failed %u\n",
	      f->rx_frames, f->rx_not_sca, f->rx_unclassified, f->rx_not_for_us,
	      f->rx_parse_failed);
	dumpf(d, "      tx calls %u   tx errors %u  mcast HELLOs %u  "
		 "solicits %u  no-slot %u\n",
	      n->tx_calls, f->tx_errors, f->mcast_hello_tx, f->rx_solicit,
	      f->rx_no_slot);
	dumpf(d, "      ignored events ch %u / vc %u   vc no-incarnation %u  "
		 "no-identity %u\n",
	      f->ignored_events, f->vc_ignored_events, f->vc_no_incarnation,
	      f->vc_no_identity);
	dumpf(d, "      vc rx no-circuit %u  no-channel %u  parse-failed %u  "
		 "undelivered %u  reformations %u\n",
	      f->vc_rx_no_circuit, f->vc_rx_no_channel, f->vc_rx_parse_failed,
	      f->vc_rx_undelivered, f->vc_reformations);
	/* Design §5.3 is OPEN: this executive holds no cluster credential, so
	 * every directed frame went out without one and the ABSENCE is
	 * counted. Printing it is the point -- a zero here would mean a token
	 * was configured, never that one was invented. */
	dumpf(d, "      credential absent on %u directed frames  "
		 "(design 5.3 open)\n", f->nonce_absent);
	dumpf(d, "      upper: msgs %u  dgrams %u  vc-up %u  vc-down %u\n",
	      n->upper.messages, n->upper.datagrams, n->upper.ups,
	      n->upper.downs);
}

void sim_dump_node(struct sim_node *n, struct sim_dump *d)
{
	char mac[18];
	char lavc[18];

	mac_str(n->cfg.hw_mac, mac);
	mac_str(n->fsm.id.lavc, lavc);
	dumpf(d, "  NODE %-6s SCSSYSTEMID %-5u  HW %s  LAVC %s  %s\n",
	      n->cfg.name, n->cfg.sysid, mac, lavc,
	      n->booted ? "booted" : "halted");
	dumpf(d, "    booted at T+%llu ms  incarnation %016llX\n",
	      (unsigned long long)n->boot_ms,
	      (unsigned long long)n->incarnation);
	dump_port(n, d);
	dump_channels(n, d);
	dump_vcs(n, d);
}

/* ------------------------------------------------------------------ *
 * The LAN, and the whole cluster
 * ------------------------------------------------------------------ */

static void dump_lan(const struct sim *s, struct sim_dump *d)
{
	const struct sim_lan *l = &s->lan;

	dumpf(d, "  VIRTUAL LAN  ports %u\n", l->n_ports);
	dumpf(d, "    tx %llu  copies %llu  delivered %llu  lost %llu  "
		 "duped %llu  reordered %llu\n",
	      (unsigned long long)l->tx_frames, (unsigned long long)l->copies,
	      (unsigned long long)l->delivered, (unsigned long long)l->lost,
	      (unsigned long long)l->duped,
	      (unsigned long long)l->reordered);
	dumpf(d, "    cut-blocked %llu  link-down-blocked %llu  "
		 "undeliverable %llu  queue-full %llu  in flight %u\n",
	      (unsigned long long)l->cut_blocked,
	      (unsigned long long)l->link_down_blocked,
	      (unsigned long long)l->undeliverable,
	      (unsigned long long)l->queue_full, sim_lan_inflight(l));
}

static void dump_engine(const struct sim *s, struct sim_dump *d)
{
	dumpf(d, "  ENGINE  seed %llu  T+%llu ms  events %llu "
		 "(deliveries %llu, timers %llu)\n",
	      (unsigned long long)s->seed,
	      (unsigned long long)s->clock.now_ms,
	      (unsigned long long)s->events,
	      (unsigned long long)s->deliveries,
	      (unsigned long long)s->timer_fires);
	dumpf(d, "    timers armed %u (arms %u, moves %u, cancels %u, "
		 "fires %u, overflows %u)  trace %016llX\n",
	      sim_clock_armed(&s->clock), s->clock.arms, s->clock.moves,
	      s->clock.cancels, s->clock.fires, s->clock.overflows,
	      (unsigned long long)s->trace);
}

void sim_dump_cluster(struct sim *s, struct sim_dump *d, const char *title)
{
	uint32_t i;

	dumpf(d, "=== SIM SNAPSHOT: %s ===\n", title != NULL ? title : "");
	dump_engine(s, d);
	dump_lan(s, d);
	for (i = 0; i < s->n_nodes; i++)
		sim_dump_node(&s->node[i], d);
	dumpf(d, "=== end snapshot ===\n");
}
