// SPDX-License-Identifier: GPL-2.0
/*
 * test_pe_group_on_wire.c - the port puts ITS OWN cluster group number on
 * EVERY frame it emits, and ignores anybody else's (rd vms-b34, rung R1).
 *
 * THE DEFECT THIS PINS, AS IT WAS MEASURED (isolated bridge in vaxlab-4,
 * 2026-09-24; tests/lab/captures/vms-b34-group-on-wire-20260924/).
 *
 * A real OpenVMS VAX V7.3 node VAXC (SCSSYSTEMID 1989) founded a one-member
 * VMScluster in group 257. A booted OVMX node OVMXA (1987) configured for the
 * SAME group 257 came up beside it. On the wire:
 *
 *  1712  VAXC  -> ab:00:04:01:01:02   multicast HELLO   abs 22 = 01 01
 *   240  OVMXA -> ab:00:04:01:01:02   multicast HELLO   abs 22 = 01 01
 *   403  VAXC <-> OVMXA               b3/b4 chan verify abs 22 = 01 01
 *    96  VAXC  -> OVMXA               0x41 VC START     abs 22 = 01 01
 *   803  OVMXA -> VAXC                0x41 VC STACK     abs 22 = 00 01   <-- WRONG
 *     0  (either direction)           0x48 VC ACK
 *
 * The channel verified. The circuit never opened: VAXC discarded all 803
 * STACKs because they claimed cluster group 1, and re-sent its START for the
 * whole window. No vc_up, so no CSB, so the join FSM had no target and CNXMAN
 * honestly said nothing after "waiting to form or join". SHOW CLUSTER on both
 * sides listed only self, for twenty minutes in the browser and for the whole
 * lab window.
 *
 * abs 22 was built from the constant `0x0001` in three separate places,
 * labelled "the observed constant connect flag". It is LE16(cluster group)
 * -- vms_cluster_codec.h carries the four real-VMS oracles, and
 * test_codec_hello.c asserts the derivation against all four. 0x0001 is
 * correct for exactly one cluster: the lab's, which is group 1, which is why
 * every earlier lab join worked and this one could not.
 *
 * WHAT IS ASSERTED HERE, on the real pe_fsm with injected ops and no boot:
 *   1. a port configured for group 257 emits 257 at abs 22 on its multicast
 *      HELLO with NO peer heard -- so the number cannot be coming from an
 *      echo of somebody else's frame;
 *   2. the frame that failed -- the STACK answering a real-shaped peer START
 *      -- carries 257;
 *   3. two ports differing ONLY in configured group emit different abs 22,
 *      so a re-baked constant reddens this file;
 *   4. a frame carrying ANOTHER cluster's number is refused and counted,
 *      never processed into a channel or a circuit.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "pe_fake_ops.h"
#include "pe_fake_vc.h"

#define OVMXA_SYSID 1987u
#define VAXC_SYSID  1989u
#define DEMO_GROUP  257u
#define LAB_GROUP   1u
#define PEER_CREDITS 10u

static const uint8_t ovmxa_hw[6] = { 0x52, 0x54, 0x00, 0x00, 0x00, 0x0a };
static const uint8_t vaxc_hw[6]  = { 0x08, 0x00, 0x2b, 0xab, 0x1e, 0x6c };

struct env {
	struct pe_fsm    fsm;
	struct pe_ops    ops;
	struct fake_pe   fake;
	struct fake_peer peer;
	struct pe_vc     vcs[2];
	uint8_t          mcast[VMS_ETH_ADDR_LEN];
	uint8_t          buf[VMS_HELLO_PADDED_MAX_FRAME];
};

static struct env g_env;   /* ~20 KB of FSM: static, not a stack frame */

/* The VMS absolute-time clock the formation body needs: a monotonic function
 * of the injected millisecond clock, so it is live and still reproducible. */
static uint64_t fake_now_vms(void *ctx)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	return 0x00bc055269000000ull + (uint64_t)f->now_ms * 10000ull;
}

/* A port whose CLUSTER_AUTHORIZE record names `group`, and a peer in `group`
 * too unless a test moves it. Both encodings of the number come from the one
 * argument, exactly as pe_build_identity() takes both from the one record. */
static void env_init(struct env *e, uint16_t group)
{
	struct pe_identity id;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);
	e->ops.now_vms = fake_now_vms;
	vms_cluster_hello_mcast_build(group, e->mcast);

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmxa_hw, VMS_ETH_ADDR_LEN);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMXA ", VMS_HELLO_NODENAME_MAX);
	id.scsnode_len = 5;
	memcpy(id.mcast, e->mcast, VMS_ETH_ADDR_LEN);
	id.mcast_valid = 1;
	id.cluster_group = group;
	id.cluster_group_valid = 1u;
	id.max_sca_len = 1500;
	memcpy(id.sw_version, "VMX V0.7", VMS_SCS_START_SWVER_LEN);
	id.sw_version_valid = 1;
	memcpy(id.hw_type, "X86 ", VMS_SCS_START_HWTYPE_LEN);
	id.hw_type_valid = 1;
	id.incarnation_time = 0x00bc0552690a0000ull;
	id.incarnation_time_valid = 1;
	id.credits_requested = PEER_CREDITS;
	id.credits_requested_valid = 1;
	id.rx_pool_bufs = 32u;

	(void)pe_fsm_init(&e->fsm, &id, OVMXA_SYSID, &e->ops);
	pe_fsm_bind_vcs(&e->fsm, e->vcs, 2);
	pe_fsm_start(&e->fsm);

	fake_peer_init(&e->peer, VAXC_SYSID, vaxc_hw, "VAXC");
	e->peer.cluster_group = group;
}

static void ovmxa_lavc(uint8_t out[VMS_ETH_ADDR_LEN])
{
	vms_cluster_lavc_addr_build(OVMXA_SYSID, out);
}

/* abs 22 of the `index`-th frame this port emitted, or -1 if there is none.
 * Read off the recorded bytes, which is what a peer's NIC sees. */
static long emitted_group(const struct fake_pe *fk, uint32_t index)
{
	const uint8_t *f;

	if (index >= fk->n_frames || fk->frame[index].len < 24u)
		return -1;
	f = fk->frame[index].b;
	return (long)((uint32_t)f[22] | ((uint32_t)f[23] << 8));
}

/* How many emitted frames carry `group` at abs 22, and how many do not. */
static void count_emitted_groups(const struct fake_pe *fk, uint16_t group,
				 uint32_t *match, uint32_t *other)
{
	uint32_t i;

	*match = 0u;
	*other = 0u;
	for (i = 0; i < fk->n_frames; i++) {
		long g = emitted_group(fk, i);

		if (g < 0)
			continue;
		if ((uint16_t)g == group)
			(*match)++;
		else
			(*other)++;
	}
}

/* One DIRECTED HELLO from the peer, carrying the sec 4(a).1 channel word. */
static void rx_directed_hello(struct env *e, uint8_t word)
{
	uint8_t dst_lavc[VMS_ETH_ADDR_LEN];

	ovmxa_lavc(dst_lavc);
	(void)pe_fsm_rx(&e->fsm, e->buf,
			fake_peer_hello(&e->peer, ovmxa_hw, dst_lavc, word,
					1u, 0u, e->buf, sizeof(e->buf)));
}

/* Walk the channel to b4 -- the path a real formation takes, and the one the
 * lab run took before the circuit stalled. */
static void bring_channel_up(struct env *e)
{
	rx_directed_hello(e, PE_PFW_VERIFY_B2);
	rx_directed_hello(e, PE_PFW_VERIFY_B4);
}

/* ------------------------------------------------------------------ *
 * 1. The number is OURS, and it is on the wire before any peer is heard
 * ------------------------------------------------------------------ */
static void test_own_group_without_any_peer(void)
{
	struct env *e = &g_env;
	uint32_t match = 0, other = 0;

	printf("-- a port in group 257 emits 257 at abs 22, peer or no peer\n");

	env_init(e, DEMO_GROUP);
	e->fake.now_ms += 4000u;
	(void)pe_fsm_tick(&e->fsm, NULL, 0);

	ct_check(e->fake.n_frames > 0, "the port advertises unprompted");
	ct_check(!e->fsm.wire_rev.valid,
		 "  with NOTHING learned from any peer");
	count_emitted_groups(&e->fake, (uint16_t)DEMO_GROUP, &match, &other);
	ct_check(match > 0 && other == 0,
		 "  and every frame carries 257 -- read from this node's own "
		 "CLUSTER_AUTHORIZE record, not echoed from a peer");
}

/* ------------------------------------------------------------------ *
 * 2. The frame the real V7.3 VAX discarded: the STACK
 * ------------------------------------------------------------------ */
static void test_vc_stack_carries_our_group(void)
{
	struct env *e = &g_env;
	uint8_t dst_lavc[VMS_ETH_ADDR_LEN];
	uint32_t match = 0, other = 0, len;

	printf("-- the VC STACK answering a peer's START carries 257\n");

	env_init(e, DEMO_GROUP);
	bring_channel_up(e);
	ct_check_eq_u32(pe_fsm_channel_at(&e->fsm, 0)->state, VMS_PE_CH_B4,
			"the channel verified, as it did in the lab");

	fake_pe_clear_frames(&e->fake);
	ovmxa_lavc(dst_lavc);
	len = fake_peer_start(&e->peer, VAXC_SYSID, ovmxa_hw, dst_lavc,
			      0u, 1u, 0u, PEER_CREDITS, e->buf,
			      sizeof(e->buf));
	ct_check(len > 0, "the peer's 0x41 START builds");
	(void)pe_fsm_rx(&e->fsm, e->buf, len);

	ct_check(e->fake.n_frames > 0,
		 "the port answered the peer's START");
	count_emitted_groups(&e->fake, (uint16_t)DEMO_GROUP, &match, &other);
	ct_check(match > 0 && other == 0,
		 "  and every byte it put back on the wire claims group 257 -- "
		 "the 0x0001 here is what a real V7.3 member discarded 803 "
		 "times");
	ct_check_eq_u32(e->fsm.rx_wrong_group, 0,
		 "  and nothing the peer sent was refused: same cluster");
}

/* ------------------------------------------------------------------ *
 * 3. TEETH: change ONLY the configured group, and abs 22 must move
 * ------------------------------------------------------------------ */
static void test_group_is_not_a_constant(void)
{
	struct env *e = &g_env;
	long lab_g, demo_g;

	printf("-- two ports differing only in group emit different abs 22\n");

	env_init(e, LAB_GROUP);
	e->fake.now_ms += 4000u;
	(void)pe_fsm_tick(&e->fsm, NULL, 0);
	lab_g = emitted_group(&e->fake, 0);

	env_init(e, DEMO_GROUP);
	e->fake.now_ms += 4000u;
	(void)pe_fsm_tick(&e->fsm, NULL, 0);
	demo_g = emitted_group(&e->fake, 0);

	ct_check_eq_u32((uint32_t)lab_g, LAB_GROUP,
			"the group-1 port emits 1");
	ct_check_eq_u32((uint32_t)demo_g, DEMO_GROUP,
			"the group-257 port emits 257");
	ct_check(lab_g != demo_g,
		 "  so abs 22 TRACKS the configured group -- re-baking any "
		 "constant there reddens this check");
}

/* ------------------------------------------------------------------ *
 * 4. Somebody else's cluster is ignored, and counted
 * ------------------------------------------------------------------ */
static void test_foreign_group_is_refused(void)
{
	struct env *e = &g_env;
	uint32_t len;

	printf("-- a frame from ANOTHER cluster's group is refused\n");

	env_init(e, DEMO_GROUP);
	e->peer.cluster_group = LAB_GROUP;   /* a group-1 node on the same LAN */

	len = fake_peer_hello(&e->peer, e->mcast, e->mcast, 0xa0u, 0u, 0u,
			      e->buf, sizeof(e->buf));
	ct_check(len > 0, "the foreign node's HELLO builds");
	(void)pe_fsm_rx(&e->fsm, e->buf, len);

	ct_check_eq_u32(e->fsm.rx_wrong_group, 1,
			"it is counted as another cluster's frame");
	ct_check(pe_fsm_channel_at(&e->fsm, 0) == NULL,
		 "  and NO channel was opened to it");
	ct_check_eq_u32(e->fsm.rx_unclassified, 0,
			"  it was not miscounted as unclassifiable -- the "
			"refusal names its real reason");

	/* The control: the same node, moved into OUR cluster, is taken. */
	e->peer.cluster_group = (uint16_t)DEMO_GROUP;
	len = fake_peer_hello(&e->peer, e->mcast, e->mcast, 0xa0u, 0u, 0u,
			      e->buf, sizeof(e->buf));
	(void)pe_fsm_rx(&e->fsm, e->buf, len);
	ct_check(pe_fsm_channel_at(&e->fsm, 0) != NULL,
		 "  while the SAME node in group 257 opens a channel");
	ct_check_eq_u32(e->fsm.rx_wrong_group, 1,
			"  and the refusal count did not move");
}

int main(void)
{
	printf("test_pe_group_on_wire: abs 22 is this node's cluster group "
	       "(rd vms-b34)\n");

	test_own_group_without_any_peer();
	test_vc_stack_carries_our_group();
	test_group_is_not_a_constant();
	test_foreign_group_is_refused();
	return ct_summary("test_pe_group_on_wire");
}
