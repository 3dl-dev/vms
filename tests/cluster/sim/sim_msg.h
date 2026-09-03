/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sim_msg.h - the simulator's stand-in upper layer: it builds a REAL sequenced
 * SCS frame and hands it to the port's own send service (FC-P1.4).
 *
 * WHY A SEPARATE FILE. The port owns the SEQUENCE and the caller owns the
 * MESSAGE -- vms_pe_fsm.h §3b, "WHAT THIS LAYER DOES NOT DO". Until SCS lands
 * (FC-P2.2) there is no caller, and without one the circuit never carries a
 * sequenced message, so the retransmit ladder, the cumulative acknowledgement
 * and the credit window never run. This is the smallest honest caller: it
 * builds the frame with the SAME codec entries the executive would use,
 * addressed from the circuit's OWN learned addressing (`pe_vc_addr`, which
 * exists for exactly this), and passes it to `pe_vc_send_frame`.
 *
 * WHAT IS AND IS NOT A CLAIM HERE. The frame's transport fields -- the
 * sequence, the acknowledgement, the retransmit marking -- are stamped by the
 * FSM out of real circuit state; this file never writes one. The Con.ID pair
 * is the harness's own test identity, derived from the node index, because no
 * CDT exists yet to own a real one; it is labelled as such, it never leaves
 * the host, and the moment FC-P2.2 lands its CDT allocator replaces it. The
 * frame class is the §4(d) 190-content one, the only class whose Con.ID
 * location the wire spec independently grounds (17557/17557).
 */
#ifndef OVMX_SIM_MSG_H
#define OVMX_SIM_MSG_H

#include <stdint.h>

struct sim;
struct sim_node;

/* The §4(d) 190-content class: 190 SCA bytes + the 14-byte Ethernet header. */
#define SIM_MSG_SCA 190u
#define SIM_MSG_LEN (14u + SIM_MSG_SCA)

/*
 * Offer `count` sequenced messages from `from` to `to`. Returns how many the
 * port ACCEPTED; the rest were refused (no circuit, no credit, ring full) and
 * the reason of the last refusal is left in `from->last_send_status`. A
 * refusal is a real answer from the FSM and is recorded, never retried behind
 * the scenario's back -- p. 2-43's credit window is a thing scenarios test.
 */
uint32_t sim_send_msgs(struct sim *s, struct sim_node *from,
		       struct sim_node *to, uint32_t count);

#endif /* OVMX_SIM_MSG_H */
