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

	vms_lnm_cleanup();
	return ct_summary("test_lnm_cluster");
}
