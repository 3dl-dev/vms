/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cluster_api_getsyi.c - the $GETSYI cluster item-code projection
 * (FC-P3.7, test-ladder rung R1).
 *
 * Builds a synthetic `struct vms_cluster` (SYSGEN params + a hand-populated
 * CLUB, exactly as test_cnxman_csb.c and test_cnxman_quorum.c do) and asserts
 * `cluster_api_getsyi_project()` reports CLUSTER_MEMBER/NODES/VOTES/QUORUM/
 * FSYSID/FTIME/NODE_CSID straight off it -- never a frame count, never a
 * default (INV-6, design SS3.5's diagnostics rule).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_quorum.h"
#include "vms_cluster_api.h"

static struct vms_cluster g_cl;

/*
 * ==========================================================================
 * 1. Before CLUSTER_START ran at all: a zeroed struct vms_cluster. Every
 *    field must read as honestly absent/off, never a fabricated member.
 * ==========================================================================
 */
static void test_projection_before_start(void)
{
	struct vms_getsyi_cluster_view v;

	printf("[getsyi] before CLUSTER_START: honestly absent, not fabricated\n");
	memset(&g_cl, 0, sizeof(g_cl));   /* g_cl.state == VMS_CLUSTER_OFF == 0 */

	cluster_api_getsyi_project(&g_cl, &v);
	ct_check(!v.cluster_member, "before start: CLUSTER_MEMBER is false");
	ct_check_eq_u32(v.cluster_nodes, 0, "before start: CLUSTER_NODES 0");
	ct_check_eq_u32(v.cluster_votes, 0, "before start: CLUSTER_VOTES 0 (no SYSGEN yet)");
	ct_check_eq_u32(v.cluster_quorum, 0, "before start: CLUSTER_QUORUM 0 (never recomputed)");
	ct_check(!v.cluster_fsysid_valid, "before start: CLUSTER_FSYSID blank");
	ct_check(!v.cluster_ftime_valid, "before start: CLUSTER_FTIME blank");
	ct_check(!v.node_csid_valid, "before start: NODE_CSID blank (still NEW)");

	/* NULL handling: a NULL cl leaves out untouched (nothing to project);
	 * a NULL out must never crash. */
	cluster_api_getsyi_project(NULL, &v);
	cluster_api_getsyi_project(&g_cl, NULL);
}

/*
 * ==========================================================================
 * 2. A synthetic 3-member CLUB, as if a join + one transition had already
 *    completed: OVMX (VOTES=0, this node), VAX1, VAX2. Every $GETSYI field
 *    FC-P3.7 owns must read straight off the CLUB it was fed.
 * ==========================================================================
 */
static void test_projection_after_join(void)
{
	struct vms_club *club;
	struct vms_csb *local, *vax1;
	struct vms_getsyi_cluster_view v;

	printf("[getsyi] after a join + transition: projects the real CLUB\n");

	memset(&g_cl, 0, sizeof(g_cl));
	g_cl.params.scssystemid = 0x000004000103ull;   /* OVMX's own SCSSYSTEMID */
	memcpy(g_cl.params.scsnode, "OVMX01", 6);
	g_cl.params.scsnode_len = 6;
	g_cl.params.votes = 0;             /* design D-10: non-voting first */
	g_cl.params.expected_votes = 3;
	g_cl.state = VMS_CLUSTER_MEMBER;

	club = cnxman_club_init(&g_cl) != NULL ? &g_cl.club : NULL;
	ct_check(club != NULL, "after-join: club_init succeeded");

	/* This node learns its own CSID by matching its SCSSYSTEMID in the
	 * membership records (vms_cluster.h SS2) -- simulated directly, the
	 * same fact the not-yet-built join FSM (FC-P3.3) will establish. */
	cnxman_club_learn_local_csid(club, 0x00010003u);
	local = cnxman_club_local(club);
	ct_check(local != NULL, "after-join: local CSB exists");
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));

	vax1 = cnxman_club_alloc_csb(club, 0x000004000101ull, 1);
	cnxman_csb_set_csid(vax1, 0x00010001u);
	cnxman_csb_set_params(vax1, 1, 3, 0);
	cnxman_csb_set_flags(vax1, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
	vax1->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;

	{
		struct vms_csb *vax2 = cnxman_club_alloc_csb(club, 0x000004000102ull, 1);
		cnxman_csb_set_csid(vax2, 0x00010002u);
		cnxman_csb_set_params(vax2, 1, 3, 0);
		cnxman_csb_set_flags(vax2, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
		vax2->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	}

	cnxman_club_recount_members(club);   /* FC-P3.6's own CLUSTER_NODES count */
	cnxman_quorum_recompute(club);       /* FC-P3.7's CEVOTES/QUORUM */

	club->fsysid = 0x000004000101ull;    /* VAX1 founded the cluster */
	club->fsysid_valid = 1u;
	club->ftime = 0x0102030405060708ull;
	club->ftime_valid = 1u;

	cluster_api_getsyi_project(&g_cl, &v);

	ct_check(v.cluster_member != 0, "after-join: CLUSTER_MEMBER true");
	ct_check_eq_u32(v.cluster_nodes, 3, "after-join: CLUSTER_NODES 3 (OVMX+VAX1+VAX2)");
	ct_check_eq_u32(v.cluster_votes, 0, "after-join: CLUSTER_VOTES 0 (OVMX is non-voting)");
	ct_check_eq_u32(v.cluster_quorum, club->quorum,
			"after-join: CLUSTER_QUORUM matches the CLUB's own quorum field");
	ct_check_eq_u32(club->quorum, 2, "after-join: QUORUM = (2+2)/2 = 2 (VAX1+VAX2 = 2 votes)");

	ct_check(v.cluster_fsysid_valid != 0, "after-join: CLUSTER_FSYSID learned");
	ct_check_eq_u32(v.cluster_fsysid_lo, 0x04000101u,
			"after-join: CLUSTER_FSYSID low word == VAX1's SCSSYSTEMID low word");
	ct_check_eq_u32(v.cluster_fsysid_hi, 0x00000000u,
			"after-join: CLUSTER_FSYSID high word");

	ct_check(v.cluster_ftime_valid != 0, "after-join: CLUSTER_FTIME learned");
	ct_check_eq_u32(v.cluster_ftime_lo, 0x05060708u, "after-join: CLUSTER_FTIME low word");
	ct_check_eq_u32(v.cluster_ftime_hi, 0x01020304u, "after-join: CLUSTER_FTIME high word");

	ct_check(v.node_csid_valid != 0, "after-join: NODE_CSID learned");
	ct_check_eq_u32(v.node_csid, 0x00010003u, "after-join: NODE_CSID == the learned CSID");
}

int main(void)
{
	test_projection_before_start();
	test_projection_after_join();

	return ct_summary("test_cluster_api_getsyi");
}
