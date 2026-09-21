/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_glue_group.c - the cluster GROUP NUMBER is REPORTED, never silently
 * defaulted (rd vms-b34, test-ladder rung R1).
 *
 * THE DEFECT, MEASURED. The cluster group number IS the LAVC HELLO multicast
 * address -- AB-00-04-01-<lo>-<hi> (vms_cluster_codec_hello.c
 * vms_cluster_hello_mcast_build) -- so two nodes with different numbers are not
 * on the same cluster's wire at all. It reaches the executive from
 * CLUSTER_AUTHORIZE (`params.auth_group`, with `params.auth_valid` saying a
 * real record was read), and vms_cluster.h's own contract is "`auth_valid` is 0
 * until the record is loaded; the port driver NEVER substitutes a default".
 * `auth_valid` had exactly ONE reader in the whole executive -- a copy in
 * vms_devtab.c -- so nothing distinguished "group 0 because configured" from
 * "group 0 because nothing was", and no surface reported either.
 *
 * On lab-2 vaxlab-4 (2026-09-20, tests/lab/captures/cn2-genesis-vaxlab4-
 * 20260920/) a booted V0.7 release node -- the shipped initramfs carries an
 * EMPTY /etc/ovmx, so no record -- ran a 195 s join window against a real
 * single-node OpenVMS V7.3 VMScluster on the same bridge: 225 frames to
 * AB-00-04-01-00-00 out, 180 to AB-00-04-01-01-01 (group 257) the other way,
 * `channels 0, circuits 0, rx 0`, ZERO records in the executive's own join
 * ring. Every surface read healthy. Staging the cluster's real group and
 * changing nothing else, the same node was MEMBER on the VAX's own SDA CSB
 * inside 30 s.
 *
 * WHAT THIS FILE PINS. vms_pe.c names exec_kbackend.h and the FC-P0.5 fork
 * API, so it is not host-linkable -- the same situation, and the same
 * source-scan shape, test_cnxman_glue.c and test_scs_glue_conn.c already
 * established for exactly this. The scan has teeth: dropping any one of these
 * bindings reddens it.
 *
 * SCOPE, STATED. The port still USES group 0 when nothing is configured,
 * because OVMX has no operator path that authors a CLUSTER_AUTHORIZE record
 * (CLUSTER_CONFIG_LAN.COM authors SCSNODE/SCSSYSTEMID/VOTES and not the
 * group), and refusing would leave the documented two-node procedure unable to
 * form any cluster -- a must-not-skip CI gate
 * (tests/qemu/test_cluster_config_lan_2node_e2e.sh). So the default is now
 * OPENLY DISCLOSED rather than silent, and the refusal lands with the operator
 * path (VMS spells it SYSMAN CONFIGURATION SET CLUSTER_AUTHORIZATION) as its
 * own item.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "vms_cluster_snapshot.h"
#include "vms_cluster_codec_hello.h"

static char glue_src[400000];

static int read_glue(const char *name)
{
	char path[512];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", OVMX_KCORE_DIR, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(glue_src, 1u, sizeof(glue_src) - 1u, f);
	fclose(f);
	glue_src[n] = '\0';
	return 0;
}

static void check_has(const char *needle, const char *what)
{
	ct_check(strstr(glue_src, needle) != NULL, what);
}

/* ==========================================================================
 * 1. The mapping itself, against the real codec: the group IS the address
 * ========================================================================== */
static void test_group_is_the_address(void)
{
	uint8_t mac[VMS_ETH_ADDR_LEN];

	vms_cluster_hello_mcast_build(257u, mac);
	ct_check(mac[0] == 0xabu && mac[1] == 0x00u && mac[2] == 0x04u &&
		 mac[3] == 0x01u && mac[4] == 0x01u && mac[5] == 0x01u,
		 "group 257 is AB-00-04-01-01-01 -- the address the lab's real "
		 "OpenVMS V7.3 cluster is on");

	vms_cluster_hello_mcast_build(0u, mac);
	ct_check(mac[4] == 0x00u && mac[5] == 0x00u,
		 "group 0 is AB-00-04-01-00-00 -- where an unconfigured node "
		 "spent 225 frames talking to nobody");
}

/* ==========================================================================
 * 2. The view carries BOTH facts, and the 48-byte ABI is unchanged
 * ========================================================================== */
static void test_view_carries_the_group(void)
{
	struct vms_pe_view v;

	ct_check_eq_u32((unsigned long)sizeof(struct vms_pe_view), 48,
			"vms_pe_view is still the 48-byte cross-substrate ABI");
	ct_check_eq_u32((unsigned long)offsetof(struct vms_pe_view, mtu), 12,
			"and every field after the reused pad is where it was");

	memset(&v, 0, sizeof(v));
	v.cluster_group = 257u;
	v.cluster_group_valid = 1u;
	ct_check(v.cluster_group == 257u && v.cluster_group_valid == 1u,
		 "the view can say 'group 257, and it was configured'");

	memset(&v, 0, sizeof(v));
	ct_check(v.cluster_group == 0u && v.cluster_group_valid == 0u,
		 "and 'group 0, and nobody chose it' -- the two states that "
		 "were indistinguishable");
}

/* ==========================================================================
 * 3. The BINDINGS, read out of the shipping glue
 * ========================================================================== */
static void test_glue_bindings(void)
{
	if (read_glue("vms_pe.c") != 0) {
		ct_check(0, "could not read vms_pe.c");
		return;
	}

	/* auth_valid is READ, and the answer is the return value. Before
	 * vms-b34 this function ignored it entirely. */
	check_has("return p->auth_valid && p->auth_group != 0u;",
		  "pe_hello_multicast REPORTS whether a real CLUSTER_AUTHORIZE "
		  "group was loaded (auth_valid finally has a reader)");

	/* Both facts are recorded from the parameters the port really opened
	 * with, so a later SYSGEN load cannot make the console disagree with
	 * the wire. */
	check_has("pe->group = cl->params.auth_group;",
		  "the port records the group it opened with");
	check_has("pe->group_valid = (uint8_t)(group_valid ? 1u : 0u);",
		  "and whether anyone configured it");

	/* And it is SAID, once, at port start. */
	check_has("pe_announce_group(pe);",
		  "the port announces the group when it comes up");
	check_has("%PEA0, cluster HELLO multicast group %u",
		  "naming the group when one is configured");
	check_has("no cluster group is configured (CLUSTER_AUTHORIZE)",
		  "and naming the ABSENCE when there is no record");
	check_has("using group 0, which no real cluster is on",
		  "and saying plainly that group 0 is a DEFAULT, not a choice "
		  "-- the silence in its place cost a 195 s lab join window");

	/* The same two facts reach the diagnostics an operator reads. */
	check_has("out->cluster_group = cl->pe->group;",
		  "vms_pe_snapshot reports the group from the port object");
	check_has("out->cluster_group_valid = cl->pe->group_valid;",
		  "and reports whether it was configured");
}

int main(void)
{
	test_group_is_the_address();
	test_view_carries_the_group();
	test_glue_bindings();
	return ct_summary("test_pe_glue_group");
}
