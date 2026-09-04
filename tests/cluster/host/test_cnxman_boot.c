/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_boot.c - FC-P3.9's R1: BOOT INTEGRATION and the STRAWMAN
 * RETIREMENT.
 *
 * FC-P3.9 wires four things and deletes one. The four:
 *
 *   (a) CLUSTER_START's join semantics -- it now starts the CONNECTION
 *       MANAGER after the port and SCS, and reports the executive's OWN
 *       cluster state (MEMBER / STANDALONE / JOINING) back to STARTUP.EXE,
 *       BEFORE the system disk is mounted;
 *   (b) peer discovery -- a system SCS has an open circuit to becomes a CSB,
 *       which is what gives the join a target at all (integration note E36);
 *   (c) SHOW CLUSTER and $GETSYI reading the CLUB/CSB table;
 *   (e) CLUSTER_MEMBER_GET re-pointed at that same table (note E35).
 *
 * The deletion is the userspace SCS daemon and every mirror it wrote through.
 *
 * TWO PROOFS, exactly the shape test_cnxman_glue.c (FC-P3.8) established for
 * the same situation -- the glue TUs are not host-linkable (they name
 * exec_kbackend.h and the fork API), but their ALGORITHM is reproducible
 * against the real pure layers and their CONTENT is readable:
 *
 *   1. THE ALGORITHM, against REAL objects. Two decisions FC-P3.9 adds are
 *      pure CLUB reads, and both are reproduced here byte for byte against a
 *      real struct vms_club built by the shipping vms_cnxman_csb.c: "is there
 *      a system to join THROUGH?" (which decides join vs STANDALONE/wait, and
 *      whose absence is why every join used to return NO_TARGET), and "what
 *      does one CSB look like as a CLUSTER_MEMBER_GET row?" (whose whole job
 *      is not to overstate a NEW CSB as a member). The $GETSYI projection is
 *      driven through the SHIPPING cluster_api_getsyi_project() directly --
 *      that one IS host-linkable.
 *
 *   2. THE WIRING ITSELF, read out of the four SHIPPING files that carry it:
 *      vms_cnxman.c, vms_devtab.c, ovmx_init.c and dcl_cmd_show.c. A future
 *      edit that drops the cnxman start, un-repoints the member table,
 *      reorders the boot step past the mount, or gives SHOW CLUSTER a second
 *      source, reddens this.
 *
 * INV-6 / E30 THROUGHOUT: the honest outcome pinned here is that a node with
 * a DISCOVERED peer and no assigned CSID reads NEW -- not MEMBER -- and that
 * its member row carries csid 0 with the state word "NEW". A CSB the port
 * merely observed is not a member, and this file fails if it ever claims to
 * be.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "vms_cluster.h"
#include "vms_cluster_api.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"

#define LOCAL_SYSID  0x0000000000000401ull   /* 1025 */
#define PEER_SYSID   0x0000000000000402ull   /* 1026 */

/* ==========================================================================
 * 1. "IS THERE A SYSTEM TO JOIN THROUGH?" -- the E36 decision
 *
 * vms_cnxman.c's cnxman_join_target_present() is this predicate: a CSB that
 * is in use, carries a REAL SCSSYSTEMID, and is not this node's own. It is
 * the same question join_select_target() (FC-P3.3) asks of the CLUB, asked by
 * the glue BEFORE it drives a join -- so a beat with nowhere to join through
 * asks nothing of the FSM at all. (E71 made the FSM's own answer safe either
 * way: a start with no target is DEFERRED in IDLE, not turned into a terminal
 * failure that no later discovery could restart.) Reproduced against a real
 * CLUB.
 * ========================================================================== */
static int target_present(const struct vms_club *club)
{
	uint32_t i;

	for (i = 0; i < club->n_csb; i++) {
		const struct vms_csb *c = &club->csb[i];

		if (c->in_use && c->sysid_valid &&
		    (c->flags & VMS_CSB_F_LOCAL) == 0u)
			return 1;
	}
	return 0;
}

static void club_bring_up(struct vms_cluster *cl)
{
	memset(cl, 0, sizeof(*cl));
	cl->params.scssystemid = LOCAL_SYSID;
	cl->params.scsnode[0] = 'O';
	cl->params.scsnode[1] = 'V';
	cl->params.scsnode[2] = 'M';
	cl->params.scsnode[3] = 'X';
	cl->params.scsnode_len = 4u;
	cl->params.votes = 0u;
	cl->params.expected_votes = 2u;
	(void)cnxman_club_init(cl);
}

static void test_join_target_predicate(void)
{
	struct vms_cluster cl;
	struct vms_csb *peer;

	printf("-- E36: a discovered system is what gives the join a target --\n");

	club_bring_up(&cl);
	ct_check(cnxman_club_local(&cl.club) != NULL,
		 "the CLUB allocates this node's own CSB at start (p. 7-26)");
	ct_check(target_present(&cl.club) == 0,
		 "a CLUB holding ONLY the local CSB has NO join target -- which "
		 "is why an unconditional join returned NO_TARGET");

	/* What the peer sweep does: allocate a CSB for a system the PORT formed
	 * a circuit to. Nothing else is asserted about it. */
	peer = cnxman_club_alloc_csb(&cl.club, PEER_SYSID, 1);
	ct_check(peer != NULL, "a discovered system gets a CSB (p. 7-23)");
	ct_check(peer != NULL && peer->state == (uint8_t)VMS_CNXMAN_CSB_NEW,
		 "... allocated in state NEW -- discovered, not admitted");
	ct_check(peer != NULL && !cnxman_csb_is_member(peer),
		 "... and NOT a member: only a real membership record sets that");
	ct_check(target_present(&cl.club) == 1,
		 "... and NOW there is a system to join through");

	/* The local CSB must never be its own target. */
	ct_check(cnxman_club_local(&cl.club) != NULL &&
		 (cnxman_club_local(&cl.club)->flags & VMS_CSB_F_LOCAL) != 0u,
		 "the local CSB is flagged LOCAL, so the predicate skips it");
}

/* ==========================================================================
 * 2. ONE CSB AS A CLUSTER_MEMBER_GET ROW -- the E35 re-point
 *
 * vms_devtab.c's csb_to_member_row()/csb_member_state_name() reproduced here
 * against real CSBs. The whole job of these two is not to overstate: a NEW
 * CSB renders "NEW" with csid 0, a CSB the cluster admitted renders "MEMBER".
 * ========================================================================== */
static const char *member_state_name(const struct vms_csb *csb)
{
	if (cnxman_csb_is_member(csb))
		return "MEMBER";
	return cnxman_csb_state_name((enum vms_cnxman_csb_state)csb->state);
}

/* A local mirror of the wire row, so this host test needs no ioctl header
 * (src/kernel/vms_ioctl.h is a substrate header and the cluster-core include
 * gate keeps it out of this tree). Field-for-field the same. */
struct vms_cluster_member_row {
	uint32_t csid;
	uint32_t sysid;
	char     scsnode[16];
	char     state[16];
};

static void csb_to_row(const struct vms_csb *csb, struct vms_cluster_member_row *out)
{
	const char *state = member_state_name(csb);
	uint32_t n;

	memset(out, 0, sizeof(*out));
	if (csb->csid_valid)
		out->csid = (uint32_t)csb->csid;
	if (csb->sysid_valid || (csb->flags & VMS_CSB_F_LOCAL))
		out->sysid = (uint32_t)csb->sysid;

	n = csb->scsnode_len;
	if (n > sizeof(out->scsnode) - 1u)
		n = (uint32_t)sizeof(out->scsnode) - 1u;
	memcpy(out->scsnode, csb->scsnode, n);

	n = 0u;
	while (state[n] != '\0' && n < sizeof(out->state) - 1u) {
		out->state[n] = state[n];
		n++;
	}
}

static void test_member_row_projection(void)
{
	struct vms_cluster cl;
	struct vms_csb *peer, *local;
	struct vms_cluster_member_row row;

	printf("\n-- E35: a CSB as a CLUSTER_MEMBER_GET row --\n");

	club_bring_up(&cl);
	local = cnxman_club_local(&cl.club);
	peer = cnxman_club_alloc_csb(&cl.club, PEER_SYSID, 1);
	ct_check(local != NULL && peer != NULL, "bed: local + one discovered CSB");
	if (local == NULL || peer == NULL)
		return;

	csb_to_row(local, &row);
	ct_check(row.sysid == (uint32_t)LOCAL_SYSID,
		 "the local row carries this node's real SCSSYSTEMID");
	ct_check(strcmp(row.scsnode, "OVMX") == 0,
		 "... and the SCSNODE the SYSGEN load committed");
	ct_check(strcmp(row.state, "LOCAL") == 0,
		 "... in state LOCAL (p. 7-24), not MEMBER");
	ct_check(row.csid == 0u,
		 "... with csid 0 -- the cluster has assigned none, and 0 means "
		 "'none assigned', never 'node zero' (E30)");

	csb_to_row(peer, &row);
	ct_check(row.sysid == (uint32_t)PEER_SYSID,
		 "the discovered peer's row carries the sysid the port read");
	ct_check(strcmp(row.state, "NEW") == 0,
		 "... in state NEW -- discovered, not a member");
	ct_check(row.csid == 0u, "... and no CSID (E30, the routine case today)");

	/* Now admit it the ONLY way anything ever does, and watch the column
	 * change -- proving the string is READ, not defaulted. */
	peer->flags |= (uint16_t)VMS_CSB_F_MEMBER;
	peer->csid = 0x00010002u;
	peer->csid_valid = 1u;
	csb_to_row(peer, &row);
	ct_check(strcmp(row.state, "MEMBER") == 0,
		 "a CSB carrying the real p. 7-23 MEMBER flag renders MEMBER");
	ct_check(row.csid == 0x00010002u,
		 "... and its LEARNED CSID appears only once csid_valid is set");
}

/* ==========================================================================
 * 3. $GETSYI, through the SHIPPING projection (FC-P3.7, host-linkable)
 * ========================================================================== */
static void test_getsyi_projection(void)
{
	struct vms_cluster cl;
	struct vms_getsyi_cluster_view v;

	printf("\n-- $GETSYI reads the CLUB, and overstates nothing --\n");

	club_bring_up(&cl);
	cl.state = VMS_CLUSTER_OFF;
	cluster_api_getsyi_project(&cl, &v);
	ct_check(v.cluster_member == 0u,
		 "VAXCLUSTER=0 / no connection manager: CLUSTER_MEMBER is 0 -- "
		 "an ANSWER, which is why the ioctl returns SS$_NORMAL for it");
	ct_check(v.cluster_nodes == 0u,
		 "... CLUSTER_NODES is the CLUB's real count, with no floor at 1");
	ct_check(v.node_csid_valid == 0u,
		 "... NODE_CSID is honestly absent, not 0");

	/* JOINING is the strongest thing the boot path can report without a
	 * membership record; it must still not read as MEMBER. */
	cl.state = VMS_CLUSTER_JOINING;
	cluster_api_getsyi_project(&cl, &v);
	ct_check(v.cluster_member == 0u,
		 "a node that is JOINING is not a member");
	cl.state = VMS_CLUSTER_STANDALONE;
	cluster_api_getsyi_project(&cl, &v);
	ct_check(v.cluster_member == 0u,
		 "a STANDALONE node is not a member");
	cl.state = VMS_CLUSTER_MEMBER;
	cluster_api_getsyi_project(&cl, &v);
	ct_check(v.cluster_member == 1u,
		 "only VMS_CLUSTER_MEMBER reads as a member -- the state phase2 "
		 "alone ever sets, from a real membership record");
}

/* ==========================================================================
 * 4. THE WIRING, read out of the four SHIPPING files
 * ========================================================================== */
static char src_buf[400000];

static int read_src(const char *dir, const char *name)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(src_buf, 1u, sizeof(src_buf) - 1u, f);
	fclose(f);
	src_buf[n] = '\0';
	return 0;
}

static void has(const char *needle, const char *what)
{
	ct_check(strstr(src_buf, needle) != NULL, what);
}

static void absent(const char *needle, const char *what)
{
	ct_check(strstr(src_buf, needle) == NULL, what);
}

static void test_cnxman_wiring(void)
{
	printf("\n-- (a)+(b), read out of src/kernel-core/vms_cnxman.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_cnxman.c") != 0) {
		ct_check(0, "could not open vms_cnxman.c");
		return;
	}

	has("vms_scs_peer_at(cn->cl, i, &sysid)",
	    "E36: the peer sweep asks SCS which systems have an OPEN circuit");
	has("cnxman_club_alloc_csb(&cn->cl->club, sysid, 1)",
	    "... and allocates a CSB for each one it does not already hold");
	has("cnxman_club_find_sysid(&cn->cl->club, sysid) != NULL",
	    "... skipping a system the CLUB already has a block for");
	has("cnxman_discover_peers(cn)",
	    "... and the sweep runs on the reconnect beat, so a member that "
	    "boots later is still found");

	has("if (!cnxman_join_target_present(cn))",
	    "the join is driven ONLY when a real target exists");
	has("(void)cnxman_join_drive(cn);",
	    "E71: ... and it is driven on EVERY reconnect beat, so a join "
	    "released after a connectivity failure is asked again");
	has("cl->state = VMS_CLUSTER_JOINING",
	    "a driven join moves this node to JOINING");
	has("cl->params.vaxcluster == 2u",
	    "VAXCLUSTER=2 is distinguished from 1");
	has("waiting to form or join an "
	    "OpenVMS Cluster",
	    "... and waits with VMS's own OPA0: line");
	has("cl->state = VMS_CLUSTER_STANDALONE",
	    "VAXCLUSTER=1 with no cluster present is STANDALONE");
	absent("cl->state = VMS_CLUSTER_MEMBER",
	       "INV-6: this glue NEVER sets MEMBER -- only phase2 does, from a "
	       "real membership record");

	printf("\n-- (a)+(c)+(e), read out of src/kernel-core/vms_devtab.c --\n");
	if (read_src(OVMX_KCORE_DIR, "vms_devtab.c") != 0) {
		ct_check(0, "could not open vms_devtab.c");
		return;
	}
	has("status = vms_cnxman_start(cl)",
	    "CLUSTER_START starts the CONNECTION MANAGER, not just the port");
	has("args.cluster_state = (uint32_t)cl->state",
	    "... and reports the executive's OWN state, read from the object "
	    "rather than composed from the status");
	has("cnxman_club_csb_at(&cl->club, i)",
	    "E35: CLUSTER_MEMBER_GET walks the CONNECTION MANAGER's CSB table");
	absent("vms_cluster_members[",
	       "... and the module-global mirror the daemon wrote is GONE");
	absent("vms_ioctl_cluster_member_set",
	       "the SET mutator is gone: no userspace path can assert membership");
	absent("vms_ioctl_cluster_member_clear", "the CLEAR mutator is gone too");
	has("cluster_api_getsyi_project(cl, &v)",
	    "$GETSYI's items come from FC-P3.7's ONE projection");

	printf("\n-- (a), read out of the boot path (ovmx_init.c) --\n");
	if (read_src(OVMX_INIT_DIR, "ovmx_init.c") != 0) {
		ct_check(0, "could not open ovmx_init.c");
		return;
	}
	has("vms_kif_cluster_start(&port_up, &state)",
	    "the boot path takes the executive's cluster state back");
	has("report_cluster_state(state)",
	    "... and renders the operator line from THAT, never from the status");
	has("start_cluster_port(cluster_vaxcluster);",
	    "CLUSTER_START is issued at boot Step 2d ...");
	{
		const char *start = strstr(src_buf, "start_cluster_port(cluster_vaxcluster);");
		const char *mount = strstr(src_buf, "run_startup();");
		ct_check(start != NULL && mount != NULL && start < mount,
			 "... BEFORE run_startup(), which is where STARTUP.COM "
			 "and every MOUNT it drives happen -- VMS joins before "
			 "it mounts the system disk");
	}

	printf("\n-- (c), read out of DCL (dcl_cmd_show.c) --\n");
	if (read_src(OVMX_DCL_DIR, "dcl_cmd_show.c") != 0) {
		ct_check(0, "could not open dcl_cmd_show.c");
		return;
	}
	has("vms_kif_cluster_get_members(members, VMS_CLUSTER_MEMBER_MAX",
	    "SHOW CLUSTER's default report reads the executive's member table");
	has("vms_kif_cluster_diag_csb(&a)",
	    "... the CLUSTER class reads the CLUB");
	has("vms_kif_cluster_diag_conn(&a)",
	    "... the CONNECTIONS class reads the SCS CDTs");
	has("vms_kif_cluster_diag_port(&a)",
	    "... and LOCAL_PORTS/CIRCUITS read the port");
	has("OVMX_CLUSTER_SW_VERSION",
	    "the SOFTWARE column comes from the identity SSOT, so the display "
	    "cannot contradict what a peer reads from us");
	absent("OVMX_CLUSTER_STATE_PATH",
	       "and there is no membership FILE left to read");
	has("if (a.club.local_csid_valid)",
	    "an unlearned CSID is BLANKED, never printed as a handle");
	has("if (a.cdt.remote_conid_valid)",
	    "... and so is an unbound remote Con.ID");
}

int main(void)
{
	printf("FC-P3.9 R1: boot integration, the CSB re-point, and the "
	       "shipped wiring\n\n");
	test_join_target_predicate();
	test_member_row_projection();
	test_getsyi_projection();
	test_cnxman_wiring();
	printf("\n");
	return ct_summary("test_cnxman_boot");
}
