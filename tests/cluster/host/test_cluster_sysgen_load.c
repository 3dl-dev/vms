/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cluster_sysgen_load.c - cluster_sysgen_load() (FC-P0.10, test-ladder
 * rung R1).
 *
 * Exercises the pure validate-then-commit body VMS_IOCTL_SYSGEN_LOAD's
 * per-substrate dispatcher calls: a full, real parameter set commits
 * field-for-field into struct vms_cluster.params; the negctl the plan row
 * names (VAXCLUSTER >= 1 with no SCSNODE) is refused and leaves
 * struct vms_cluster.params exactly as it already honestly stood (INV-6 --
 * never a half-written or fabricated identity). The booted-node R4 readback
 * (SYSGEN SHOW, CLUSTER_DIAG on a live node) is deferred to the lab lane.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "vms_cluster.h"
#include "vms_cluster_sysgen.h"

static struct vms_cluster g_cl;

/*
 * A full, real parameter set -- as STARTUP.EXE would build it from
 * SYS$SYSTEM:OVMXVMSSYS.PAR (sysgen_params.h) and CLUSTER_AUTHORIZE.DAT
 * (cluster_authorize.h). Every field is a value a real boot would carry,
 * never a placeholder.
 */
static void fill_valid_params(struct vms_cluster_params *p)
{
	memset(p, 0, sizeof(*p));
	memcpy(p->scsnode, "VAX1", 4);
	p->scsnode_len = 4;
	p->scssystemid = 0x000004000101ull;

	p->votes = 1;
	p->expected_votes = 3;
	p->qdskvotes = 1;
	p->recnxinterval = 20;
	p->timvcfail = 10;
	p->cluster_credits = 10;

	p->vaxcluster = 2;
	p->lockdirwt = 1;
	p->alloclass = 1;
	p->mscp_load = 1;
	p->mscp_serve_all = 0;

	p->niscs_max_pktsz = 1498;

	memcpy(p->disk_quorum, "DUA0", 4);
	p->disk_quorum_len = 4;

	p->auth_group = 42;
	memcpy(p->auth_password, "seekrit", 7);
	p->auth_password_len = 7;
	p->auth_valid = 1;

	/* The identity SSOT's cluster software version, as STARTUP.EXE carries
	 * it down (OVMX_CLUSTER_SW_VERSION -- userland, so this test spells the
	 * shape rather than including it; the SSOT-to-boot-path binding itself
	 * is tests/integration/test_identity_ssot.sh's). */
	memcpy(p->sw_version, "VMX V0.6", 8);
	p->sw_version_len = 8;
}

/*
 * ==========================================================================
 * 1. A full, valid parameter set commits field-for-field.
 * ==========================================================================
 */
static void test_valid_load_commits(void)
{
	struct vms_cluster_params p;
	int ok;

	printf("[sysgen] a real parameter set loads into struct vms_cluster\n");
	memset(&g_cl, 0, sizeof(g_cl));
	fill_valid_params(&p);

	ok = cluster_sysgen_load(&g_cl, &p);
	ct_check(ok != 0, "valid params: cluster_sysgen_load succeeds");
	ct_check(memcmp(&g_cl.params, &p, sizeof(p)) == 0,
	         "valid params: struct vms_cluster.params matches field-for-field");
	ct_check_eq_u32(g_cl.params.scsnode_len, 4, "SCSNODE length loaded");
	ct_check_eq_u32((uint32_t)g_cl.params.scssystemid, 0x04000101u,
	                 "SCSSYSTEMID low word loaded");
	ct_check_eq_u32(g_cl.params.votes, 1, "VOTES loaded");
	ct_check_eq_u32(g_cl.params.expected_votes, 3, "EXPECTED_VOTES loaded");
	ct_check_eq_u32(g_cl.params.vaxcluster, 2, "VAXCLUSTER loaded");
	ct_check_eq_u32(g_cl.params.lockdirwt, 1, "LOCKDIRWT loaded");
	ct_check_eq_u32(g_cl.params.qdskvotes, 1, "QDSKVOTES loaded");
	ct_check_eq_u32(g_cl.params.disk_quorum_len, 4, "DISK_QUORUM length loaded");
	ct_check_eq_u32(g_cl.params.recnxinterval, 20, "RECNXINTERVAL loaded");
	ct_check_eq_u32(g_cl.params.timvcfail, 10, "TIMVCFAIL loaded");
	ct_check_eq_u32(g_cl.params.cluster_credits, 10, "CLUSTER_CREDITS loaded");
	ct_check_eq_u32(g_cl.params.niscs_max_pktsz, 1498, "NISCS_MAX_PKTSZ loaded");
	ct_check_eq_u32(g_cl.params.mscp_load, 1, "MSCP_LOAD loaded");
	ct_check_eq_u32(g_cl.params.mscp_serve_all, 0, "MSCP_SERVE_ALL loaded");
	ct_check_eq_u32(g_cl.params.auth_group, 42, "CLUSTER_AUTHORIZE group loaded");
	ct_check_eq_u32(g_cl.params.auth_valid, 1, "CLUSTER_AUTHORIZE valid flag loaded");
	ct_check_eq_u32(g_cl.params.sw_version_len, 8, "software-version token loaded");
	ct_check(memcmp(g_cl.params.sw_version, "VMX V0.6", 8) == 0,
	         "software-version token loaded VERBATIM, no re-spelling");
	ct_check_eq_u32(g_cl.params_valid, 1,
	                "the COMMIT is what marks the record real (params_valid)");
}

/*
 * ==========================================================================
 * 5. E57: what the port may ADVERTISE out of the loaded record -- the SCS
 *    START body's abs 72-79 software version and its abs 95 CLUSTER_CREDITS.
 *    Both zeros on the wire were what made VAX1 render OVMXJ1's software as
 *    "?" and form no CSB. Neither has a default: with nothing loaded the
 *    accessor says so and the port advertises an honest zero (INV-6).
 * ==========================================================================
 */
static void test_advertised_identity_from_loaded_params(void)
{
	struct vms_cluster_params p;
	uint8_t swver[VMS_CLUSTER_SWVER_LEN];
	uint8_t credits;

	printf("[sysgen] E57: the advertised identity is READ from the commit\n");

	/* (a) Never loaded: nothing to assert, and NOT a fabricated default. */
	memset(&g_cl, 0, sizeof(g_cl));
	memset(swver, 0xee, sizeof(swver));
	credits = 0xee;
	ct_check(cluster_sysgen_sw_version(&g_cl, swver) == 0,
	         "no SYSGEN load: no software version to advertise");
	ct_check(swver[0] == 0xee,
	         "no SYSGEN load: the caller's buffer is left UNTOUCHED");
	ct_check(cluster_sysgen_credits(&g_cl, &credits) == 0,
	         "no SYSGEN load: no credit grant to advertise");
	ct_check(credits == 0xee,
	         "no SYSGEN load: the caller's credit byte is left UNTOUCHED");

	/* (b) A real boot's record: both come back, byte-exact. */
	fill_valid_params(&p);
	ct_check(cluster_sysgen_load(&g_cl, &p) != 0, "the record commits");
	ct_check(cluster_sysgen_sw_version(&g_cl, swver) == 1,
	         "loaded: a software version IS advertised");
	ct_check(memcmp(swver, "VMX V0.6", 8) == 0,
	         "loaded: OUR OWN version, verbatim -- never a peer's 'VMS V7.3'");
	ct_check(cluster_sysgen_credits(&g_cl, &credits) == 1,
	         "loaded: a credit grant IS advertised");
	ct_check_eq_u32(credits, 10, "loaded: CLUSTER_CREDITS 10 at abs 95");

	/* (c) A shorter token is BLANK-padded to the fixed field, the way the
	 * node name is -- not NUL-padded, and not truncated. */
	fill_valid_params(&p);
	memset(p.sw_version, 0, sizeof(p.sw_version));
	memcpy(p.sw_version, "VMX V1", 6);
	p.sw_version_len = 6;
	memset(&g_cl, 0, sizeof(g_cl));
	ct_check(cluster_sysgen_load(&g_cl, &p) != 0, "the short-token record commits");
	ct_check(cluster_sysgen_sw_version(&g_cl, swver) == 1, "and is advertised");
	ct_check(memcmp(swver, "VMX V1  ", 8) == 0,
	         "a short token is blank-padded to the fixed 8-byte field");

	/* (d) A loaded record with NO token: still nothing to assert. A boot
	 * that never carried the identity SSOT down does not get a guess. */
	fill_valid_params(&p);
	memset(p.sw_version, 0, sizeof(p.sw_version));
	p.sw_version_len = 0;
	memset(&g_cl, 0, sizeof(g_cl));
	ct_check(cluster_sysgen_load(&g_cl, &p) != 0, "the token-less record commits");
	ct_check(cluster_sysgen_sw_version(&g_cl, swver) == 0,
	         "no token carried down: no software version advertised");

	/* (e) CLUSTER_CREDITS 0 is a REAL configuration -- a grant of nothing --
	 * and is asserted; only "never loaded" is the absence. That distinction
	 * is exactly what params_valid exists for. */
	fill_valid_params(&p);
	p.cluster_credits = 0;
	memset(&g_cl, 0, sizeof(g_cl));
	credits = 0xee;
	ct_check(cluster_sysgen_load(&g_cl, &p) != 0, "the zero-credit record commits");
	ct_check(cluster_sysgen_credits(&g_cl, &credits) == 1,
	         "a configured 0 IS a grant and is advertised");
	ct_check_eq_u32(credits, 0, "and it is 0, not a substituted default");

	/* (f) A credit the one-byte field cannot express is omitted, not
	 * truncated: 256 would become 0, a window this node never configured. */
	fill_valid_params(&p);
	p.cluster_credits = 256;
	memset(&g_cl, 0, sizeof(g_cl));
	credits = 0xee;
	ct_check(cluster_sysgen_load(&g_cl, &p) != 0, "the over-wide record commits");
	ct_check(cluster_sysgen_credits(&g_cl, &credits) == 0,
	         "CLUSTER_CREDITS > 255 is not truncated onto the wire");
	ct_check(credits == 0xee, "and the caller's byte is left untouched");

	/* (g) NULLs. */
	ct_check(cluster_sysgen_sw_version(NULL, swver) == 0, "NULL cl refused");
	ct_check(cluster_sysgen_sw_version(&g_cl, NULL) == 0, "NULL out refused");
	ct_check(cluster_sysgen_credits(NULL, &credits) == 0, "NULL cl refused");
	ct_check(cluster_sysgen_credits(&g_cl, NULL) == 0, "NULL out refused");
}

/*
 * ==========================================================================
 * 2. The negctl this item's plan row names: VAXCLUSTER=2 (always) with no
 *    SCSNODE loaded is refused, SS$_BADPARAM at the ioctl layer -- and here,
 *    at the pure layer, a 0 return with struct vms_cluster.params left
 *    UNTOUCHED (still whatever it honestly held before this call).
 * ==========================================================================
 */
static void test_missing_scsnode_vaxcluster2_refused(void)
{
	struct vms_cluster_params before, bad;
	int ok;

	printf("[sysgen] negctl: VAXCLUSTER=2 with no SCSNODE -> refused, INV-6\n");
	memset(&g_cl, 0, sizeof(g_cl));
	fill_valid_params(&before);
	g_cl.params = before;   /* the node already has a real, prior identity */

	memset(&bad, 0, sizeof(bad));
	bad.vaxcluster = 2;      /* always a cluster member ... */
	bad.scsnode_len = 0;     /* ... but no SCSNODE was ever loaded */

	ok = cluster_sysgen_load(&g_cl, &bad);
	ct_check(ok == 0, "missing SCSNODE + VAXCLUSTER=2: cluster_sysgen_load refuses");
	ct_check(memcmp(&g_cl.params, &before, sizeof(before)) == 0,
	         "missing SCSNODE + VAXCLUSTER=2: prior params left UNCHANGED, not fabricated");
	ct_check_eq_u32(g_cl.params_valid, 0,
	                "a REFUSED load never marks the record committed");
}

/*
 * ==========================================================================
 * 3. VAXCLUSTER=0 never attempts to join: an absent SCSNODE in that mode is
 *    not the negctl -- SYSGEN was simply never asked to load a cluster
 *    identity, so the load still succeeds (vms_cluster.h section 2).
 * ==========================================================================
 */
static void test_vaxcluster_zero_no_scsnode_ok(void)
{
	struct vms_cluster_params p;
	int ok;

	printf("[sysgen] VAXCLUSTER=0 with no SCSNODE: not the negctl, load succeeds\n");
	memset(&g_cl, 0, sizeof(g_cl));
	memset(&p, 0, sizeof(p));
	p.vaxcluster = 0;
	p.scsnode_len = 0;

	ok = cluster_sysgen_load(&g_cl, &p);
	ct_check(ok != 0, "VAXCLUSTER=0, no SCSNODE: cluster_sysgen_load succeeds");
	ct_check_eq_u32(g_cl.params.vaxcluster, 0, "VAXCLUSTER=0 committed");
}

/*
 * ==========================================================================
 * 4. NULL handling: never crash, never touch a NULL target.
 * ==========================================================================
 */
static void test_null_handling(void)
{
	struct vms_cluster_params p;

	printf("[sysgen] NULL cl/in: refused, no crash\n");
	fill_valid_params(&p);
	memset(&g_cl, 0, sizeof(g_cl));

	ct_check(cluster_sysgen_load(NULL, &p) == 0, "NULL cl refused");
	ct_check(cluster_sysgen_load(&g_cl, NULL) == 0, "NULL in refused");
	ct_check(g_cl.params.scsnode_len == 0,
	         "NULL in: struct vms_cluster.params still untouched");
}

int main(void)
{
	test_valid_load_commits();
	test_advertised_identity_from_loaded_params();
	test_missing_scsnode_vaxcluster2_refused();
	test_vaxcluster_zero_no_scsnode_ok();
	test_null_handling();

	return ct_summary("test_cluster_sysgen_load");
}
