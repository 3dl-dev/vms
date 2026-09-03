/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_headers_host.c - the rung-1 host compile-smoke for FC-P0.1: proves the
 * "narrow inter-layer headers" (design docs/design-faithful-cluster-executive.md
 * SS3.2.1/SS3.2.2, plan FC-P0.1) are genuinely host-includable.
 *
 * WHAT "HOST-INCLUDABLE" MEANS, PRECISELY. Each of vms_cluster.h,
 * vms_cluster_snapshot.h, vms_pe.h, vms_scs.h, vms_cnxman.h and vms_dlm_scs.h
 * must compile as the ONLY #include in a translation unit, with -DOVMX_CLUSTER_HOST
 * and an include path that names src/kernel-core ALONE -- no src/kernel, no
 * src/kernel-netbsd, no exec_kbackend.h. A header that quietly depends on some
 * OTHER header having been included first (or on vms_internal.h / exec_kbackend.h
 * being reachable) fails to compile in isolation even though it happens to work
 * inside vms_pe.c, where a dozen other includes already dragged in what it needed.
 * That silent-order dependency is exactly the class of bug this test exists to
 * catch (it is what cost FC-P0.1 its host-includability once already: a seam type,
 * exec_mutex_t, was named directly in struct vms_cluster before this test existed).
 *
 * HOW: this file does NOT test that directly -- a header, once included, leaves
 * its include guard defined for the rest of the translation unit, so a second
 * `#include` of the same header later in the SAME .c file proves nothing about
 * whether the header could stand alone. The six hdr_alone_*.c files beside this
 * one each contain EXACTLY ONE #include (plus the OVMX_CLUSTER_HOST define) and
 * are separate translation units for exactly that reason: the compiler parses
 * each one from a blank slate. This file is the aggregate integration smoke (the
 * order a real consumer uses, vms_cluster.h first) plus the sizeof/reference
 * checks that prove the six TUs are not compiling to nothing.
 *
 * This binary needs nothing beyond <stdint.h>/<stddef.h> plus the six pure
 * headers: no kernel headers, no exec_kbackend.h, no linking against vms_lock.c
 * or any kmod object. `ctest -R cluster_host` runs it in well under a second.
 */
/* OVMX_CLUSTER_HOST is supplied by CMakeLists.txt (target_compile_definitions),
 * matching the selection mechanism vms_cluster.h itself documents. */
#include <stdint.h>
#include <stddef.h>

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_pe.h"
#include "vms_scs.h"
#include "vms_cnxman.h"
#include "vms_dlm_scs.h"

#include <stdio.h>

/* One line per header: touch a type it declares so an accidental empty
 * (guard-only) header would fail to compile here, not just "link". */
static int touch_vms_cluster(void)
{
	struct vms_cluster_params p;
	enum vms_cluster_state st = VMS_CLUSTER_OFF;

	(void)st;
	return (int)sizeof(p) + (int)sizeof(struct vms_cluster);
}

static int touch_vms_cluster_snapshot(void)
{
	struct vms_pe_view pv;
	struct vms_club_view cv;

	(void)pv;
	(void)cv;
	return (int)vms_cluster_snapshot_q(1, 0);
}

static int touch_vms_pe(void)
{
	struct pe_ops ops;
	enum pe_event ev = PE_EV_RX_HELLO;

	(void)ops;
	(void)ev;
	return PE_EV__COUNT;
}

static int touch_vms_scs(void)
{
	struct scs_ops ops;
	enum scs_mtype mt = SCS_MTYPE_CON_REQ;

	(void)ops;
	(void)mt;
	return SCS_MTYPE__COUNT;
}

static int touch_vms_cnxman(void)
{
	struct cnxman_transition tr;
	enum cnxman_event ev = CNXMAN_EV_START;

	(void)tr;
	(void)ev;
	return CNXMAN_EV__COUNT;
}

static int touch_vms_dlm_scs(void)
{
	struct dlm_scs_role_ops ops;
	enum dlm_role r = DLM_ROLE_REQUESTER;

	(void)ops;
	(void)r;
	return DLM_ROLE__COUNT;
}

int main(void)
{
	int total = 0;

	total += touch_vms_cluster();
	total += touch_vms_cluster_snapshot();
	total += touch_vms_pe();
	total += touch_vms_scs();
	total += touch_vms_cnxman();
	total += touch_vms_dlm_scs();

	printf("test_headers_host: all six pure cluster headers compiled and linked "
	       "(checksum %d)\n", total);
	return 0;
}
