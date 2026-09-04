/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_diag.c - the JOIN's transition ring (E69, test-ladder rung R1).
 *
 * WHAT THIS PROVES, AND WHY EACH GROUP EXISTS. The ring is an INSTRUMENT, and
 * an instrument has exactly three ways to be worthless:
 *
 *   1. IT LIES ABOUT HISTORY -- records out of order, a wrap that silently
 *      drops the newest instead of the oldest, or a read past the end that
 *      returns a zeroed record a reader would take for a real transition.
 *      Group A drives all of that on the real ring.
 *
 *   2. IT RENDERS THE WRONG NAME. The record carries ORDINALS across
 *      /dev/vms and the userland dumper (tools/vms_cnxtrace.c) carries its own
 *      copy of the name tables, because it cannot link kernel-core. Group B
 *      compares the two copies ORDINAL BY ORDINAL for all seven vocabularies,
 *      so a name added on one side and not the other is a red test here rather
 *      than a "?" on a lab console at 2 a.m.
 *
 *   3. IT CHANGES WHAT IT OBSERVES. This is the one that would be a real bug
 *      in the product rather than in the tooling, and it is the reason group D
 *      exists: the SAME join scenario is driven twice, once with a ring and
 *      once without, and the FSM's whole context is compared BYTE FOR BYTE.
 *      Observability that perturbs the join would be worse than no
 *      observability at all.
 *
 * Group C is the positive case: a real join drive, instrumented, and the
 * records it must produce -- the transition that fired, the empty table cell
 * that did not, and the emit gate that says WHY a message did not go out,
 * which is the question E69 was raised to answer.
 *
 * Everything is injected: the clock, the SCS client surface, the ring itself.
 * No wire, no daemon, no boot.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cnxman_diag.h"
#include "vms_cluster_codec_cm.h"
#include "vms_pe_fsm.h"   /* E70: enum pe_vc_send_status + the port's readback,
			   * the vocabulary group B2 maps into this ring's  */

/* The userland dumper's OWN copy of the name tables -- the thing group B is
 * here to keep honest. A header of static tables, so including it costs no
 * link edge. */
#include "cnxtrace_names.h"

/* ==========================================================================
 * A. The ring itself
 * ========================================================================== */

static struct cnxman_diag_ring g_ring;

/* A distinguishable dispatch record: `aux` carries the index so every
 * assertion below can name WHICH record it is looking at. */
static void fill_dispatch(struct cnxman_diag_ring *r, uint32_t i)
{
	cnxman_diag_dispatch(r, 1000u + i, (uint8_t)CNXMAN_JOIN_DIR_ROUND,
			     (uint8_t)CNXMAN_JOIN_ADVERTISE,
			     (uint8_t)CNXMAN_EV_CDT_OPEN, 1,
			     (uint8_t)CNXMAN_JOIN_RX_CONSUMED, i);
}

static void test_empty_ring(void)
{
	struct cnxman_diag_rec rec;

	printf("\n-- an empty ring reports nothing, and refuses a read --\n");
	cnxman_diag_init(&g_ring, 1);

	ct_check_eq_u32(cnxman_diag_count(&g_ring), 0u, "count is 0");
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 0u, "recorded is 0");

	memset(&rec, 0xee, sizeof(rec));
	ct_check(cnxman_diag_get(&g_ring, 0, &rec) == 0,
		 "a read of record 0 is REFUSED, not a zeroed record");
	ct_check(rec.seq == 0xeeeeeeeeu,
		 "... and the caller's buffer is untouched");
}

static void test_records_in_order(void)
{
	struct cnxman_diag_rec rec;
	uint32_t i;
	int ordered = 1, seq_ok = 1;

	printf("\n-- N records read back OLDEST FIRST, with monotonic seq --\n");
	cnxman_diag_init(&g_ring, 1);
	for (i = 0; i < 5u; i++)
		fill_dispatch(&g_ring, i);

	ct_check_eq_u32(cnxman_diag_count(&g_ring), 5u, "count is 5");
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 5u, "recorded is 5");

	for (i = 0; i < 5u; i++) {
		if (!cnxman_diag_get(&g_ring, i, &rec)) {
			ordered = 0;
			break;
		}
		if (rec.aux != i)
			ordered = 0;
		if (rec.seq != i)
			seq_ok = 0;
	}
	ct_check(ordered, "record i is the i'th one recorded");
	ct_check(seq_ok, "seq is 0,1,2,... -- assigned at record time");
	ct_check(cnxman_diag_get(&g_ring, 5u, &rec) == 0,
		 "record 5 is past the end and is REFUSED");
}

static void test_wrap_drops_the_oldest(void)
{
	struct cnxman_diag_rec first, last;
	uint32_t i, extra = 10u;

	printf("\n-- the wrap drops the OLDEST, and says how many --\n");
	cnxman_diag_init(&g_ring, 1);
	for (i = 0; i < CNXMAN_DIAG_SLOTS + extra; i++)
		fill_dispatch(&g_ring, i);

	ct_check_eq_u32(cnxman_diag_count(&g_ring), CNXMAN_DIAG_SLOTS,
			"count saturates at the ring size");
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring),
			CNXMAN_DIAG_SLOTS + extra,
			"recorded keeps counting past the ring size");
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring) -
				cnxman_diag_count(&g_ring),
			extra, "recorded - count IS the number dropped");

	ct_check(cnxman_diag_get(&g_ring, 0, &first), "record 0 readable");
	ct_check_eq_u32(first.aux, extra,
			"the oldest HELD record is the first survivor");
	ct_check_eq_u32(first.seq, extra,
			"... and its seq shows the gap the wrap left");

	ct_check(cnxman_diag_get(&g_ring, CNXMAN_DIAG_SLOTS - 1u, &last),
		 "the last held record is readable");
	ct_check_eq_u32(last.aux, CNXMAN_DIAG_SLOTS + extra - 1u,
			"the newest record survived the wrap");
}

static void test_disabled_and_absent(void)
{
	printf("\n-- a disabled ring, and no ring at all, record nothing --\n");
	cnxman_diag_init(&g_ring, 0);
	fill_dispatch(&g_ring, 1u);
	cnxman_diag_arrival(&g_ring, 1u, 0u, 0u, 0u, 0, 0u);
	cnxman_diag_emit(&g_ring, 1u, 0u, 0u, 0u, 0u, 0, 0u);
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 0u,
			"a disabled ring records nothing");

	cnxman_diag_enable(&g_ring, 1);
	fill_dispatch(&g_ring, 2u);
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 1u,
			"re-enabling resumes recording");

	/* A NULL ring is the normal wiring for a build with no diagnostics.
	 * These must be safe no-ops, not a crash on a booted node. */
	fill_dispatch(NULL, 0u);
	cnxman_diag_arrival(NULL, 0u, 0u, 0u, 0u, 0, 0u);
	cnxman_diag_emit(NULL, 0u, 0u, 0u, 0u, 0u, 0, 0u);
	cnxman_diag_init(NULL, 1);
	cnxman_diag_enable(NULL, 1);
	ct_check_eq_u32(cnxman_diag_count(NULL), 0u,
			"a NULL ring holds nothing and does not fault");
	ct_check_eq_u32(cnxman_diag_recorded(NULL), 0u,
			"... and reports nothing recorded");
}

/* ==========================================================================
 * A2. COALESCING -- the property that keeps the join drive in the ring
 * ========================================================================== */

/* The exact shape the join watchdog produces: the SAME [state][event] with the
 * same outcome, once a second, forever (CNXMAN_JOIN_WATCH_MS). */
static void watchdog_tick(struct cnxman_diag_ring *r, uint32_t t_ms)
{
	cnxman_diag_dispatch(r, t_ms, (uint8_t)CNXMAN_JOIN_ADMIT,
			     (uint8_t)CNXMAN_JOIN_ADMIT,
			     (uint8_t)CNXMAN_EV_TIMER_JOIN, 1,
			     (uint8_t)CNXMAN_JOIN_RX_CONSUMED, 0u);
}

static void test_identical_facts_coalesce(void)
{
	struct cnxman_diag_rec rec;
	uint32_t i;

	printf("\n-- an identical fact repeated back to back takes ONE slot --\n");
	cnxman_diag_init(&g_ring, 1);
	for (i = 0; i < 600u; i++)
		watchdog_tick(&g_ring, 1000u + i);

	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 1u,
			"600 identical watchdog ticks took ONE slot");
	ct_check(cnxman_diag_get(&g_ring, 0, &rec), "the record is readable");
	ct_check_eq_u32(rec.repeat, 599u,
			"repeat is the number of ADDITIONAL occurrences");
	ct_check_eq_u32(rec.t_ms, 1000u, "t_ms is the FIRST occurrence");
	ct_check_eq_u32(rec.t_last_ms, 1599u, "t_last_ms is the LAST");
	ct_check_eq_u32(rec.seq, 0u, "and it kept its own sequence number");
}

static void test_a_different_fact_breaks_the_run(void)
{
	struct cnxman_diag_rec a, b, c;

	printf("\n-- one different fact ENDS the run: nothing is hidden --\n");
	cnxman_diag_init(&g_ring, 1);
	watchdog_tick(&g_ring, 1000u);
	watchdog_tick(&g_ring, 2000u);
	/* something real happens between two ticks */
	cnxman_diag_emit(&g_ring, 2500u, (uint8_t)CNXMAN_JOIN_ADMIT,
			 VMS_CM_CAT_CONFIG, VMS_CM_OP_PARAMS,
			 (uint8_t)CNXMAN_DIAG_G_SENT, 0, 7u);
	watchdog_tick(&g_ring, 3000u);

	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 3u,
			"three slots: the run, the event, a NEW run");
	ct_check(cnxman_diag_get(&g_ring, 0, &a) &&
		 cnxman_diag_get(&g_ring, 1, &b) &&
		 cnxman_diag_get(&g_ring, 2, &c), "all three are readable");
	ct_check_eq_u32(a.repeat, 1u, "the first run held two ticks");
	ct_check_eq_u32(b.kind, (uint32_t)CNXMAN_DIAG_K_EMIT,
			"the event that broke it is recorded in full");
	ct_check_eq_u32(c.repeat, 0u,
			"the tick AFTER it started a fresh record");
	ct_check_eq_u32(c.t_ms, 3000u, "... at its own time");
}

/*
 * THE REASON COALESCING EXISTS, driven end to end. A join that is waiting
 * offers one identical watchdog dispatch a second and the lab dumps the ring
 * ~600 s into a run; on a plain wrap-around ring the drive -- the only records
 * that carry the answer -- would be long gone.
 */
static void test_the_drive_survives_a_long_wait(void)
{
	struct cnxman_diag_rec rec;
	uint32_t i;

	printf("\n-- the join drive SURVIVES a 600-second watchdog wait --\n");
	cnxman_diag_init(&g_ring, 1);

	/* a drive: a handful of distinct facts */
	for (i = 0; i < 8u; i++)
		fill_dispatch(&g_ring, i);
	/* then ten minutes of waiting, at the real watchdog's real rate */
	for (i = 0; i < 600u; i++)
		watchdog_tick(&g_ring, 100000u + i * 1000u);

	ct_check_eq_u32(cnxman_diag_recorded(&g_ring), 9u,
			"600 s of waiting cost ONE slot, not 600");
	ct_check_eq_u32(cnxman_diag_recorded(&g_ring) -
				cnxman_diag_count(&g_ring), 0u,
			"nothing was dropped");
	ct_check(cnxman_diag_get(&g_ring, 0, &rec),
		 "the FIRST record of the drive is still held");
	ct_check_eq_u32(rec.aux, 0u, "... and it is really the first one");
	ct_check(cnxman_diag_get(&g_ring, 8, &rec), "the wait is held too");
	ct_check_eq_u32(rec.repeat, 599u, "... as one record with its count");
}

static void test_every_field_round_trips(void)
{
	struct cnxman_diag_rec rec;

	printf("\n-- every field of every kind survives the round trip --\n");
	cnxman_diag_init(&g_ring, 1);

	cnxman_diag_dispatch(&g_ring, 0x11223344u,
			     (uint8_t)CNXMAN_JOIN_ADMIT,
			     (uint8_t)CNXMAN_JOIN_BARRIER,
			     (uint8_t)CNXMAN_EV_RX_TR_GO, 1,
			     (uint8_t)CNXMAN_JOIN_RX_HANDOFF, 0xdeadbeefu);
	ct_check(cnxman_diag_get(&g_ring, 0, &rec), "the dispatch is readable");
	ct_check_eq_u32(rec.kind, (uint32_t)CNXMAN_DIAG_K_DISPATCH, "kind");
	ct_check_eq_u32(rec.t_ms, 0x11223344u, "t_ms is the caller's clock");
	ct_check_eq_u32(rec.state, (uint32_t)CNXMAN_JOIN_ADMIT, "state before");
	ct_check_eq_u32(rec.new_state, (uint32_t)CNXMAN_JOIN_BARRIER,
			"state after");
	ct_check_eq_u32(rec.event, (uint32_t)CNXMAN_EV_RX_TR_GO, "event");
	ct_check_eq_u32(rec.detail, 1u, "detail 1 = a table cell fired");
	ct_check_eq_u32(rec.rx, (uint32_t)CNXMAN_JOIN_RX_HANDOFF, "rx");
	ct_check_eq_u32(rec.aux, 0xdeadbeefu, "aux");
	ct_check_eq_u32(rec.repeat, 0u, "repeat 0 = it happened exactly once");
	ct_check_eq_u32(rec.t_last_ms, 0x11223344u,
			"t_last_ms == t_ms when it happened once");

	cnxman_diag_arrival(&g_ring, 7u, (uint8_t)CNXMAN_JOIN_VC_CONNECT,
			    CNXMAN_DIAG_EV_NONE,
			    (uint8_t)CNXMAN_DIAG_R_CM_REJ, -1234, 0x99u);
	ct_check(cnxman_diag_get(&g_ring, 1, &rec), "the arrival is readable");
	ct_check_eq_u32(rec.kind, (uint32_t)CNXMAN_DIAG_K_ARRIVAL, "kind");
	ct_check_eq_u32(rec.detail, (uint32_t)CNXMAN_DIAG_R_CM_REJ, "reason");
	ct_check(rec.rc == -1234, "rc carries the op's OWN signed return");
	ct_check_eq_u32(rec.new_state, (uint32_t)CNXMAN_JOIN_VC_CONNECT,
			"no handler ran, so new_state == state");

	cnxman_diag_emit(&g_ring, 8u, (uint8_t)CNXMAN_JOIN_ADVERTISE,
			 VMS_CM_CAT_CONFIG, VMS_CM_OP_MODEL,
			 (uint8_t)CNXMAN_DIAG_G_NO_CONN, 0, 0x4e620009u);
	ct_check(cnxman_diag_get(&g_ring, 2, &rec), "the emit is readable");
	ct_check_eq_u32(rec.kind, (uint32_t)CNXMAN_DIAG_K_EMIT, "kind");
	ct_check_eq_u32(rec.cat, VMS_CM_CAT_CONFIG, "category");
	ct_check_eq_u32(rec.op, VMS_CM_OP_MODEL, "opcode");
	ct_check_eq_u32(rec.detail, (uint32_t)CNXMAN_DIAG_G_NO_CONN, "gate");
}

/* ==========================================================================
 * B. The two copies of every name table agree
 *
 * The executive renders ordinals for its own %CNXMAN lines; the userland
 * dumper renders the SAME ordinals off its own tables. Nothing links the two,
 * so nothing but this test stops them drifting -- and a drifted table produces
 * a transcript that reads plausibly and names the wrong transition, which is
 * strictly worse than no transcript.
 * ========================================================================== */

static void check_table(const char *what, const char *const *tab, unsigned n,
			const char *(*exec_name)(uint8_t), unsigned count)
{
	unsigned i;
	int same = 1;

	ct_check_eq_u32(n, count, what);
	for (i = 0; i < count && i < n; i++) {
		if (strcmp(tab[i], exec_name((uint8_t)i)) != 0) {
			printf("       ordinal %u: dumper \"%s\" != executive "
			       "\"%s\"\n", i, tab[i], exec_name((uint8_t)i));
			same = 0;
		}
	}
	ct_check(same, "... and every ordinal renders the SAME name");
}

/* ==========================================================================
 * B2. THE PORT'S REFUSAL, NAMED (E70)
 *
 * THE LAST AMBIGUITY THIS CLOSES. Everything above the port is told an SS$_
 * status, and that map is many-to-one on purpose (Rule 8 forbids inventing the
 * statuses OpenVMS uses): the port's NOCIRCUIT and RINGFULL are BOTH
 * SS$_DEVOFFLINE. On a live cluster those are different defects, so the ring
 * records the port's own cause as a NAMED reason with the ONE live number
 * behind it. This drives that mapping directly -- it is a pure function in the
 * ring's own TU precisely so it does not need a booted node to test.
 * ========================================================================== */
static void check_port_reason(int32_t code, enum cnxman_diag_reason want,
			      const char *why)
{
	ct_check_eq_u32((uint32_t)cnxman_diag_port_reason(code), (uint32_t)want,
			why);
}

static void test_port_refusal_is_named_not_collapsed(void)
{
	struct pe_vc_send_refusal p;

	printf("\n-- E70: the PORT's own refusal, named rather than collapsed "
	       "--\n");

	check_port_reason(PE_VC_SEND_NOCIRCUIT, CNXMAN_DIAG_R_PORT_NOCIRCUIT,
			  "no circuit is its own reason");
	check_port_reason(PE_VC_SEND_RINGFULL, CNXMAN_DIAG_R_PORT_RINGFULL,
			  "... and a full unacked ring is a DIFFERENT one, "
			  "though both are SS$_DEVOFFLINE above the port");
	check_port_reason(PE_VC_SEND_NOCREDIT, CNXMAN_DIAG_R_PORT_NOCREDIT,
			  "a spent port send-window is a third");
	check_port_reason(PE_VC_SEND_BADFRAME, CNXMAN_DIAG_R_PORT_BADFRAME,
			  "an unsendable frame is a fourth");
	check_port_reason(PE_VC_SEND_TOOBIG, CNXMAN_DIAG_R_PORT_BADFRAME,
			  "... which an oversized one shares, with `rc` still "
			  "carrying WHICH of the two it was");
	check_port_reason(PE_VC_SEND_TXFAIL, CNXMAN_DIAG_R_PORT_TXFAIL,
			  "and the interface refusing is a fifth");
	check_port_reason(-99, CNXMAN_DIAG_R_PORT_TXFAIL,
			  "a code this vocabulary has not grown still gets a "
			  "REAL record, never a neighbour's name");

	ct_check(cnxman_diag_port_reason(PE_VC_SEND_NOCIRCUIT) !=
		 cnxman_diag_port_reason(PE_VC_SEND_RINGFULL),
		 "THE POINT: the two causes that share one SS$_ status do NOT "
		 "share a reason");

	/* The ONE live number beside each name, read out of the port's own
	 * readback and never derived. */
	memset(&p, 0, sizeof(p));
	p.code = PE_VC_SEND_NOCREDIT;
	p.vc_present = 1u;
	p.send_refused_credit = 7u;
	p.send_refused_ring = 3u;
	ct_check_eq_u32(cnxman_diag_port_aux(&p), 7u,
			"a credit refusal carries the CREDIT counter");
	p.code = PE_VC_SEND_RINGFULL;
	ct_check_eq_u32(cnxman_diag_port_aux(&p), 3u,
			"a ring refusal carries the RING counter -- the two "
			"never stand in for each other");
	p.code = PE_VC_SEND_NOCIRCUIT;
	p.vc_state = (uint8_t)VMS_PE_VC_START_SENT;
	ct_check_eq_u32(cnxman_diag_port_aux(&p),
			(uint32_t)VMS_PE_VC_START_SENT,
			"a no-circuit refusal carries the circuit's LIVE state");
	p.vc_present = 0u;
	ct_check_eq_u32(cnxman_diag_port_aux(&p), CNXMAN_DIAG_NO_VC,
			"... or the sentinel that says there is no circuit "
			"object at all, which no state can be mistaken for");
	p.code = PE_VC_SEND_TXFAIL;
	p.vc_present = 1u;
	ct_check_eq_u32(cnxman_diag_port_aux(&p), 0u,
			"a cause the port keeps no count for reports an "
			"explicit 0, never an invented number");
	ct_check_eq_u32(cnxman_diag_port_aux(NULL), 0u,
			"and a NULL readback answers 0 rather than reading it");

	/* Every new reason RENDERS -- in the executive and in the dumper. */
	ct_check(strcmp(cnxman_diag_reason_name(CNXMAN_DIAG_R_PORT_NOCREDIT),
			"port-nocredit") == 0,
		 "the executive renders the named cause as a WORD, so "
		 "`grep CNXTRACE` needs no decoder ring");
	ct_check(strcmp(cnxtrace_name(cnxtrace_reason_names,
				      CNXTRACE_N(cnxtrace_reason_names),
				      (unsigned char)
				      CNXMAN_DIAG_R_CDT_NOT_SENDABLE),
			"cdt-not-sendable") == 0,
		 "... and the dumper renders the same ordinal identically");

	/* E71: the refused CONNECT, which had no record at all before. */
	ct_check(strcmp(cnxman_diag_reason_name(CNXMAN_DIAG_R_CONNECT_REFUSED),
			"connect-refused") == 0,
		 "E71: a refused connect renders as its own word");
	ct_check(strcmp(cnxtrace_name(cnxtrace_reason_names,
				      CNXTRACE_N(cnxtrace_reason_names),
				      (unsigned char)
				      CNXMAN_DIAG_R_CONNECT_REFUSED),
			"connect-refused") == 0,
		 "... in the dumper too, at the same ordinal");
	ct_check(strcmp(cnxman_join_failure_name(CNXMAN_JOIN_FAIL_TIMEOUT),
			"reconnect interval expired") == 0,
		 "E71: and the honest end of a wait has a name of its own, "
		 "distinct from every refusal");
}

/* The two join vocabularies are rendered by the FSM's own accessors, whose
 * prototypes take the enum rather than a uint8_t -- one thunk each, so
 * check_table() stays one function. */
static const char *exec_state_name(uint8_t s)
{
	return cnxman_join_state_name((enum cnxman_join_state)s);
}

static const char *exec_failure_name(uint8_t f)
{
	return cnxman_join_failure_name((enum cnxman_join_failure)f);
}

static void test_name_tables_agree(void)
{
	printf("\n-- the dumper's name tables match the executive's --\n");

	check_table("state table has one entry per join state",
		    cnxtrace_state_names, CNXTRACE_N(cnxtrace_state_names),
		    exec_state_name, (unsigned)CNXMAN_JOIN_STATE__COUNT);
	check_table("failure table has one entry per failure",
		    cnxtrace_failure_names, CNXTRACE_N(cnxtrace_failure_names),
		    exec_failure_name,
		    (unsigned)CNXMAN_JOIN_FAIL__COUNT);
	check_table("kind table has one entry per record kind",
		    cnxtrace_kind_names, CNXTRACE_N(cnxtrace_kind_names),
		    cnxman_diag_kind_name, (unsigned)CNXMAN_DIAG_K__COUNT);
	check_table("reason table has one entry per arrival reason",
		    cnxtrace_reason_names, CNXTRACE_N(cnxtrace_reason_names),
		    cnxman_diag_reason_name, (unsigned)CNXMAN_DIAG_R__COUNT);
	check_table("gate table has one entry per emit gate",
		    cnxtrace_gate_names, CNXTRACE_N(cnxtrace_gate_names),
		    cnxman_diag_gate_name, (unsigned)CNXMAN_DIAG_G__COUNT);
	check_table("rx table has one entry per join rx outcome",
		    cnxtrace_rx_names, CNXTRACE_N(cnxtrace_rx_names),
		    cnxman_diag_rx_name, (unsigned)CNXMAN_JOIN_RX_BAD + 1u);
	check_table("event table has one entry per shared cnxman event",
		    cnxtrace_event_names, CNXTRACE_N(cnxtrace_event_names),
		    cnxman_diag_event_name, (unsigned)CNXMAN_EV__COUNT);

	/* The one value that is deliberately NOT an event ordinal. */
	ct_check(strcmp(cnxman_diag_event_name(CNXMAN_DIAG_EV_NONE), "-") == 0,
		 "the executive renders \"no event\" as \"-\"");
	/* The dumper renders CNXMAN_DIAG_EV_NONE as "-" too, through its own
	 * special case (cnxtrace_event()); its BOUNDED table lookup is what
	 * refuses an ordinal the executive grew and the image has not. */
	ct_check(strcmp(cnxtrace_name(cnxtrace_event_names,
				      CNXTRACE_N(cnxtrace_event_names),
				      (unsigned char)CNXMAN_EV__COUNT),
			"?") == 0,
		 "the dumper's bounded lookup refuses to index past its table");
	ct_check(strcmp(cnxman_diag_event_name((uint8_t)CNXMAN_EV__COUNT),
			"?") == 0,
		 "... and the executive refuses the same unknown ordinal");
}

/* ==========================================================================
 * C + D. A real join drive, instrumented
 *
 * The smallest bed that reaches an origination: two members in the CLUB, both
 * directory answers delivered, MSCP$DISK absent on the target (a real
 * configuration -- MSCP_LOAD 0), so the drive goes straight to the
 * VMS$VAXcluster connect and the MODEL/PARAMS burst.
 * ========================================================================== */

#define MEMBER_SYSID 0x000004000101ull
#define OTHER_SYSID  0x000004000102ull
#define OWN_SYSID    0x000004000103ull
#define CM_CONID     0x4e620009u

struct jbed {
	struct vms_cluster     cl;
	struct cnxman_ops      ops;
	struct fake_cnx        fake;
	struct cnxman_join_ops jops;
	struct cnxman_join     j;
	struct cnxman_diag_ring ring;
	int                    fail_send;
	int                    fail_send_rc;   /* the refusal, verbatim (E70) */
};

static struct jbed g_b;

static int jb_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx; (void)dst; (void)name;
	return 0;
}

static int jb_connect(void *ctx, vms_scs_sysid_t dst,
		      const uint8_t *local_name, const uint8_t *remote_name,
		      const uint8_t *conndata, uint16_t credits,
		      vms_conid_t *out_conid)
{
	(void)ctx; (void)local_name; (void)remote_name;
	(void)conndata; (void)credits;
	*out_conid = CM_CONID;
	/* Mirror the production glue (cnxman_jop_connect): the Con.ID SCS just
	 * minted goes into the destination's CSB at that instant, which is what
	 * binds that block's dialogue counters to THIS connection (E77). A bed
	 * that skips it leaves the join stamping one connection's numbers onto
	 * another's frames -- the very thing join_emit_gate() now refuses. */
	cnxman_csb_bind_connection(cnxman_club_find_sysid(&g_b.cl.club, dst),
				   CM_CONID);
	return 0;
}

static int jb_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
		       uint32_t len)
{
	(void)ctx; (void)conid; (void)body; (void)len;
	/* A REAL refusal code, not a bare -1: E70's whole point is that the
	 * ring must carry what the executive actually answered. 0 selects the
	 * historic -1 so every other test in this file is unchanged. */
	if (!g_b.fail_send)
		return 0;
	return g_b.fail_send_rc != 0 ? g_b.fail_send_rc : -1;
}

static uint64_t jb_time_now(void *ctx)
{
	(void)ctx;
	return 0x00bc021975280bc0ULL;
}

/* `with_ring` is the ONLY difference between the two runs group D compares. */
static void jbed_init(int with_ring)
{
	struct vms_csb *other, *member;

	memset(&g_b, 0, sizeof(g_b));
	fake_ops_init(&g_b.ops, &g_b.fake);
	g_b.fake.now_ms = 100000u;

	g_b.jops.dir_inquire = jb_dir_inquire;
	g_b.jops.connect = jb_connect;
	g_b.jops.send_msg = jb_send_msg;
	g_b.jops.time_now = jb_time_now;

	memcpy(g_b.cl.params.scsnode, "OVMXJ0", 6);
	g_b.cl.params.scsnode_len = 6;
	g_b.cl.params.scssystemid = OWN_SYSID;
	g_b.cl.params.vaxcluster = 2;

	(void)cnxman_club_init(&g_b.cl);
	other = cnxman_club_alloc_csb(&g_b.cl.club, OTHER_SYSID, 1);
	cnxman_csb_set_csid(other, 0x00010002u);
	member = cnxman_club_alloc_csb(&g_b.cl.club, MEMBER_SYSID, 1);
	cnxman_csb_set_csid(member, 0x00010001u);

	cnxman_join_init(&g_b.j, &g_b.cl, &g_b.ops, &g_b.jops);
	if (with_ring) {
		cnxman_diag_init(&g_b.ring, 1);
		cnxman_join_set_diag(&g_b.j, &g_b.ring);
	}
}

/*
 * The drive, identical in both runs. It deliberately begins with an event the
 * IDLE table has no cell for, so the transcript has to show an EMPTY CELL as
 * well as the transitions that fired.
 */
static void jbed_drive(void)
{
	uint8_t mscp_frame[VMS_MSCP_CMD_FRAME_LEN];

	memset(mscp_frame, 0, sizeof(mscp_frame));
	/* [IDLE][MSCP_END] is an empty cell: counted, never guessed. */
	cnxman_join_rx_mscp(&g_b.j, CM_CONID, mscp_frame, sizeof(mscp_frame));

	(void)cnxman_join_start(&g_b.j);
	cnxman_join_dir_result(&g_b.j, MEMBER_SYSID,
			       cnxman_join_name_mscp_disk, 0);
	cnxman_join_dir_result(&g_b.j, MEMBER_SYSID,
			       cnxman_join_name_vaxcluster, 1);
	cnxman_join_opened(&g_b.j, CM_CONID);
}

/* Find the first record matching a (kind, predicate). Returns 0 when there is
 * none -- an ABSENT record is a real answer here, so the search must be able to
 * report one. */
static int find_emit(uint8_t op, struct cnxman_diag_rec *out)
{
	uint32_t i, n = cnxman_diag_count(&g_b.ring);

	for (i = 0; i < n; i++) {
		if (!cnxman_diag_get(&g_b.ring, i, out))
			break;
		if (out->kind == (uint8_t)CNXMAN_DIAG_K_EMIT && out->op == op &&
		    out->cat == VMS_CM_CAT_CONFIG)
			return 1;
	}
	return 0;
}

static int find_dispatch(uint8_t event, struct cnxman_diag_rec *out)
{
	uint32_t i, n = cnxman_diag_count(&g_b.ring);

	for (i = 0; i < n; i++) {
		if (!cnxman_diag_get(&g_b.ring, i, out))
			break;
		if (out->kind == (uint8_t)CNXMAN_DIAG_K_DISPATCH &&
		    out->event == event)
			return 1;
	}
	return 0;
}

static void test_live_join_is_recorded(void)
{
	struct cnxman_diag_rec rec;

	printf("\n-- a real join drive writes a real transcript --\n");
	jbed_init(1);
	jbed_drive();

	ct_check(cnxman_diag_recorded(&g_b.ring) > 0u,
		 "the drive recorded something");

	ct_check(find_dispatch((uint8_t)CNXMAN_EV_MSCP_END, &rec),
		 "the event that hit an EMPTY CELL is recorded");
	ct_check_eq_u32(rec.detail, 0u,
			"... with detail 0 -- no table cell fired");
	ct_check_eq_u32(rec.state, (uint32_t)CNXMAN_JOIN_IDLE,
			"... and the STATE it hit, which the wire cannot show");
	ct_check_eq_u32(g_b.j.ignored_events, 1u,
			"the FSM's own ignored_events agrees with the ring");

	ct_check(find_dispatch((uint8_t)CNXMAN_EV_START, &rec),
		 "CLUSTER_START is recorded");
	ct_check_eq_u32(rec.state, (uint32_t)CNXMAN_JOIN_IDLE, "from IDLE");
	ct_check_eq_u32(rec.new_state, (uint32_t)CNXMAN_JOIN_DIR_ROUND,
			"to DIR ROUND");
	ct_check_eq_u32(rec.detail, 1u, "a table cell fired");

	/*
	 * ONE dispatch record spans the WHOLE handler, and this bed's handler
	 * runs far: with no MSCP$DISK on the member there is no discovery walk
	 * to wait for, so join_cm_advertise() emits MODEL + PARAMS and goes
	 * straight on through join_walk_complete() to ADMIT. So the record's
	 * `new_state` is ADMIT, and the three EMIT records with lower sequence
	 * numbers are what happened inside it -- exactly the "emits first, then
	 * the transition that closed them" ordering vms_cnxman_diag.h states.
	 */
	ct_check(find_dispatch((uint8_t)CNXMAN_EV_CDT_OPEN, &rec),
		 "the VMS$VAXcluster CDT_OPEN is recorded");
	ct_check_eq_u32(rec.state, (uint32_t)CNXMAN_JOIN_VC_CONNECT,
			"... from VC CONNECT");
	ct_check_eq_u32(rec.new_state, (uint32_t)CNXMAN_JOIN_ADMIT,
			"... and its handler carried the join through to ADMIT");

	ct_check(find_emit(VMS_CM_OP_MODEL, &rec),
		 "the cat-0x01 op-0x14 MODEL origination is recorded");
	ct_check_eq_u32(rec.detail, (uint32_t)CNXMAN_DIAG_G_SENT,
			"... with the gate SENT");
	ct_check_eq_u32(rec.aux, CM_CONID,
			"... on the Con.ID the join really holds");
	ct_check(find_emit(VMS_CM_OP_PARAMS, &rec),
		 "the cat-0x01 op-0x01 PARAMS origination is recorded");
	ct_check_eq_u32(rec.cat, VMS_CM_CAT_CONFIG,
			"the category is READ BACK from the body being sent");
	ct_check(find_emit(VMS_CM_OP_CONFIG, &rec),
		 "the cat-0x01 op-0x02 CONFIG that starts admission is recorded");

	/* The emits really do precede the dispatch that produced them. */
	{
		struct cnxman_diag_rec model, open;

		ct_check(find_emit(VMS_CM_OP_MODEL, &model) &&
			 find_dispatch((uint8_t)CNXMAN_EV_CDT_OPEN, &open),
			 "both records are present for the ordering check");
		ct_check(model.seq < open.seq,
			 "a handler's EMITs carry LOWER seq than its DISPATCH");
	}
}

static void test_a_refused_send_names_its_gate(void)
{
	struct cnxman_diag_rec rec;

	printf("\n-- a message that did NOT go out says why (the E69 case) --\n");
	jbed_init(1);
	g_b.fail_send = 1;
	g_b.fail_send_rc = 2692;   /* what the executive answers for a
				    * connection that cannot carry traffic */
	jbed_drive();

	ct_check(find_emit(VMS_CM_OP_MODEL, &rec),
		 "the MODEL attempt is recorded even though it never went");
	ct_check_eq_u32(rec.detail, (uint32_t)CNXMAN_DIAG_G_REFUSED,
			"... with the gate scs-refused");
	ct_check(rec.rc == 2692,
		 "... and SCS's OWN return code, VERBATIM -- E70: a flattened "
		 "-1 here fits five different defects and diagnosed none of "
		 "them on the live cluster");
	/* MODEL, PARAMS and -- because this member serves no disks, so the
	 * discovery walk is complete on arrival -- the op-0x02 CONFIG too. */
	ct_check_eq_u32(g_b.j.send_failures, 3u,
			"the FSM counted every refusal (MODEL, PARAMS, CONFIG)");
	ct_check(find_emit(VMS_CM_OP_CONFIG, &rec),
		 "the CONFIG attempt is recorded as well");
	ct_check_eq_u32(rec.detail, (uint32_t)CNXMAN_DIAG_G_REFUSED,
			"... with the same gate");
}

/*
 * THE ANTI-PERTURBATION PROOF. Identical drive, with and without a ring; the
 * FSM's whole context must be byte-identical afterwards. The `diag` pointer is
 * the one field that legitimately differs, so it is cleared in the copy before
 * the comparison -- and only that field.
 */
static void test_the_ring_changes_nothing(void)
{
	struct cnxman_join with, without;

	printf("\n-- OBSERVABILITY ONLY: the ring perturbs nothing --\n");

	jbed_init(1);
	jbed_drive();
	with = g_b.j;

	jbed_init(0);
	jbed_drive();
	without = g_b.j;

	ct_check(with.diag != NULL && without.diag == NULL,
		 "the two runs really differ only in whether a ring is bound");
	with.diag = NULL;

	ct_check(memcmp(&with, &without, sizeof(with)) == 0,
		 "the join FSM's ENTIRE context is byte-identical either way");
	ct_check_eq_u32(with.state, (uint32_t)CNXMAN_JOIN_ADMIT,
			"... and the drive really did run to ADMIT "
			"(a comparison of two no-ops would prove nothing)");
}

/* ==========================================================================
 * main
 * ========================================================================== */

int main(void)
{
	printf("=== test_cnxman_diag (E69: the join transition ring) ===\n");

	test_empty_ring();
	test_records_in_order();
	test_wrap_drops_the_oldest();
	test_disabled_and_absent();
	test_identical_facts_coalesce();
	test_a_different_fact_breaks_the_run();
	test_the_drive_survives_a_long_wait();
	test_every_field_round_trips();

	test_name_tables_agree();
	test_port_refusal_is_named_not_collapsed();

	test_live_join_is_recorded();
	test_a_refused_send_names_its_gate();
	test_the_ring_changes_nothing();

	return ct_summary("test_cnxman_diag");
}
