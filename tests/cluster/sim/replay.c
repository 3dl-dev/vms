/* SPDX-License-Identifier: GPL-2.0 */
/* replay.c - the FC-P1.5 pcap replay driver. See replay.h for the contract
 * and why every check is structure-tolerant, never a byte-diff. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "replay.h"
#include "vms_cluster_codec_cm.h"

/* ------------------------------------------------------------------ *
 * A tiny append-to-buffer helper, so every check function can build its
 * report the same way sim_dump.c does, without pulling in that file's
 * bigger machinery.
 * ------------------------------------------------------------------ */
static void report_line(char *report, size_t cap, size_t *used,
			const char *fmt, ...)
{
	va_list ap;
	int n;
	size_t room;

	if (report == NULL || cap == 0u || *used >= cap)
		return;
	room = cap - *used;
	va_start(ap, fmt);
	n = vsnprintf(report + *used, room, fmt, ap);
	va_end(ap);
	if (n > 0)
		*used += (size_t)n < room ? (size_t)n : room - 1u;
}

/* ------------------------------------------------------------------ *
 * Driving: inject in order, drain the LAN after every injection
 * ------------------------------------------------------------------ */

/* Move every frame currently queued on `s->lan` into `out`, in scheduling
 * order (the same (due_ms, serial) order sim_lan_next/take give the real
 * event loop). With this replay's default zero-latency links, "queued" is
 * exactly "just transmitted" -- nothing here waits on the virtual clock. */
static int drain_lan(struct sim *s, struct vms_replay_result *out)
{
	uint32_t slot;

	while (sim_lan_next(&s->lan, &slot)) {
		const struct sim_inflight *e = sim_lan_peek(&s->lan, slot);

		if (e == NULL) {
			sim_lan_take(&s->lan, slot);
			continue;
		}
		if (out->n >= REPLAY_MAX_EMITTED) {
			sim_lan_take(&s->lan, slot);
			return -1;
		}
		out->frame[out->n].len = e->len;
		memcpy(out->frame[out->n].bytes, e->b, e->len);
		out->n++;
		sim_lan_take(&s->lan, slot);
	}
	return 0;
}

int vms_replay_drive(struct sim *s, struct sim_node *node,
		     const struct vms_replay_input *in, uint32_t n_in,
		     struct vms_replay_result *out)
{
	uint32_t i;

	if (s == NULL || node == NULL || out == NULL)
		return -1;
	memset(out, 0, sizeof(*out));

	for (i = 0; i < n_in; i++) {
		/* The SAME call sim_dispatch_delivery makes on a real event-loop
		 * delivery (sim_engine.c): "the CODEC decides what it is -- the
		 * harness never classifies a frame itself." A captured frame
		 * goes straight into the shipping FSM's receive path. */
		(void)pe_fsm_rx(&node->fsm, in[i].bytes, in[i].len);
		if (drain_lan(s, out) != 0)
			return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Assertion 1: SHAPE
 * ------------------------------------------------------------------ */

uint32_t vms_replay_check_shape(const struct vms_replay_result *r,
				char *report, size_t report_cap)
{
	size_t used = 0u;
	uint32_t i, bad = 0u;

	if (r == NULL)
		return 0u;
	for (i = 0; i < r->n; i++) {
		struct vms_frame_info fi;
		vms_codec_status_t st;

		st = vms_frame_classify(r->frame[i].bytes, r->frame[i].len,
					&fi);
		if (st == VMS_CODEC_E_NOTSCA || fi.cls == VMS_FCLS_UNKNOWN) {
			bad++;
			report_line(report, report_cap, &used,
				   "  shape FAIL frame %u: not a grounded "
				   "class (status %d)\n", i, (int)st);
			continue;
		}
		if (fi.len_check == (uint8_t)VMS_SCA_LEN_MISMATCH) {
			bad++;
			report_line(report, report_cap, &used,
				   "  shape FAIL frame %u: class %s, sec2 "
				   "length mismatch\n", i,
				   vms_frame_class_lookup(fi.cls)->name);
			continue;
		}
		report_line(report, report_cap, &used,
			   "  shape ok   frame %u: class %s (%u bytes)\n",
			   i, vms_frame_class_lookup(fi.cls)->name,
			   (unsigned)r->frame[i].len);
	}
	return bad;
}

/* ------------------------------------------------------------------ *
 * Assertion 2: ALLOWLIST
 * ------------------------------------------------------------------ */

/*
 * Look `category`/`opcode` up in `t` WITHOUT the sysap key -- see replay.h's
 * doc comment on why this driver checks "grounded in ANY table" rather than
 * the per-connection SYSAP resolution the executive itself will do. A
 * direct scan of the table's own rows, never a raw wire offset.
 */
static const struct vms_wire_allow_entry *
allow_find_any_sysap(const struct vms_wire_allow_table *t, uint8_t category,
		     uint8_t opcode)
{
	uint16_t i;

	if (t == NULL || t->rows == NULL)
		return NULL;
	for (i = 0; i < t->n; i++) {
		if (t->rows[i].category == category &&
		    t->rows[i].opcode == opcode)
			return &t->rows[i];
	}
	return NULL;
}

uint32_t vms_replay_check_allowlist(const struct vms_replay_result *r,
				    const struct vms_wire_allow_table *const *tables,
				    uint32_t n_tables, uint32_t *skipped,
				    char *report, size_t report_cap)
{
	size_t used = 0u;
	uint32_t i, t, bad = 0u, skip = 0u;

	if (r == NULL)
		return 0u;
	for (i = 0; i < r->n; i++) {
		struct vms_frame_info fi;
		struct vms_cm_envelope env;
		uint8_t req_category;
		const struct vms_wire_allow_entry *hit = NULL;
		vms_codec_status_t cst;

		cst = vms_frame_classify(r->frame[i].bytes, r->frame[i].len,
					 &fi);
		/* An unshaped frame (E_NOTSCA/UNKNOWN) is Assertion 1's
		 * failure to report; a class with no SYSAP envelope carries
		 * no allowlist obligation either way -- both are "skipped"
		 * here, never silently counted as passing. */
		if (cst != VMS_CODEC_OK || fi.cls != VMS_FCLS_SCS_MSG) {
			skip++;
			continue;
		}
		/* E73: the codec parses the SYSAP BODY, which is what SCS hands
		 * a SYSAP. The replay works from captured FRAMES, so it slices
		 * them exactly as the executive does. */
		if (vms_cm_envelope_parse(r->frame[i].bytes + VMS_OFF_SYSAP_BODY,
					  r->frame[i].len - VMS_OFF_SYSAP_BODY,
					  &env) != VMS_CODEC_OK) {
			bad++;
			report_line(report, report_cap, &used,
				   "  allow  FAIL frame %u: SCS_MSG class but "
				   "envelope did not parse\n", i);
			continue;
		}
		req_category = vms_wire_is_response(env.category) ?
			(uint8_t)(env.category & (uint8_t)~VMS_WIRE_RESPONSE_BIT) :
			env.category;
		for (t = 0; t < n_tables && hit == NULL; t++)
			hit = allow_find_any_sysap(tables[t], req_category,
						   env.opcode);
		if (hit == NULL) {
			bad++;
			report_line(report, report_cap, &used,
				   "  allow  FAIL frame %u: (cat 0x%02x, op "
				   "0x%02x) not in any GROUNDED table\n", i,
				   (unsigned)req_category, (unsigned)env.opcode);
			continue;
		}
		report_line(report, report_cap, &used,
			   "  allow  ok   frame %u: (cat 0x%02x, op 0x%02x) "
			   "-- %s\n", i, (unsigned)req_category,
			   (unsigned)env.opcode, hit->spec);
	}
	if (skipped != NULL)
		*skipped = skip;
	return bad;
}

/* ------------------------------------------------------------------ *
 * Assertion 3: SEQ CONTIGUITY
 * ------------------------------------------------------------------ */

int vms_replay_check_seq_contiguous(const struct vms_replay_result *r,
				    char *report, size_t report_cap)
{
	size_t used = 0u;
	uint32_t i;
	int have_prev = 0;
	uint16_t prev = 0u;

	if (r == NULL)
		return 1;
	for (i = 0; i < r->n; i++) {
		struct vms_frame_info fi;
		uint16_t recv_ack, send_seq;

		if (vms_frame_classify(r->frame[i].bytes, r->frame[i].len,
				       &fi) != VMS_CODEC_OK)
			continue;
		if (fi.family != (uint8_t)VMS_FFAM_SCS)
			continue;
		if (vms_scs_seq(r->frame[i].bytes, r->frame[i].len, &fi,
				&recv_ack, &send_seq) != VMS_CODEC_OK)
			continue;
		if (!have_prev) {
			have_prev = 1;
			prev = send_seq;
			report_line(report, report_cap, &used,
				   "  seq    first frame %u: send_seq=%u\n",
				   i, (unsigned)send_seq);
			continue;
		}
		if ((uint16_t)(prev + 1u) != send_seq) {
			report_line(report, report_cap, &used,
				   "  seq    FAIL frame %u: send_seq=%u, "
				   "want %u (prev %u)\n", i,
				   (unsigned)send_seq,
				   (unsigned)(prev + 1u), (unsigned)prev);
			return 0;
		}
		report_line(report, report_cap, &used,
			   "  seq    ok   frame %u: send_seq=%u\n", i,
			   (unsigned)send_seq);
		prev = send_seq;
	}
	return 1;
}
