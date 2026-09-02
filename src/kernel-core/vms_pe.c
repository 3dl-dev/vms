/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_pe.c - the LAN port layer's executive glue (PEDRIVER role, PEA0:).
 *
 * FC-P0.1 SCOPE. This item freezes the substrate seam (exec_kbackend.h
 * SS14..SS18) and the inter-layer contracts (vms_pe.h, vms_scs.h, vms_cnxman.h,
 * vms_dlm_scs.h, vms_cluster_snapshot.h, vms_cluster.h). What lands HERE is the
 * port's lifecycle skeleton: enough to PROVE the seam is real -- that both kmods
 * compile against it, that the core reaches the LAN only through SS14, and that
 * a substrate with no binding yet fails honestly instead of pretending. The
 * protocol arrives in the items that own it:
 *
 *   FC-P0.5  the cluster fork context (the thread, the queues, cf_timer_*)
 *   FC-P0.6  the codec (frame classes, typed accessors, the allowlist)
 *   FC-P0.7  HELLO/SOLICIT codec entries
 *   FC-P0.8  the channel FSM (vms_pe_fsm.c: cadence, b2/b3/b4, timeouts)
 *   FC-P0.9  this file's real glue: PEA0: in vms_devtab, the multicast join,
 *            the rx queue into the fork thread, CLUSTER_DIAG_PORT
 *   FC-P1.2  the virtual-circuit FSM
 *
 * WHY THE FUNCTIONS BELOW RETURN SS$_NOSUCHDEV TODAY. Neither substrate has its
 * SS14..SS18 binding yet (FC-P0.2 Linux, FC-P0.3/FC-P0.4 NetBSD), so
 * exec_lan_open() honestly reports that there is no such device. The port does
 * not come up, PEA0: is not created, and no HELLO is emitted -- which is exactly
 * what a node with no cluster interconnect should do. There is no fallback path,
 * here or anywhere: an executive that cannot serve says so (CLAUDE.md Rule 9),
 * and INV-6 forbids a port whose state is not backed by a real device.
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header (<linux/netdevice.h>, <sys/mbuf.h>,
 * <net/if.h> ...). That gate has a negative control that proves it fails on an
 * injected substrate include in this very file.
 */

#include "vms_internal.h"    /* the SS$_ vocabulary + the host's fixed-width types */
#include "exec_kbackend.h"   /* SS14..SS18: the ONLY substrate surface this TU has */
#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_pe.h"

/*
 * One value, two spellings, asserted. EXEC_SS_NOSUCHDEV lets a backend header
 * name the status without pulling a VMS header; SS__NOSUCHDEV is the executive's
 * own definition. They are the same condition, and this assert is what keeps
 * them one lineage instead of two constants that agree by luck.
 */
_Static_assert(EXEC_SS_NOSUCHDEV == SS__NOSUCHDEV,
	       "the seam's SS$_NOSUCHDEV must be the executive's SS$_NOSUCHDEV");

/* The cluster HELLO multicast group, spec SS4(a): AB-00-04-01-<group>, with the
 * group number coming from CLUSTER_AUTHORIZE. Assembled here (not hard-coded
 * whole) because the last two bytes are the operator's configuration, not a
 * constant. */
static void pe_hello_multicast(const struct vms_cluster_params *p, uint8_t mac[6])
{
	mac[0] = 0xab;
	mac[1] = 0x00;
	mac[2] = 0x04;
	mac[3] = 0x01;
	mac[4] = (uint8_t)(p->auth_group & 0xff);
	mac[5] = (uint8_t)((p->auth_group >> 8) & 0xff);
}

/*
 * The unsolicited-receive callback. CONTRACT RULE 1 (exec_kbackend.h): copy,
 * enqueue under the rx-IPL lock, wake the fork thread -- nothing else, ever.
 * FC-P0.5 supplies the pool and the queue and FC-P0.9 wires this in; until then
 * it is never registered, because exec_lan_open never succeeds, so it counts a
 * drop rather than touching state it does not own yet.
 */
static void pe_rx_cb(void *ctx, const uint8_t *frame, uint32_t len)
{
	(void)ctx;
	(void)frame;
	(void)len;
	/* FC-P0.5/FC-P0.9: take an exec_lanbuf_t from the port's pool, copy
	 * `len` bytes into it, push it on the input queue under the rx lock,
	 * exec_cv_signal the fork thread. No allocation, no sleep, no protocol. */
}

int vms_pe_start(struct vms_cluster *cl)
{
	uint8_t mcast[6];
	int status;

	if (!cl)
		return SS__BADPARAM;
	if (cl->params.vaxcluster == 0)
		return SS__NOSUCHDEV;   /* VAXCLUSTER=0: no port, by configuration */

	/* SS14: bind the port to the interface the device table resolved for
	 * ETH0:. ethertype 0x6007 is SCA, the cluster's own protocol. */
	status = exec_lan_open((const char *)cl->ifname, 0x6007u, pe_rx_cb, cl);
	if (status != 0)
		return status;          /* honest: no interconnect, no PEA0: */

	pe_hello_multicast(&cl->params, mcast);
	status = exec_lan_mc_add(mcast);
	if (status != 0) {
		exec_lan_close();
		return status;
	}

	cl->state = VMS_CLUSTER_PORT_UP;
	return SS__NORMAL;
}

void vms_pe_stop(struct vms_cluster *cl)
{
	uint8_t mcast[6];

	if (!cl || cl->state == VMS_CLUSTER_OFF)
		return;

	pe_hello_multicast(&cl->params, mcast);
	(void)exec_lan_mc_del(mcast);
	exec_lan_close();
	cl->state = VMS_CLUSTER_OFF;
}

/*
 * The snapshot. Today the port holds no objects, so the only fields that can be
 * filled are the ones read back from the seam -- and each carries its validity
 * flag, so a reader blanks what the executive does not know instead of printing
 * a zero that looks like an answer (vms_cluster_snapshot.h rule 2).
 */
int vms_pe_snapshot(struct vms_cluster *cl, struct vms_pe_view *out)
{
	uint32_t mtu = 0;
	int link = 0;

	if (!cl || !out)
		return SS__BADPARAM;

	memset(out, 0, sizeof(*out));
	if (cl->state == VMS_CLUSTER_OFF)
		return SS__NOSUCHDEV;

	out->port_open = 1;
	if (exec_lan_hwaddr(out->hwaddr) == 0)
		out->hwaddr_valid = 1;
	if (exec_lan_mtu(&mtu) == 0)
		out->mtu = mtu;
	if (exec_lan_link_up(&link) == 0)
		out->link_up = link ? 1 : 0;

	/* n_channels / n_vcs / the counters stay 0: there are no channel or VC
	 * objects yet (FC-P0.8, FC-P1.2), and 0 is the truthful count of an
	 * empty table -- not a placeholder. */
	return SS__NORMAL;
}

int vms_pe_channel_snapshot(struct vms_cluster *cl, uint32_t index,
			    struct vms_pe_channel_view *out)
{
	if (!cl || !out)
		return SS__BADPARAM;
	(void)index;
	return SS__NOSUCHDEV;   /* no channel objects until FC-P0.8 */
}

int vms_pe_vc_snapshot(struct vms_cluster *cl, uint32_t index,
		       struct vms_pe_vc_view *out)
{
	if (!cl || !out)
		return SS__BADPARAM;
	(void)index;
	return SS__NOSUCHDEV;   /* no circuit objects until FC-P1.2 */
}
