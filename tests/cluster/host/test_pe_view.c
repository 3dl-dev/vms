/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_view.c - FC-P0.9's pure port-view assembler, pe_fsm_view_project().
 *
 * The glue (vms_pe.c) is not host-testable: it names exec_kbackend.h and only
 * links into the two kmods. What IS pure -- reading n_channels/n_vcs/rx_frames/
 * rx_drops_badclass straight off this FSM's own fields -- lives in
 * vms_pe_fsm.c so it gets the same host rung every other projection in this
 * layer gets (pe_fsm_channel_project, pe_fsm_vc_project). This file is that
 * projection's R1 test: a fresh FSM projects all-zero, a real HELLO advances
 * rx_frames, a verified channel advances n_channels, and a frame the codec
 * cannot classify advances rx_drops_badclass -- never a placeholder count.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "pe_fake_ops.h"

#define OVMX_SYSID 1030u

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

struct pe_env {
	struct pe_fsm    fsm;
	struct pe_ops    ops;
	struct fake_pe   fake;
	struct fake_peer peer;
	uint8_t          buf[VMS_HELLO_PADDED_MAX_FRAME];
};

static void env_init(struct pe_env *e)
{
	struct pe_identity id;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = 1500;

	(void)pe_fsm_init(&e->fsm, &id, OVMX_SYSID, &e->ops);
	pe_fsm_start(&e->fsm);
	fake_peer_init(&e->peer, 1025, vax1_hw, "VAX1");

	/* pe_fsm_start's own cadence beat does not emit anything (nothing is
	 * armed until the first tick), but clear the capture buffer anyway so
	 * every test's frame count is its own, not a leftover from init. */
	fake_pe_clear_frames(&e->fake);
}

static void test_null_is_safe(void)
{
	printf("-- NULL in, NULL out: no crash, nothing claimed\n");
	pe_fsm_view_project(NULL, NULL);
}

static void test_fresh_fsm_is_all_zero(void)
{
	struct pe_env e;
	struct vms_pe_view v;

	printf("-- a fresh FSM's view is all-zero: nothing happened yet\n");
	env_init(&e);
	memset(&v, 0xff, sizeof(v));   /* poison, so a missed field would show */
	pe_fsm_view_project(&e.fsm, &v);
	ct_check_eq_u32(v.n_channels, 0, "no channel seen yet");
	ct_check_eq_u32(v.n_vcs, 0, "no circuit formed yet");
	ct_check_eq_u32(v.rx_frames, 0, "no frame received yet");
	ct_check_eq_u32(v.rx_drops_badclass, 0, "nothing dropped yet");
}

static void test_a_real_hello_advances_the_view(void)
{
	struct pe_env e;
	struct vms_pe_view v;
	uint32_t len;
	uint8_t dst_lavc[6];

	printf("-- one directed HELLO: rx_frames and n_channels both advance\n");
	env_init(&e);
	vms_cluster_lavc_addr_build(OVMX_SYSID, dst_lavc);
	len = fake_peer_hello(&e.peer, ovmx_hw, dst_lavc, PE_PFW_VERIFY_B3, 1, 0,
			      e.buf, sizeof(e.buf));
	ct_check_eq_u32(len != 0, 1, "the fixture built a real frame");
	(void)pe_fsm_rx(&e.fsm, e.buf, len);

	pe_fsm_view_project(&e.fsm, &v);
	ct_check_eq_u32(v.n_channels, 1, "one station now has a channel row");
	ct_check_eq_u32(v.rx_frames, 1, "the one frame handed to pe_fsm_rx");
	ct_check_eq_u32(v.rx_drops_badclass, 0, "a real HELLO classifies fine");
}

static void test_unclassifiable_frame_counts_as_a_drop(void)
{
	struct pe_env e;
	struct vms_pe_view v;
	uint8_t frame[60];

	printf("-- a frame the codec cannot classify: counted, not silent\n");
	env_init(&e);
	memset(frame, 0, sizeof(frame));
	memcpy(frame + 0, ovmx_hw, 6);
	memcpy(frame + 6, vax1_hw, 6);
	frame[12] = 0x60;
	frame[13] = 0x07;      /* SCA ethertype, so pe_fsm_rx accepts it */
	/* A zero body matches neither the discovery prefix (abs 14: must be
	 * 08 00 00 80) nor the SCS format byte (abs 31: must be 0x13), so the
	 * codec's own rule table classifies this frame as NEITHER family --
	 * the real, GROUNDED "no rule matched" outcome, not a hand-picked
	 * offset this test happens to know breaks something. */
	frame[14] = 0xffu;

	(void)pe_fsm_rx(&e.fsm, frame, sizeof(frame));

	pe_fsm_view_project(&e.fsm, &v);
	ct_check_eq_u32(v.rx_frames, 1, "the frame WAS received");
	ct_check(v.rx_drops_badclass >= 1,
		"and counted as undecodable, never guessed at");
}

int main(void)
{
	test_null_is_safe();
	test_fresh_fsm_is_all_zero();
	test_a_real_hello_advances_the_view();
	test_unclassifiable_frame_counts_as_a_drop();
	return ct_summary("test_pe_view");
}
