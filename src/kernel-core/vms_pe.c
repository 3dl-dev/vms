/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_pe.c - the LAN port layer's executive glue (PEDRIVER role, PEA0:),
 * FC-P0.9.
 *
 * FC-P0.1 froze the substrate seam (exec_kbackend.h SS14..SS18) and the
 * inter-layer contracts (vms_pe.h, vms_scs.h, vms_cnxman.h, vms_dlm_scs.h,
 * vms_cluster_snapshot.h, vms_cluster.h). FC-P0.5 built the ONE execution
 * context every line of cluster protocol runs in (vms_cluster_fork.{c,h}):
 * a receive queue and a work queue, drained one event at a time by one
 * kernel thread, under one mutex. FC-P0.8 (channels) and FC-P1.2 (virtual
 * circuits) built the pure state machine that context drives
 * (vms_pe_fsm.{c,h}). This file is what turns those three into a running
 * port:
 *
 *   - PEA0: entered in vms_devtab, bound to the SAME interface ETH0: was
 *     discovered on at boot (device-native naming: one discovery, every
 *     consumer binds to the same record).
 *   - the HELLO multicast group joined on that interface.
 *   - the substrate's unsolicited-receive callback, which does exactly what
 *     CONTRACT RULE 1 permits and nothing else: hand the frame to
 *     cf_rx_deliver() (FC-P0.5's copy-enqueue-wake, under the FC-P0.16
 *     receive-level lock) and return.
 *   - the fork thread's two handlers -- one frame at a time into
 *     pe_fsm_rx(), one timer expiry at a time into the right pe_fsm_*
 *     entry point -- which is where the protocol actually RUNS (never at
 *     receive level, never at timer level).
 *   - the HELLO/channel/circuit timers, armed and cancelled through
 *     cf_timer_arm/cf_timer_cancel (FC-P0.5), never a raw substrate timer.
 *   - the CLUSTER_DIAG_PORT snapshot (vms_pe_snapshot/_channel_snapshot/
 *     _vc_snapshot), read under the fork mutex so a reader never observes a
 *     dispatch half-applied.
 *
 * WHAT THIS FILE DOES NOT DO. It builds no frame and decodes none (the
 * codec, FC-P0.6/P0.7/P1.1, owns that); it makes no protocol decision (the
 * FSM, FC-P0.8/P1.2, owns that); and it never reaches the substrate except
 * through exec_kbackend.h SS14/SS17/SS18 and the FC-P0.5 fork API -- no
 * <linux/...>, no <sys/...>, ever (tools/ci/cluster_core_includes_gate.sh).
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header. That gate has a negative control
 * that proves it fails on an injected substrate include in this very file.
 */

#include "vms_internal.h"      /* the SS$_ vocabulary + the host's fixed-width types */
#include "exec_kbackend.h"     /* SS14..SS18: the ONLY substrate surface this TU has */
#include "vms_cluster.h"
#include "vms_cluster_fork.h"  /* FC-P0.5: cf_rx_deliver, cf_timer_*, fork_enter/leave */
#include "vms_cluster_snapshot.h"
#include "vms_pe.h"
#include "vms_pe_fsm.h"

/*
 * One value, two spellings, asserted. EXEC_SS_NOSUCHDEV lets a backend header
 * name the status without pulling a VMS header; SS__NOSUCHDEV is the executive's
 * own definition. They are the same condition, and this assert is what keeps
 * them one lineage instead of two constants that agree by luck.
 */
_Static_assert(EXEC_SS_NOSUCHDEV == SS__NOSUCHDEV,
	       "the seam's SS$_NOSUCHDEV must be the executive's SS$_NOSUCHDEV");

/* ==========================================================================
 * 0. This node's honest hardware class (spec SS4(g) phase 2 abs 88)
 *
 * A compile-time fact, never a guess: whichever of these macros the
 * toolchain defines is genuinely what this binary was built for, so there is
 * no "unknown" case to disclose here the way there is for a value the
 * executive has to LEARN. NetBSD/vax builds VAX; the Linux kmod builds for
 * whatever host arch it targets. Where OpenVMS never ran on the arch (ARM),
 * OVMX reports its own honest label rather than a borrowed VMS one (the
 * ovmx_identity.h IRON RULE: never lie to the metal).
 * ========================================================================== */
#if defined(__vax__)
#define PE_HW_TYPE "VAX "
#elif defined(__alpha__)
#define PE_HW_TYPE "AXP "
#elif defined(__aarch64__)
#define PE_HW_TYPE "ARM "
#elif defined(__x86_64__) || defined(_M_X64)
#define PE_HW_TYPE "X86 "
#else
#define PE_HW_TYPE "UNK "
#endif

/* One (interface, remote system) slot per possible cluster member -- the
 * same bound VMS_CLUB_MAX_CSB the CLUB's own CSB table uses (vms_cluster.h),
 * because a virtual circuit forms with one remote SYSTEM at a time. */
#define PE_MAX_VC_SLOTS VMS_CLUB_MAX_CSB

/*
 * The port's objects. Opaque to every other TU (vms_pe.h says so); this is
 * the ONLY file that may look inside it.
 */
struct vms_pe {
	struct pe_fsm       fsm;
	struct pe_vc        vcs[PE_MAX_VC_SLOTS];
	struct pe_ops       ops;
	struct vms_cluster *cl;

	/*
	 * The port-wide transmit counters vms_pe_view reports (INV-6): counted
	 * HERE, in the one place every layer's frame actually leaves the node
	 * (pe_ops_send below), rather than summed from the FSM's per-channel/
	 * per-circuit counters after the fact.
	 */
	uint32_t tx_frames;
	uint32_t tx_errors;
};

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

/* ==========================================================================
 * 1. struct pe_ops -- the FSM's only door to the world, bound to the real seam
 * ========================================================================== */

/* Transmit ONE complete frame. Every attempt is counted here -- the one
 * place in the whole port a frame actually leaves the node, whichever layer
 * (HELLO cadence, channel verify, VC formation/data/retransmit) built it. */
static int pe_ops_send(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct vms_pe *pe = (struct vms_pe *)ctx;
	int status;

	status = exec_lan_xmit(frame, len);
	if (status == 0)
		pe->tx_frames++;
	else
		pe->tx_errors++;
	return status;
}

/* Timers ride FC-P0.5's cf_timer_* wrappers, never a raw substrate timer:
 * that is the whole point of CF_OWNER_PE existing (vms_cluster_fork.h SS2) --
 * one place enforces CONTRACT RULE 2, and every layer's timer idiom is
 * identical. `which` is this layer's own enum pe_timer, carried in cf_work's
 * arg0 unchanged; `key` (the channel/VC index, or 0 for the port-wide beat)
 * is arg1. */
static void pe_ops_arm_timer(void *ctx, enum pe_timer which, uint32_t key,
			     uint32_t ms)
{
	struct vms_pe *pe = (struct vms_pe *)ctx;

	(void)cf_timer_arm(pe->cl->fork, CF_OWNER_PE, (uint32_t)which, key, ms);
	/* CF_E_NOSLOT is an honest, counted failure inside cf_stats
	 * (FC-P0.5); pe_ops.arm_timer has no return channel of its own to
	 * widen (the contract is frozen), so a caller who needs to know reads
	 * the fork's own stats. */
}

static void pe_ops_cancel_timer(void *ctx, enum pe_timer which, uint32_t key)
{
	struct vms_pe *pe = (struct vms_pe *)ctx;

	cf_timer_cancel(pe->cl->fork, CF_OWNER_PE, (uint32_t)which, key);
}

static uint32_t pe_ops_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)exec_ticks_ms();
}

static uint64_t pe_ops_now_vms(void *ctx)
{
	(void)ctx;
	return exec_time_now_vms();
}

static void pe_ops_log(void *ctx, const char *msg)
{
	(void)ctx;
	if (msg != NULL)
		exec_console_printf("%s", msg);
}

/*
 * alloc/free stay unbound (NULL), exactly as the FC-P0.8/FC-P1.2 host tests
 * and the rung-2 simulator leave them. The FSM owns a fixed scratch buffer
 * (struct pe_fsm.scratch) and a BOUND circuit table (pe->vcs, sized above)
 * and has no business allocating; a NULL function pointer is a harder
 * guarantee of that than any counter, because reaching for one crashes a
 * booted node instead of quietly leaking one.
 */
static void pe_ops_bind(struct vms_pe *pe)
{
	pe->ops.send = pe_ops_send;
	pe->ops.arm_timer = pe_ops_arm_timer;
	pe->ops.cancel_timer = pe_ops_cancel_timer;
	pe->ops.now_ms = pe_ops_now_ms;
	pe->ops.now_vms = pe_ops_now_vms;
	pe->ops.log = pe_ops_log;
	pe->ops.alloc = NULL;
	pe->ops.free = NULL;
	pe->ops.ctx = pe;
}

/* ==========================================================================
 * 2. This node's honest identity (spec SS4(a)/(b), SS4(g) phase 2)
 *
 * Every field is either read off the real interface through the seam, taken
 * from the loaded SYSGEN parameters, or -- where nothing has sourced it yet
 * -- left honestly invalid. `netif` and `mtu` are already known by the time
 * this runs (the caller opened the LAN seam on `netif` first); this function
 * adds nothing that is not real executive state.
 * ========================================================================== */
static void pe_build_identity(struct vms_cluster *cl, const uint8_t mcast[6],
			      struct pe_identity *id)
{
	uint32_t mtu = 0;
	uint32_t nlen;

	memset(id, 0, sizeof(*id));

	if (exec_lan_hwaddr(id->hw_mac) == 0)
		id->hw_mac_valid = 1u;

	nlen = cl->params.scsnode_len;
	if (nlen > VMS_HELLO_NODENAME_MAX)
		nlen = VMS_HELLO_NODENAME_MAX;
	memcpy(id->scsnode, cl->params.scsnode, nlen);
	id->scsnode_len = (uint8_t)nlen;

	memcpy(id->mcast, mcast, 6);
	id->mcast_valid = 1u;

	/*
	 * The largest SCA content this port may put on the wire: the real
	 * interface MTU, further capped by the operator's configured
	 * NISCS_MAX_PKTSZ when one has actually been loaded (0 means it has
	 * not -- FC-P0.10 has not landed SYSGEN_LOAD yet). 0 either way means
	 * "size verification is not attempted", the honest default on a port
	 * whose MTU is unknown (vms_pe_fsm.h SS4).
	 */
	if (exec_lan_mtu(&mtu) == 0 && mtu > 0) {
		id->max_sca_len = (mtu > 0xffffu) ? 0xffffu : (uint16_t)mtu;
		if (cl->params.niscs_max_pktsz != 0 &&
		    cl->params.niscs_max_pktsz + 2u < (uint32_t)id->max_sca_len)
			id->max_sca_len =
				(uint16_t)(cl->params.niscs_max_pktsz + 2u);
	}

	memcpy(id->hw_type, PE_HW_TYPE, VMS_SCS_START_HWTYPE_LEN);
	id->hw_type_valid = 1u;

	/*
	 * DISCLOSED GAPS, not fabrications (INV-6). Two fields this node
	 * genuinely cannot attest to yet, both closed by FC-P0.10
	 * (VMS_IOCTL_SYSGEN_LOAD, not landed):
	 *   - sw_version: the "VMX Vx.y" honest-OS-identity broadcast has no
	 *     source in `struct vms_cluster` today (no version field is
	 *     loaded into cl->params) -- left invalid, zero bytes go out,
	 *     exactly the join_nonce_valid precedent this same struct already
	 *     documents for an unset credential.
	 *   - cluster_credits_valid: cl->params.cluster_credits cannot yet be
	 *     told apart from "SYSGEN never loaded" (there is no loaded-params
	 *     flag today), so this stays unset rather than asserting a
	 *     borrowed vaxcluster check proves it. vc_fill_identity() still
	 *     copies the raw (zero) value -- an honest "no credit granted
	 *     yet", never a placeholder.
	 * Neither gap blocks a channel from forming (SS4(a)/(b) never touch
	 * either field) or a circuit from forming (only incarnation_time_valid
	 * gates that, filled below).
	 */

	id->incarnation_time = exec_time_now_vms();
	id->incarnation_time_valid = 1u;
}

/* ==========================================================================
 * 3. Receive: CONTRACT RULE 1 at the substrate boundary, real dispatch in
 *    the fork thread
 * ========================================================================== */

/*
 * The unsolicited-receive callback exec_lan_open registers. Runs in the
 * substrate's receive context (a Linux softirq, a NetBSD softint) and may do
 * exactly what CONTRACT RULE 1 permits: hand the frame to cf_rx_deliver(),
 * FC-P0.5's copy-enqueue-wake under the FC-P0.16 receive-level lock. `ctx` is
 * the fork context itself (not `struct vms_pe *`): cf_rx_deliver needs
 * nothing else, and this callback touches no protocol state.
 */
static void pe_lan_rx_cb(void *ctx, const uint8_t *frame, uint32_t len)
{
	(void)cf_rx_deliver((struct vms_cluster_fork *)ctx, frame, len);
}

/* The fork thread's frame handler: one frame, one call into the FSM, under
 * the fork mutex (cf_dispatch_one's own contract) -- this is where the
 * protocol actually runs. */
static void pe_rx_handler(void *ctx, const uint8_t *frame, uint32_t len)
{
	struct vms_pe *pe = (struct vms_pe *)ctx;

	if (pe == NULL)
		return;
	(void)pe_fsm_rx(&pe->fsm, frame, len);
}

/* The fork thread's work handler: one timer expiry, routed to the pe_fsm
 * entry point spec-owned for that timer identity (vms_pe_fsm.h SS7/SS8b).
 * FC-P0.9 defines no OTHER PE work kind, so anything else is counted by the
 * fork context itself (cf_stats.work_undeliverable would require a NULL
 * handler, which this is not -- an unrecognised kind here is simply
 * ignored, honestly, rather than guessed at). */
static void pe_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_pe *pe = (struct vms_pe *)ctx;

	if (pe == NULL || w == NULL || w->kind != CF_WORK_TIMER)
		return;

	switch ((enum pe_timer)w->arg0) {
	case PE_TIMER_HELLO:
		/* The port-wide beat: multicast HELLO, every channel's own
		 * tick, every circuit's own tick, then re-arms itself. */
		(void)pe_fsm_tick(&pe->fsm, NULL, 0u);
		break;
	case PE_TIMER_CHANNEL:
		(void)pe_fsm_channel_timer(&pe->fsm, w->arg1);
		break;
	case PE_TIMER_RETRANSMIT:
		pe_fsm_vc_timer(&pe->fsm, w->arg1);
		break;
	case PE_TIMER_VCFAIL:
		pe_fsm_vc_event(&pe->fsm, w->arg1, PE_EV_TIMER_VCFAIL);
		break;
	default:
		break;
	}
}

/* ==========================================================================
 * 4. Lifecycle
 * ========================================================================== */

/* Undo everything vms_pe_start built, in reverse order, for every failure
 * exit past the point the LAN seam opened. `pe` may be NULL (nothing beyond
 * the seam was built yet). */
static void pe_start_unwind(struct vms_cluster *cl, struct vms_pe *pe,
			    const uint8_t mcast[6])
{
	if (cl->fork != NULL) {
		cf_set_rx_handler(cl->fork, NULL, NULL);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_PE, NULL, NULL);
	}
	if (pe != NULL)
		exec_free(pe);
	(void)exec_lan_mc_del(mcast);
	exec_lan_close();
}

int vms_pe_start(struct vms_cluster *cl)
{
	struct vms_pe *pe;
	struct pe_identity id;
	uint8_t mcast[6];
	int status;

	if (!cl)
		return SS__BADPARAM;
	if (cl->params.vaxcluster == 0)
		return SS__NOSUCHDEV;   /* VAXCLUSTER=0: no port, by configuration */
	if (cl->pe != NULL)
		return SS__NORMAL;      /* already up: idempotent */
	if (cl->fork == NULL)
		return SS__NOSUCHDEV;   /* FC-P0.5 must be running first */

	/*
	 * Device-native naming (operator 2026-08-14): PEA0: binds to the SAME
	 * interface ETH0: was discovered on at boot (vms_devtab_probe_nic),
	 * never a second, possibly different, query of the host.
	 */
	status = (int)vms_devtab_eth0_netif((char *)cl->ifname,
					    (uint32_t)sizeof(cl->ifname));
	if (status != SS__NORMAL)
		return status;          /* no NIC: no port, honestly (Rule 9) */

	/* SS14: bind the port to that interface. ethertype 0x6007 is SCA, the
	 * cluster's own protocol. `ctx` is the fork context: the receive
	 * callback needs nothing else (CONTRACT RULE 1). */
	status = exec_lan_open((const char *)cl->ifname, 0x6007u,
			       pe_lan_rx_cb, cl->fork);
	if (status != 0)
		return status;           /* honest: no interconnect, no PEA0: */

	pe_hello_multicast(&cl->params, mcast);
	status = exec_lan_mc_add(mcast);
	if (status != 0) {
		exec_lan_close();
		return status;
	}

	pe = (struct vms_pe *)exec_zalloc(sizeof(*pe));
	if (!pe) {
		pe_start_unwind(cl, NULL, mcast);
		return SS__INSFMEM;
	}
	pe->cl = cl;
	pe_ops_bind(pe);

	pe_build_identity(cl, mcast, &id);
	if (pe_fsm_init(&pe->fsm, &id, cl->params.scssystemid, &pe->ops) != 0 ||
	    !pe->fsm.id.lavc_valid) {
		/* SCSSYSTEMID does not fit the two bytes the wire grounds
		 * (vms_pe_fsm.c pe_fsm_init): the port would emit nothing, so
		 * this is a configuration failure, not a running port. */
		pe_start_unwind(cl, pe, mcast);
		return SS__BADPARAM;
	}
	pe_fsm_bind_vcs(&pe->fsm, pe->vcs, PE_MAX_VC_SLOTS);

	cf_set_rx_handler(cl->fork, pe_rx_handler, pe);
	(void)cf_set_work_handler(cl->fork, CF_OWNER_PE, pe_work_handler, pe);

	status = vms_devtab_add_pea((const char *)cl->ifname);
	if (status != SS__NORMAL) {
		pe_start_unwind(cl, pe, mcast);
		return status;
	}

	pe_fsm_start(&pe->fsm);

	cl->pe = pe;
	cl->state = VMS_CLUSTER_PORT_UP;
	return SS__NORMAL;
}

void vms_pe_stop(struct vms_cluster *cl)
{
	struct vms_pe *pe;
	uint8_t mcast[6];

	if (!cl || cl->pe == NULL)
		return;
	pe = cl->pe;

	/* SS4(O.30): announce the clean leave before tearing anything down --
	 * pe_fsm_shutdown below stops the beat pe_may_send checks. Best
	 * effort: with no identity to send from this is a documented no-op,
	 * never a blocking failure of CLUSTER_STOP. */
	(void)pe_fsm_send_last_gasp(&pe->fsm);
	pe_fsm_shutdown(&pe->fsm);

	if (cl->fork != NULL) {
		cf_set_rx_handler(cl->fork, NULL, NULL);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_PE, NULL, NULL);
	}

	pe_hello_multicast(&cl->params, mcast);
	(void)exec_lan_mc_del(mcast);
	exec_lan_close();

	cl->pe = NULL;
	exec_free(pe);
	cl->state = VMS_CLUSTER_OFF;
}

/* ==========================================================================
 * 4b. THE E9 BRIDGE -- the frozen glue-facing send surface (FC-P2.4)
 *
 * vms_pe.h SS5 froze these names at FC-P0.1 and said in its own doc comment
 * what their bodies must be: "the glue's job is exactly the one-line
 * dereference pe_send_msg(pe, ...) { return pe_vc_send_msg(&pe->fsm, ...); },
 * never a second implementation of the sequencing/envelope logic". They come
 * into existence HERE, with FC-P2.4 -- the first item to have a caller
 * (vms_scs.c) -- because `struct vms_pe` is private to this file and the pure
 * `struct pe_fsm *` twins cannot share these names in one TU (integration
 * note E9).
 *
 * TWO STATUS VOCABULARIES MEET HERE, AND THAT IS THE POINT. The pure FSM
 * returns `enum pe_vc_send_status` (design SS3.2.2 keeps kernel-core cluster
 * headers free of SS$_ definitions); a user-mode reader gets an SS$_ status.
 * pe_send_status() below is the whole mapping, in one place.
 * ========================================================================== */

/*
 * enum pe_vc_send_status -> SS$_. Every target is a status this tree can
 * CITE (src/libvms/include/ssdef.h / vms_internal.h), never a number invented
 * for the occasion -- the same discipline SS__ABORT's own comment in
 * vms_internal.h records for the BGn: driver ("reusing an already-grounded
 * status rather than inventing one this tree cannot cite", CLAUDE.md Rule 8).
 * Named per case rather than folded, so a reader sees which refusal is which:
 *   NOCIRCUIT/RINGFULL  the path to that system cannot carry this right now
 *                       -> SS$_DEVOFFLINE (2692)
 *   NOCREDIT            the peer's window is spent          -> SS$_EXQUOTA
 *   BADFRAME/TOOBIG     the caller handed down something unsendable
 *                       -> SS$_BADPARAM
 *   TXFAIL              the interface refused the frame     -> SS$_ABORT
 */
static uint32_t pe_send_status(int rc)
{
	switch (rc) {
	case PE_VC_SEND_OK:        return SS__NORMAL;
	case PE_VC_SEND_NOCIRCUIT: return SS__DEVOFFLINE;
	case PE_VC_SEND_RINGFULL:  return SS__DEVOFFLINE;
	case PE_VC_SEND_NOCREDIT:  return SS__EXQUOTA;
	case PE_VC_SEND_BADFRAME:  return SS__BADPARAM;
	case PE_VC_SEND_TOOBIG:    return SS__BADPARAM;
	case PE_VC_SEND_TXFAIL:    return SS__ABORT;
	default:                   return SS__ABORT;
	}
}

int pe_send_msg(struct vms_pe *pe, vms_scs_sysid_t dst, vms_conid_t dst_conid,
		const uint8_t *body, uint32_t len)
{
	if (pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;   /* no port: honest, never a fake send */
	return (int)pe_send_status(pe_vc_send_msg(&pe->fsm, dst, dst_conid,
						  body, len));
}

int pe_send_dg(struct vms_pe *pe, vms_scs_sysid_t dst,
	       const uint8_t *body, uint32_t len)
{
	if (pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;
	return (int)pe_send_status(pe_vc_send_dg(&pe->fsm, dst, body, len));
}

int pe_send_frame(struct vms_pe *pe, vms_scs_sysid_t dst,
		  const uint8_t *frame, uint32_t len)
{
	if (pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;
	return (int)pe_send_status(pe_vc_send_frame(&pe->fsm, dst, frame, len));
}

int pe_addr(struct vms_pe *pe, vms_scs_sysid_t dst, struct vms_scs_addr *out)
{
	if (pe == (struct vms_pe *)0 || out == (struct vms_scs_addr *)0)
		return SS__NOSUCHDEV;
	/* pe_vc_addr answers 0 / -1, and its -1 means "no such circuit" --
	 * NOT a partially filled `out`. Passed straight through: the caller's
	 * contract (scs_fsm_ops.addr) is "non-zero means build nothing". */
	return pe_vc_addr(&pe->fsm, dst, out) == 0 ? 0 : SS__DEVOFFLINE;
}

void pe_set_upper(struct vms_pe *pe, const struct pe_upper_ops *upper)
{
	if (pe == (struct vms_pe *)0)
		return;
	pe_fsm_set_upper(&pe->fsm, upper);
}

int pe_incarnation(struct vms_pe *pe, uint32_t *lo, uint32_t *hi)
{
	uint64_t q;

	if (pe == (struct vms_pe *)0 || lo == (uint32_t *)0 ||
	    hi == (uint32_t *)0)
		return SS__BADPARAM;
	/* INV-6: only a value pe_build_identity() really sampled. An invalid
	 * incarnation is reported as absent; the caller may not fall back. */
	if (!pe->fsm.id.incarnation_time_valid)
		return SS__NOSUCHDEV;
	q = pe->fsm.id.incarnation_time;
	*lo = (uint32_t)(q & 0xffffffffu);
	*hi = (uint32_t)((q >> 32) & 0xffffffffu);
	return SS__NORMAL;
}

/* ==========================================================================
 * 5. Snapshot -- the same views CLUSTER_DIAG_PORT hands userland (INV-6)
 * ========================================================================== */

int vms_pe_snapshot(struct vms_cluster *cl, struct vms_pe_view *out)
{
	uint32_t mtu = 0;
	int link = 0;
	struct cf_stats fst;

	if (!cl || !out)
		return SS__BADPARAM;

	memset(out, 0, sizeof(*out));
	if (cl->state == VMS_CLUSTER_OFF || cl->pe == NULL)
		return SS__NOSUCHDEV;

	/* The FSM's own counters and the port-wide send counters, read
	 * together under the fork mutex so a reader never observes a
	 * dispatch half-applied. */
	vms_cluster_fork_enter(cl);
	pe_fsm_view_project(&cl->pe->fsm, out);
	out->max_pktsz = cl->pe->fsm.id.max_sca_len;
	out->tx_frames = cl->pe->tx_frames;
	out->tx_errors = cl->pe->tx_errors;
	vms_cluster_fork_leave(cl);

	/* Live seam reads: the interface's real, current state, not a value
	 * cached at CLUSTER_START. */
	out->port_open = 1u;
	if (exec_lan_hwaddr(out->hwaddr) == 0)
		out->hwaddr_valid = 1u;
	if (exec_lan_mtu(&mtu) == 0)
		out->mtu = mtu;
	if (exec_lan_link_up(&link) == 0)
		out->link_up = link ? 1u : 0u;

	/* rx_drops_nobuf: the fork context's own pool-empty counter
	 * (FC-P0.5) -- the port has no receive pool of its own to ask. */
	if (cl->fork != NULL) {
		cf_stats_get(cl->fork, &fst);
		out->rx_drops_nobuf = (uint32_t)fst.rx_dropped_nobuf;
	}

	return SS__NORMAL;
}

int vms_pe_channel_snapshot(struct vms_cluster *cl, uint32_t index,
			    struct vms_pe_channel_view *out)
{
	struct pe_channel *ch;

	if (!cl || !out)
		return SS__BADPARAM;
	if (cl->pe == NULL)
		return SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	ch = pe_fsm_channel_at(&cl->pe->fsm, index);
	pe_fsm_channel_project(ch, out);
	vms_cluster_fork_leave(cl);

	return (ch != NULL && ch->in_use) ? SS__NORMAL : SS__NOSUCHDEV;
}

int vms_pe_vc_snapshot(struct vms_cluster *cl, uint32_t index,
		       struct vms_pe_vc_view *out)
{
	struct pe_vc *vc;

	if (!cl || !out)
		return SS__BADPARAM;
	if (cl->pe == NULL)
		return SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	vc = pe_fsm_vc_at(&cl->pe->fsm, index);
	pe_fsm_vc_project(&cl->pe->fsm, vc, out);
	vms_cluster_fork_leave(cl);

	return (vc != NULL && vc->in_use) ? SS__NORMAL : SS__NOSUCHDEV;
}
