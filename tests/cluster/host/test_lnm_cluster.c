// SPDX-License-Identifier: GPL-2.0
/*
 * test_lnm_cluster.c - the executive's clusterwide logical-name tables
 * (src/kernel-core/vms_lnm.c, rd vms-94a), compiled unchanged on the host
 * (lnm_shim/) and driven with what real V7.3 members propagated in
 * tests/lab/captures/vms-94a-cwlnm-oracle-20261013. Each step names the DCL
 * command a real VAX ran, and every check reads the arena the way a
 * userspace reader would.
 */
#include <stdio.h>
#include <string.h>
#include "vms_internal.h"
#include "cluster_test.h"

static struct vms_lnm_arena *A;

/* The executive's other cluster members (vms_devtab.c on a real build). */
static uint32_t g_others;
uint32_t vms_lnm_cluster_other_members(void) { return g_others; }

static const struct vms_lnm_entry *find(uint32_t table, uint32_t key,
					const char *name, uint8_t mode)
{
	uint32_t i;

	for (i = 0; i < A->max_entries; i++) {
		const struct vms_lnm_entry *e = &A->entries[i];

		if (e->in_use && e->table == table && e->scope_key == key &&
		    e->acmode == mode && !strcmp(e->name, name))
			return e;
	}
	return NULL;
}

static uint32_t table_id(const char *tab)
{
	uint32_t i;

	for (i = 0; i < A->max_entries; i++) {
		const struct vms_lnm_entry *e = &A->entries[i];

		if (e->in_use && e->table == VMS_LNM_TBL_CLUSTER_DIR &&
		    !strcmp(e->name, tab))
			return e->scope_key;
	}
	return 0;
}

static struct vms_lnm_equiv eq[VMS_LNM_MAX_EQUIV];

static void set_eq(int i, const char *v, uint32_t trans_attr)
{
	memset(&eq[i], 0, sizeof(eq[i]));
	eq[i].length = (uint16_t)strlen(v);
	eq[i].pad = (uint8_t)(trans_attr >> 8);
	strcpy(eq[i].value, v);
}

int main(void)
{
	const struct vms_lnm_entry *e;
	uint32_t sys, cl, ct;

	printf("test_lnm_cluster: the executive's clusterwide tables (rd vms-94a)\n");
	ct_check(vms_lnm_init() == 0, "the arena and its built-in cluster tables");
	A = (struct vms_lnm_arena *)vms_lnm_arena_base();
	cl = table_id("LNM$CLUSTER_TABLE");
	sys = table_id("LNM$SYSCLUSTER_TABLE");
	ct_check(cl != 0u && sys != 0u && cl != sys,
		 "LNM$CLUSTER_TABLE and LNM$SYSCLUSTER_TABLE exist from init");

	/* D1: DEFINE/TABLE=LNM$SYSCLUSTER OVMXT1 "A" */
	set_eq(0, "A", 0);
	ct_check_eq_u32(vms_lnm_cluster_crelnm("LNM$SYSCLUSTER_TABLE", "OVMXT1",
					       0, 2, 1, eq), SS__NORMAL,
			"D1 is created");
	e = find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT1", 2);
	ct_check(e && e->num_equiv == 1 && !strcmp(e->equiv[0].value, "A"),
		 "  OVMXT1 [super] = \"A\" in LNM$SYSCLUSTER_TABLE");

	/* D3: three equivalences */
	set_eq(0, "X", 0); set_eq(1, "YY", 0); set_eq(2, "ZZZ", 0);
	ct_check_eq_u32(vms_lnm_cluster_crelnm("LNM$SYSCLUSTER_TABLE", "OVMXT3",
					       0, 2, 3, eq), SS__NORMAL,
			"D3 is created");
	e = find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT3", 2);
	ct_check(e && e->num_equiv == 3 && !strcmp(e->equiv[2].value, "ZZZ") &&
		 e->equiv[1].index == 1, "  three equivalences, indexed");

	/* D4: /EXECUTIVE_MODE/TRANSLATION=CONCEALED */
	set_eq(0, "DUA0:[A.]", 0x100);
	ct_check_eq_u32(vms_lnm_cluster_crelnm("LNM$SYSCLUSTER_TABLE", "OVMXT4",
					       0, 1, 1, eq), SS__NORMAL,
			"D4 is created");
	e = find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT4", 1);
	ct_check(e && (e->attributes & 0x100u) && e->equiv[0].pad == 1u,
		 "  [exec], concealed (name and equivalence)");

	/* D5: supersede D1 */
	set_eq(0, "B", 0);
	ct_check_eq_u32(vms_lnm_cluster_crelnm("LNM$SYSCLUSTER_TABLE", "OVMXT1",
					       0, 2, 1, eq), SS__SUPERSEDE,
			"D5 supersedes OVMXT1 (SS$_SUPERSEDE)");
	e = find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT1", 2);
	ct_check(e && !strcmp(e->equiv[0].value, "B"), "  now \"B\"");

	/* D6: DEASSIGN OVMXT3 */
	ct_check_eq_u32(vms_lnm_cluster_dellnm("LNM$SYSCLUSTER_TABLE", "OVMXT3",
					       2), SS__NORMAL, "D6 deletes OVMXT3");
	ct_check(find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT3", 2) == NULL, "  gone");
	ct_check_eq_u32(vms_lnm_cluster_dellnm("LNM$SYSCLUSTER_TABLE", "OVMXT3",
					       2), SS__NOLOGNAM,
			"  a second delete: SS$_NOLOGNAM");

	/* D7: NO_ALIAS; P1 terminal, P2 concealed, P3 */
	set_eq(0, "P1", 0x200); set_eq(1, "P2", 0x100); set_eq(2, "P3", 0);
	ct_check_eq_u32(vms_lnm_cluster_crelnm("LNM$SYSCLUSTER_TABLE", "OVMXT5",
					       1, 2, 3, eq), SS__NORMAL,
			"D7 is created");
	e = find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT5", 2);
	ct_check(e && (e->attributes & 1u) && e->equiv[0].pad == 2u &&
		 e->equiv[1].pad == 1u && e->equiv[2].pad == 0u,
		 "  NO_ALIAS on the name; each equivalence keeps its own");

	/* D8: CREATE/NAME_TABLE/PARENT=LNM$CLUSTER_TABLE OVMX_CTAB, then D9c */
	ct_check_eq_u32(vms_lnm_cluster_crelnt("OVMX_CTAB", "LNM$CLUSTER_TABLE",
					       0x01000000u, 2, 0x8000ee00u),
			SS__NORMAL, "D8 creates the table OVMX_CTAB");
	ct = table_id("OVMX_CTAB");
	ct_check(ct != 0u && ct != sys && ct != cl, "  with an id of its own");
	ct_check_eq_u32(vms_lnm_cluster_crelnt("OVMX_CTAB", "LNM$CLUSTER_TABLE",
					       0x01000000u, 2, 0x8000ee00u),
			SS__NORMAL, "  a second CREATE_IF finds it");
	set_eq(0, "IN_CTAB", 0);
	ct_check_eq_u32(vms_lnm_cluster_crelnm("OVMX_CTAB", "OVMXT6", 0, 2, 1,
					       eq), SS__NORMAL,
			"D9c defines OVMXT6 in OVMX_CTAB");
	ct_check(find(VMS_LNM_TBL_CLUSTER, ct, "OVMXT6", 2) != NULL &&
		 find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT6", 2) == NULL,
		 "  there, and not in LNM$SYSCLUSTER_TABLE");
	ct_check_eq_u32(vms_lnm_cluster_crelnt("NOPARENT_TAB", "NO$SUCH_TABLE",
					       0, 2, 0), SS__NOLOGTAB,
			"a table under a parent this node does not hold: NOLOGTAB");
	ct_check_eq_u32(vms_lnm_cluster_crelnm("NO$SUCH_TABLE", "X", 0, 2, 1,
					       eq), SS__NOLOGTAB,
			"a name in a table this node does not hold: NOLOGTAB");

	/* D10 / D11: an exec-mode delete, then DEASSIGN/ALL at super */
	ct_check_eq_u32(vms_lnm_cluster_dellnm("LNM$SYSCLUSTER_TABLE", "OVMXT4",
					       1), SS__NORMAL,
			"D10 deletes the exec-mode OVMXT4");
	ct_check_eq_u32(vms_lnm_cluster_dellnm("LNM$SYSCLUSTER_TABLE", NULL, 2),
			SS__NORMAL, "D11 deletes every super-mode name");
	ct_check(find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT1", 2) == NULL &&
		 find(VMS_LNM_TBL_CLUSTER, sys, "OVMXT5", 2) == NULL,
		 "  LNM$SYSCLUSTER_TABLE is empty, as SHOW LOGICAL said on VAX1");
	ct_check(find(VMS_LNM_TBL_CLUSTER, ct, "OVMXT6", 2) != NULL,
		 "  and OVMX_CTAB's name is untouched");
	ct_check(A->generation % 2u == 0u, "the arena is not mid-write");

	/* LOCAL $CRELNM/$DELLNM through the define/delete ioctls (rd vms-94a). */
	{
		struct vms_proc p;
		struct vms_lnm_def_args d;
		struct vms_lnm_del_args x;

		memset(&p, 0, sizeof(p));
		p.vms_pid = 0x20400010u;
		p.cur_privs = VMS_PRV_M_SYSNAM;
		memset(&d, 0, sizeof(d));
		d.table = VMS_LNM_TBL_CLUSTER_OF(sys);
		d.acmode = 2;
		d.num_equiv = 1;
		strcpy(d.name, "OVMX_LOCAL");
		d.name_length = 10;
		set_eq(0, "HERE", 0);
		d.equiv[0] = eq[0];
		g_others = 0;
		ct_check(vms_ioctl_lnm_define(&p, (unsigned long)&d) == 0 &&
			 d.status == SS__NORMAL,
			 "alone: a local DEFINE/TABLE=LNM$SYSCLUSTER is made");
		ct_check(find(VMS_LNM_TBL_CLUSTER, sys, "OVMX_LOCAL", 2) != NULL,
			 "  and the executive holds it in LNM$SYSCLUSTER_TABLE");

		g_others = 2;
		d.status = 0;
		strcpy(d.name, "OVMX_SHARED");
		ct_check(vms_ioctl_lnm_define(&p, (unsigned long)&d) == 0 &&
			 d.status == SS__UNSUPPORTED,
			 "*** with other members: refused SS$_UNSUPPORTED, "
			 "never made unannounced ***");
		ct_check(find(VMS_LNM_TBL_CLUSTER, sys, "OVMX_SHARED", 2) == NULL,
			 "  and nothing was written");

		memset(&x, 0, sizeof(x));
		x.table = VMS_LNM_TBL_CLUSTER_OF(sys);
		x.acmode = 2;
		strcpy(x.name, "OVMX_LOCAL");
		ct_check(vms_ioctl_lnm_delete(&p, (unsigned long)&x) == 0 &&
			 x.status == SS__UNSUPPORTED,
			 "  a DEASSIGN likewise");
		g_others = 0;
		ct_check(vms_ioctl_lnm_delete(&p, (unsigned long)&x) == 0 &&
			 x.status == SS__NORMAL &&
			 find(VMS_LNM_TBL_CLUSTER, sys, "OVMX_LOCAL", 2) == NULL,
			 "alone: the DEASSIGN removes it");

		p.cur_privs = 0;
		d.status = 0;
		ct_check(vms_ioctl_lnm_define(&p, (unsigned long)&d) == 0 &&
			 d.status == SS__NOPRIV,
			 "without SYSNAM or SYSPRV: SS$_NOPRIV, as LNM$SYSTEM");
		p.cur_privs = VMS_PRV_M_SYSNAM;
		d.table = VMS_LNM_TBL_CLUSTER_OF(999u);
		d.status = 0;
		ct_check(vms_ioctl_lnm_define(&p, (unsigned long)&d) == 0 &&
			 d.status == SS__BADPARAM,
			 "a clusterwide table id this node does not hold: refused");

		/*
		 * rd vms-94a (ruling 2026-10-14): THIS NODE'S COPY IS SHORT.
		 * A clusterwide database the connection manager pulled and
		 * could not take whole leaves these tables holding less than
		 * the cluster's, and the executive says so -- alone or not.
		 */
		g_others = 0;
		d.table = VMS_LNM_TBL_CLUSTER_OF(sys);
		d.status = 0;
		strcpy(d.name, "OVMX_AFTER_BAD_PULL");
		d.name_length = 19;
		vms_lnm_cluster_desynchronize(1u);   /* CNXMAN_LNM_DB_UNDECODED */
		ct_check_eq_u32(vms_lnm_cluster_unsync(), 1u,
				"the executive records WHY the tables are short");
		/* negctl: lnm-unsync-still-serves */
		ct_check(vms_ioctl_lnm_define(&p, (unsigned long)&d) == 0 &&
			 d.status == SS__UNSUPPORTED,
			 "*** a clusterwide DEFINE on an unsynchronized node is "
			 "refused SS$_UNSUPPORTED, with no other member in "
			 "sight ***");
		ct_check(find(VMS_LNM_TBL_CLUSTER, sys,
			      "OVMX_AFTER_BAD_PULL", 2) == NULL,
			 "  and nothing was written");
		x.status = 0;
		strcpy(x.name, "OVMX_AFTER_BAD_PULL");
		ct_check(vms_ioctl_lnm_delete(&p, (unsigned long)&x) == 0 &&
			 x.status == SS__UNSUPPORTED,
			 "  a DEASSIGN on an unsynchronized node likewise");
		ct_check(A->cluster_unsync == 1u,
			 "  and a userspace reader sees it in the arena header");
		vms_lnm_cluster_synchronized();
		ct_check_eq_u32(vms_lnm_cluster_unsync(), 0u,
				"a pull that DID apply clears it");
		d.status = 0;
		ct_check(vms_ioctl_lnm_define(&p, (unsigned long)&d) == 0 &&
			 d.status == SS__NORMAL,
			 "  and the clusterwide DEFINE is served again");
	}

	vms_lnm_cleanup();
	return ct_summary("test_lnm_cluster");
}
