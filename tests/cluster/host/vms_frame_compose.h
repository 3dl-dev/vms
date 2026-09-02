/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_frame_compose.h - TEST-ONLY full-frame composer for the
 * `VMS$VAXcluster` (CM) wire class.
 *
 * Design: docs/design-faithful-cluster-executive.md sec 3.2.4 ("A full-frame
 * composer (vms_frame_compose(link, scs, body)) exists for tests and the
 * simulator only -- it is how rung-1 byte-exactness against a 204-byte
 * specimen and the pcap replay are done -- and is never called from a
 * layer.").
 *
 * THIS FILE IS THE DEMOTED `struct vms_cm_link` / `vms_cm_link_build()` FROM
 * vms_cluster_codec_cm.h (FC-P3.1), moved here by FC-P3.15's body-level
 * conformance retrofit (design sec 3.2.4 ruling E1: "a SYSAP that fills
 * send_seq is the same category error as a daemon that fills a lock id").
 * Production CNXMAN code never builds a full frame; a body only, through
 * `cnxman_ops.send`/`respond` (vms_cnxman.h), after `cnxman_envelope_stamp()`
 * (vms_cnxman_csb.h). This composer exists ONLY so:
 *
 *   - a host test can assemble a byte-exact 204-byte specimen from a
 *     codec_cm builder's 132-byte body output, to compare against a captured
 *     RESPONSE fixture (test_codec_cm.c);
 *   - a host test can assemble a full 204-byte REQUEST specimen to feed
 *     `cnxman_barrier_rx_frame()`/`cnxman_coord_rx_frame()`, whose RECEIVE
 *     side is unaffected by this retrofit (design sec 3.2.4: the port
 *     delivers the whole frame on receive; only ORIGINATION/RESPONSE changed
 *     to body-level) (test_cnxman_barrier.c, test_cnxman_coord.c);
 *   - the rung-2 N-node simulator replays a pcap.
 *
 * No production translation unit includes this header --
 * tools/ci/cluster_core_includes_gate.sh enforces kernel-core-only includes
 * there, and this file lives outside src/kernel-core entirely.
 *
 * SCOPE, unchanged from the demoted vms_cm_link: a MINIMAL, honestly-scoped
 * stand-in for abs [0,72) that builds only the fields spec sec 4(d) itself
 * grounds (the SCA header, recv_ack/send_seq, the Con.ID pair, the CM
 * msgtype/format markers) and leaves every other byte of that span an
 * explicit zero -- never an invented mirror or a captured template. A
 * fuller/richer envelope is FC-P1.1/FC-P2.1's job; this stays a test fixture
 * assembler, never a claim to be that layer.
 */
#ifndef OVMX_TESTS_VMS_FRAME_COMPOSE_H
#define OVMX_TESTS_VMS_FRAME_COMPOSE_H

#include "vms_cluster_codec_cm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The abs [0,72) span: Ethernet/SCA header (abs 0-31), the VC's recv_ack/
 * send_seq (abs 32-35), the CM's link-overhead constant (abs 38-39), and the
 * SCS Con.ID pair (abs 64-71). Identical fields to the demoted `vms_cm_link`. */
struct vms_cm_link {
	struct vms_sca_hdr hdr;   /* abs 0-31; hdr.word30 is IGNORED by the
				   * composer and overwritten with the CM
				   * msgtype/format marker (msgtype 0x4b,
				   * format 0x13, GROUNDED sec 4(g)/4(d))    */
	uint16_t recv_ack;        /* abs 32, GROUNDED sec 4(d)/(h)             */
	uint16_t send_seq;        /* abs 34, GROUNDED sec 4(d)/(h)             */
	uint32_t remote_conid;    /* abs 64, GROUNDED sec 4(d): the PEER's own
				   * Con.ID, as the sender addresses it        */
	uint32_t local_conid;     /* abs 68, GROUNDED sec 4(d): OUR own Con.ID */
};

/*
 * vms_frame_compose_link - write ONLY the abs [0,72) span into a >=204-byte
 * buffer, leaving [72,204) untouched (a caller that wants a full specimen
 * still has to fill the body itself -- e.g. a test building a synthetic
 * REQUEST field by field through vms_wire_put_*). Bakes in ONLY the
 * discovery-independent CM format markers (connect flag 0x0001, msgtype
 * 0x4b / format 0x13, and the sec 4(d)-grounded constant 18 at abs 38-39);
 * every other byte in [36,64) and [70,72) is left ZERO rather than guessed.
 */
vms_codec_status_t vms_frame_compose_link(const struct vms_cm_link *l,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written);

/*
 * vms_frame_compose - assemble a full VMS_CM_FRAME_LEN specimen from a link
 * (abs [0,72)) and an already-built VMS_CM_BODY_LEN body (abs [72,204)) --
 * exactly what a codec_cm builder now returns. This is the function the
 * design names in full as `vms_frame_compose(link, scs, body)`; the `scs`
 * parameter (the abs [56,71) SCS Con.ID header, split out as its own layer)
 * is not yet a struct of its own -- FC-P2.1's SCS codec has not landed --
 * so `link` still carries that span the way the demoted vms_cm_link always
 * did. The day FC-P2.1 lands, this composer's `link` narrows to abs [0,55]
 * and a separate `scs` parameter is added; nothing here asserts to be that
 * layer today.
 */
vms_codec_status_t vms_frame_compose(const struct vms_cm_link *l,
				     const uint8_t body[VMS_CM_BODY_LEN],
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_TESTS_VMS_FRAME_COMPOSE_H */
