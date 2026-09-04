/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_diag.c - the JOIN's transition ring (E69).
 *
 * Read vms_cnxman_diag.h first: it carries what the ring is, the three
 * properties that make it safe to leave recording on through a live-cluster
 * run, and the record's field-by-field provenance. This file is the mechanism
 * and nothing else -- a bounded store, a bounded read, and five name tables.
 *
 * ONE RULE GOVERNS EVERY LINE HERE: a diagnostic may not fail, may not block
 * and may not change anything it observes. So there is no return code on the
 * recording path, no allocation, no clock read, no lock, and no branch that
 * depends on what is being recorded.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * PURE TU: no seam call, no allocation, no clock but the caller's.
 */

#include "vms_cnxman_diag.h"
#include "vms_cnxman_join_fsm.h"   /* enum cnxman_join_rx, for rx_name only */
#include "vms_pe_fsm.h"            /* E70: enum pe_vc_send_status + the port's
				    * refusal readback, for the two mappers in
				    * SS8b -- vocabulary only, no port call */

/* This TU calls no library: a pure TU builds on the host too, where the
 * substrate's memset is not in scope. */
static void diag_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void cnxman_diag_init(struct cnxman_diag_ring *r, int enabled)
{
	if (r == (struct cnxman_diag_ring *)0)
		return;
	diag_bzero(r, (uint32_t)sizeof(*r));
	r->enabled = enabled ? 1u : 0u;
}

void cnxman_diag_enable(struct cnxman_diag_ring *r, int enabled)
{
	if (r == (struct cnxman_diag_ring *)0)
		return;
	r->enabled = enabled ? 1u : 0u;
}

/* ==========================================================================
 * Recording
 *
 * ONE store, from ONE place. `recorded` is both the number of SLOTS ever taken
 * and the sequence number of the next one, which is what makes a gap in the
 * `seq` column an unambiguous report of the wrap rather than something a
 * reader has to infer.
 * ========================================================================== */

/*
 * Is this the SAME FACT as the record before it? Every field that carries
 * MEANING is compared; the two that carry only WHEN (`t_ms`) and WHICH SLOT
 * (`seq`) are not, and neither are the coalescing fields themselves.
 *
 * This is the whole of the SS4b rule. It is deliberately an exact comparison of
 * named fields rather than a memcmp: a memcmp would silently start including
 * any field a later item appends, which is how a coalescing rule turns into a
 * subtle omission.
 */
static int diag_same_fact(const struct cnxman_diag_rec *a,
			  const struct cnxman_diag_rec *b)
{
	return a->kind == b->kind && a->state == b->state &&
	       a->new_state == b->new_state && a->event == b->event &&
	       a->detail == b->detail && a->cat == b->cat &&
	       a->op == b->op && a->rx == b->rx &&
	       a->rc == b->rc && a->aux == b->aux;
}

/* The most recently STORED record, or NULL when the ring is empty. After a
 * wrap this is still the newest one: the slot is derived from the sequence, so
 * the two can never disagree. */
static struct cnxman_diag_rec *diag_newest(struct cnxman_diag_ring *r)
{
	if (r->recorded == 0u)
		return (struct cnxman_diag_rec *)0;
	return &r->slot[(r->recorded - 1u) & CNXMAN_DIAG_SLOT_MASK];
}

/*
 * Offer one fully-built record to the ring. Either it coalesces into the
 * previous one (SS4b) or it takes the next slot. No return value: a diagnostic
 * that could fail would be a second thing to diagnose.
 */
static void diag_store(struct cnxman_diag_ring *r, struct cnxman_diag_rec *rec)
{
	struct cnxman_diag_rec *prev = diag_newest(r);

	if (prev != (struct cnxman_diag_rec *)0 && diag_same_fact(prev, rec)) {
		prev->repeat++;
		prev->t_last_ms = rec->t_ms;
		return;
	}

	rec->seq = r->recorded;
	rec->t_last_ms = rec->t_ms;
	rec->repeat = 0u;
	r->slot[r->recorded & CNXMAN_DIAG_SLOT_MASK] = *rec;
	r->recorded++;
}

/* The one gate every recorder passes through: a NULL or disabled ring records
 * nothing, and that is a legitimate wiring, not an error. Returns 0 when the
 * caller should not bother building a record at all. */
static int diag_begin(const struct cnxman_diag_ring *r,
		      struct cnxman_diag_rec *rec, uint32_t t_ms,
		      uint8_t state, uint8_t kind)
{
	if (r == (const struct cnxman_diag_ring *)0 || !r->enabled)
		return 0;

	diag_bzero(rec, (uint32_t)sizeof(*rec));
	rec->t_ms      = t_ms;
	rec->kind      = kind;
	rec->state     = state;
	rec->new_state = state;
	rec->event     = CNXMAN_DIAG_EV_NONE;
	return 1;
}

void cnxman_diag_dispatch(struct cnxman_diag_ring *r, uint32_t t_ms,
			  uint8_t state, uint8_t new_state, uint8_t event,
			  int handled, uint8_t rx, uint32_t aux)
{
	struct cnxman_diag_rec rec;

	if (!diag_begin(r, &rec, t_ms, state, (uint8_t)CNXMAN_DIAG_K_DISPATCH))
		return;
	rec.new_state = new_state;
	rec.event     = event;
	rec.detail    = handled ? 1u : 0u;
	rec.rx        = rx;
	rec.aux       = aux;
	diag_store(r, &rec);
}

void cnxman_diag_arrival(struct cnxman_diag_ring *r, uint32_t t_ms,
			 uint8_t state, uint8_t event, uint8_t reason,
			 int32_t rc, uint32_t aux)
{
	struct cnxman_diag_rec rec;

	if (!diag_begin(r, &rec, t_ms, state, (uint8_t)CNXMAN_DIAG_K_ARRIVAL))
		return;
	rec.event  = event;
	rec.detail = reason;
	rec.rc     = rc;
	rec.aux    = aux;
	diag_store(r, &rec);
}

void cnxman_diag_emit(struct cnxman_diag_ring *r, uint32_t t_ms,
		      uint8_t state, uint8_t cat, uint8_t op, uint8_t gate,
		      int32_t rc, uint32_t aux)
{
	struct cnxman_diag_rec rec;

	if (!diag_begin(r, &rec, t_ms, state, (uint8_t)CNXMAN_DIAG_K_EMIT))
		return;
	rec.cat    = cat;
	rec.op     = op;
	rec.detail = gate;
	rec.rc     = rc;
	rec.aux    = aux;
	diag_store(r, &rec);
}

/* ==========================================================================
 * Readback
 * ========================================================================== */

uint32_t cnxman_diag_count(const struct cnxman_diag_ring *r)
{
	if (r == (const struct cnxman_diag_ring *)0)
		return 0u;
	if (r->recorded > CNXMAN_DIAG_SLOTS)
		return CNXMAN_DIAG_SLOTS;
	return r->recorded;
}

uint32_t cnxman_diag_recorded(const struct cnxman_diag_ring *r)
{
	if (r == (const struct cnxman_diag_ring *)0)
		return 0u;
	return r->recorded;
}

int cnxman_diag_get(const struct cnxman_diag_ring *r, uint32_t i,
		    struct cnxman_diag_rec *out)
{
	uint32_t held = cnxman_diag_count(r);
	uint32_t oldest;

	if (out == (struct cnxman_diag_rec *)0 || i >= held)
		return 0;

	/* The oldest record still held is `recorded - held` in SEQUENCE terms;
	 * its slot is that sequence masked. Deriving the slot from the sequence
	 * (rather than from a separate head cursor) is what keeps the two from
	 * ever disagreeing after a wrap. */
	oldest = r->recorded - held;
	*out = r->slot[(oldest + i) & CNXMAN_DIAG_SLOT_MASK];
	return 1;
}

/* ==========================================================================
 * Names
 *
 * Plain switch statements rather than indexed tables: a switch cannot be
 * silently mis-indexed by a value inserted into the middle of an enum, and
 * every one of these renders an ordinal that crosses /dev/vms.
 * ========================================================================== */

const char *cnxman_diag_kind_name(uint8_t kind)
{
	switch (kind) {
	case CNXMAN_DIAG_K_DISPATCH: return "DISPATCH";
	case CNXMAN_DIAG_K_ARRIVAL:  return "ARRIVAL";
	case CNXMAN_DIAG_K_EMIT:     return "EMIT";
	default:                     return "?";
	}
}

const char *cnxman_diag_reason_name(uint8_t reason)
{
	switch (reason) {
	case CNXMAN_DIAG_R_NONE:       return "-";
	case CNXMAN_DIAG_R_DISPATCHED: return "dispatched";
	case CNXMAN_DIAG_R_UNPARSED:   return "unparsed";
	case CNXMAN_DIAG_R_NOT_MINE:   return "not-mine";
	case CNXMAN_DIAG_R_PEER_ACK:   return "peer-ack";
	case CNXMAN_DIAG_R_ACCEPTED:   return "accepted";
	case CNXMAN_DIAG_R_REFUSED:    return "refused-no-csb";
	case CNXMAN_DIAG_R_MSCP_REJ:   return "mscp-rejected";
	case CNXMAN_DIAG_R_CM_REJ:     return "cm-rejected";
	case CNXMAN_DIAG_R_NOT_OURS:   return "not-our-conid";
	case CNXMAN_DIAG_R_CDT_OPEN:   return "cdt-open";
	case CNXMAN_DIAG_R_CM_ACCEPT:  return "cm-accept";
	case CNXMAN_DIAG_R_CDT_CLOSED: return "cdt-closed";
	case CNXMAN_DIAG_R_SEND_REFUSED: return "send-refused";
	case CNXMAN_DIAG_R_PORT_NOCIRCUIT: return "port-nocircuit";
	case CNXMAN_DIAG_R_PORT_NOCREDIT:  return "port-nocredit";
	case CNXMAN_DIAG_R_PORT_RINGFULL:  return "port-ringfull";
	case CNXMAN_DIAG_R_PORT_BADFRAME:  return "port-badframe";
	case CNXMAN_DIAG_R_PORT_TXFAIL:    return "port-txfail";
	case CNXMAN_DIAG_R_CDT_NOT_SENDABLE: return "cdt-not-sendable";
	default:                       return "?";
	}
}

const char *cnxman_diag_gate_name(uint8_t gate)
{
	switch (gate) {
	case CNXMAN_DIAG_G_SENT:    return "SENT";
	case CNXMAN_DIAG_G_NO_CSB:  return "no-csb";
	case CNXMAN_DIAG_G_NO_CONN: return "no-open-vc";
	case CNXMAN_DIAG_G_NO_OPS:  return "no-send-op";
	case CNXMAN_DIAG_G_REFUSED: return "scs-refused";
	case CNXMAN_DIAG_G_CODEC:   return "codec-refused";
	default:                    return "?";
	}
}

const char *cnxman_diag_rx_name(uint8_t rx)
{
	switch (rx) {
	case CNXMAN_JOIN_RX_CONSUMED: return "consumed";
	case CNXMAN_JOIN_RX_HANDOFF:  return "handoff";
	case CNXMAN_JOIN_RX_NOT_MINE: return "not-mine";
	case CNXMAN_JOIN_RX_BAD:      return "bad";
	default:                      return "?";
	}
}

const char *cnxman_diag_event_name(uint8_t event)
{
	switch (event) {
	case CNXMAN_EV_CDT_OPEN:       return "CDT_OPEN";
	case CNXMAN_EV_CDT_CLOSED:     return "CDT_CLOSED";
	case CNXMAN_EV_RX_MEMBERSHIP:  return "RX_MEMBERSHIP";
	case CNXMAN_EV_RX_TR_REQUEST:  return "RX_TR_REQUEST";
	case CNXMAN_EV_RX_TR_RELAY:    return "RX_TR_RELAY";
	case CNXMAN_EV_RX_TR_OPEN:     return "RX_TR_OPEN";
	case CNXMAN_EV_RX_TR_GO:       return "RX_TR_GO";
	case CNXMAN_EV_RX_BARRIER:     return "RX_BARRIER";
	case CNXMAN_EV_RX_BARRIER_ACK: return "RX_BARRIER_ACK";
	case CNXMAN_EV_RX_REBUILD:     return "RX_REBUILD";
	case CNXMAN_EV_RX_CLOSE:       return "RX_CLOSE";
	case CNXMAN_EV_START:          return "START";
	case CNXMAN_EV_CSID_LEARNED:   return "CSID_LEARNED";
	case CNXMAN_EV_TIMER_RECNX:    return "TIMER_RECNX";
	case CNXMAN_EV_TIMER_JOIN:     return "TIMER_JOIN";
	case CNXMAN_EV_TIMER_BARRIER:  return "TIMER_BARRIER";
	case CNXMAN_EV_SHUTDOWN:       return "SHUTDOWN";
	case CNXMAN_EV_RX_TR_ACK:      return "RX_TR_ACK";
	case CNXMAN_EV_DIR_RESULT:     return "DIR_RESULT";
	case CNXMAN_EV_MSCP_END:       return "MSCP_END";
	case CNXMAN_EV_RX_CONFIG:      return "RX_CONFIG";
	case CNXMAN_EV_RX_COMMIT:      return "RX_COMMIT";
	case CNXMAN_EV_CM_ACCEPTED:    return "CM_ACCEPTED";
	case CNXMAN_DIAG_EV_NONE:      return "-";
	default:                       return "?";
	}
}

/* ==========================================================================
 * 8b. The PORT's refusal, in this ring's vocabulary (E70)
 *
 * WHY IT LIVES HERE and not in the connection manager's glue: it is a mapping
 * between two VOCABULARIES -- the port's `enum pe_vc_send_status` and this
 * ring's `enum cnxman_diag_reason` -- and this TU is the one that owns the
 * second of them. Keeping it here also makes it a PURE function the R1 tests
 * drive directly, instead of a branch that only a booted node could exercise;
 * the glue that calls it is not host-linkable, and "the trace named the wrong
 * cause" is precisely the defect that must not wait for a lab run to surface.
 *
 * Nothing here reads the port. It is handed a readback the caller already
 * took, and it decides only which NAME and which single live number the
 * record carries.
 * ========================================================================== */

enum cnxman_diag_reason cnxman_diag_port_reason(int32_t code)
{
	switch (code) {
	case PE_VC_SEND_NOCIRCUIT: return CNXMAN_DIAG_R_PORT_NOCIRCUIT;
	case PE_VC_SEND_NOCREDIT:  return CNXMAN_DIAG_R_PORT_NOCREDIT;
	case PE_VC_SEND_RINGFULL:  return CNXMAN_DIAG_R_PORT_RINGFULL;
	/* Two port codes, one cause as a reader meets it: the port was handed
	 * something it cannot put on this circuit. `rc` still carries WHICH of
	 * the two it was, verbatim, so nothing is lost by naming them alike. */
	case PE_VC_SEND_BADFRAME:  return CNXMAN_DIAG_R_PORT_BADFRAME;
	case PE_VC_SEND_TOOBIG:    return CNXMAN_DIAG_R_PORT_BADFRAME;
	/* Including PE_VC_SEND_TXFAIL, and including a code this vocabulary
	 * has not grown yet: an unknown refusal is recorded as the honest
	 * catch-all WITH its real number in `rc`, never dropped and never
	 * assigned a neighbour's name. */
	default:                   return CNXMAN_DIAG_R_PORT_TXFAIL;
	}
}

uint32_t cnxman_diag_port_aux(const struct pe_vc_send_refusal *p)
{
	if (p == (const struct pe_vc_send_refusal *)0)
		return 0u;
	switch (p->code) {
	case PE_VC_SEND_NOCIRCUIT:
		/* The circuit's LIVE state -- or the fact that the port holds
		 * no circuit object for that system at all, which is a
		 * stronger answer than any state and is reported as itself. */
		return p->vc_present ? (uint32_t)p->vc_state
				     : CNXMAN_DIAG_NO_VC;
	case PE_VC_SEND_NOCREDIT: return p->send_refused_credit;
	case PE_VC_SEND_RINGFULL: return p->send_refused_ring;
	/* The port keeps no count of unsendable frames or interface failures,
	 * and inventing one would be worse than an explicit zero. */
	default:                  return 0u;
	}
}
