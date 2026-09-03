/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_scs.c - System Communication Services' executive glue (FC-P2.4).
 *
 * FC-P2.2 built the PURE SCS state machine (vms_scs_fsm.{c,h}): the SB set,
 * the CDL/CDT connection ladder, the Con.ID allocator, the per-connection
 * credit ledger and the MTYPE dispatch. FC-P2.3 built the SYSAP registry read
 * and the `SCS$DIRECTORY` / `SCS$DIR_LOOKUP` SYSAPs on top of it
 * (vms_scs_dir.{c,h}). Neither can reach the world: every action they emit
 * goes through an injected ops table, and neither owns one byte of storage.
 *
 * THIS FILE IS WHAT TURNS THOSE TWO INTO A RUNNING LAYER, and it is the exact
 * counterpart of what vms_pe.c (FC-P0.9/FC-P1.6) is for the port:
 *
 *   - it OWNS the objects: `struct vms_scs` below holds the scs_fsm, the CDL,
 *     the SB table, the Credit Wait pool, the scs_dir context and its peer /
 *     inquiry tables. The pure layers still allocate nothing.
 *   - it BINDS `struct scs_fsm_ops` DOWNWARD to the port -- three one-line
 *     dereferences (integration note E1's FC-P2.2 addendum): send_ctrl ->
 *     pe_send_frame, send_msg -> pe_send_msg, addr -> pe_addr.
 *   - it BINDS `struct pe_upper_ops` UPWARD from the port: message ->
 *     scs_fsm_rx_message, datagram -> scs_fsm_rx_datagram, vc_up ->
 *     scs_fsm_vc_up and -- the design SS3.2.5 contract, integration note E10 --
 *     vc_down -> scs_fsm_vc_down, whose reason this file maps to the SS$_
 *     status a user-mode reader sees.
 *   - it BINDS `struct scs_dir_ops` ACROSS to the registry and the SCS
 *     services, and REGISTERS `SCS$DIRECTORY` at start, so a real inbound
 *     directory connection from another node routes to the server half and a
 *     local lookup goes out over a real circuit.
 *   - it runs SCS's timers on FC-P0.5's cf_timer_* wrappers under CF_OWNER_SCS
 *     -- never a raw substrate timer, and never protocol at timer level.
 *   - it fills the CLUSTER_DIAG_CONN snapshot (vms_scs_snapshot /
 *     vms_scs_cdt_snapshot) under the fork mutex.
 *
 * WHAT THIS FILE DOES NOT DO. It builds no frame and decodes none (the codec
 * owns that); it makes no protocol decision (the FSM owns that); it keeps no
 * SYSAP name table of its own (there is ONE registry -- integration note E20 --
 * and this file READS it); and it reaches the substrate only through
 * exec_kbackend.h and the FC-P0.5 fork API -- no <linux/...>, no <sys/...>,
 * ever (tools/ci/cluster_core_includes_gate.sh).
 *
 * WHERE THE SS$_ VOCABULARY ENTERS. The pure layers hold none (design
 * SS3.2.2), which is why every one of them answers in its own small enum. The
 * two mapping tables in this file -- scs_glue_status() for
 * `enum scs_fsm_status` and scs_close_status() for `enum scs_close_reason` --
 * are the ONLY place those enums become the numbers userland sees, and each
 * target is a status this tree can CITE. See scs_glue_status()'s comment for
 * the two OpenVMS statuses this tree cannot cite yet and what happens when the
 * lab pins them.
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header.
 */

#include "vms_internal.h"      /* the SS$_ vocabulary + the host's fixed-width types */
#include "exec_kbackend.h"     /* the ONLY substrate surface this TU has */
#include "vms_cluster.h"
#include "vms_cluster_fork.h"  /* FC-P0.5: cf_timer_*, fork_enter/leave */
#include "vms_cluster_snapshot.h"
#include "vms_pe.h"            /* the port services SCS sends through */
#include "vms_scs.h"
#include "vms_scs_fsm.h"
#include "vms_scs_dir.h"

/* ==========================================================================
 * 0. How big the tables are
 *
 * *VAXcluster Principles* p. 2-29/2-30 sizes the CDL as SCSCONNCNT entries
 * plus 200 spares. SCSCONNCNT is a SYSGEN parameter, and it is NOT among the
 * ones VMS_IOCTL_SYSGEN_LOAD carries into struct vms_cluster_params today
 * (the same gap integration note E21 records for SCSFLOWCUSH), so this file
 * cannot size the CDL from the operator's value and does not pretend to.
 * SCS_CDL_ENTRIES below is therefore an OVMX DESIGN VALUE, labelled as one:
 * a fixed bound big enough for every SYSAP this executive registers plus the
 * connections a small VMScluster's members open to it, and small enough to be
 * one allocation on a VAX. Running out is an HONEST, COUNTED refusal
 * (SCS_ERR_NOCDT -> SS$_INSFMEM, scs_fsm.rx_no_cdt / the CONNECT return), never
 * a connection quietly dropped.
 *
 * The SB table is sized to VMS_CLUB_MAX_CSB for the same reason vms_pe.c sizes
 * its circuit table that way: an SB is one REMOTE SYSTEM, and the CLUB's CSB
 * table is what bounds how many of those this node can have.
 * ========================================================================== */
#define SCS_CDL_ENTRIES      128u
#define SCS_SB_SLOTS         VMS_CLUB_MAX_CSB
#define SCS_SENDWAIT_SLOTS    16u
#define SCS_DIR_PEER_SLOTS   VMS_CLUB_MAX_CSB
#define SCS_DIR_INQUIRIES     32u

/*
 * The glue's OWN timer identity, on CF_OWNER_SCS beside the pure FSM's two.
 * `enum scs_timer` is the FSM's vocabulary and stops at SCS_TIMER__COUNT; the
 * directory's tick is not an FSM timer at all (scs_dir_tick expires overdue
 * inquiries and closes a finished transient round), so it takes the first
 * identity past the enum rather than being smuggled into it.
 *
 * SCS_DIR_TICK_MS is an OVMX design value bounded by a real one: an inquiry's
 * deadline is scs_dir_cfg.lookup_timeout_ms (5 s default), so a 1 s beat
 * expires an overdue inquiry within one beat of its deadline and costs one
 * wakeup a second on an idle node.
 */
#define SCS_GLUE_TIMER_DIR_TICK ((uint32_t)SCS_TIMER__COUNT)
#define SCS_DIR_TICK_MS 1000u

/*
 * The SCS layer's objects. Opaque to every other TU (vms_scs.h says so); this
 * is the ONLY file that may look inside it.
 */
struct vms_scs {
	struct scs_fsm         fsm;
	struct scs_cdt         cdl[SCS_CDL_ENTRIES];
	struct scs_sb          sbs[SCS_SB_SLOTS];
	struct scs_sendwait    sw[SCS_SENDWAIT_SLOTS];
	struct scs_fsm_ops     ops;

	struct scs_dir         dir;
	struct scs_dir_ops     dir_ops;
	struct scs_dir_peer    dir_peers[SCS_DIR_PEER_SLOTS];
	struct scs_dir_inquiry dir_inq[SCS_DIR_INQUIRIES];

	struct pe_upper_ops    upper;

	struct vms_cluster    *cl;

	/* Real events this layer counts HERE, because here is where they
	 * happen: the port refusing a frame SCS handed down, and a circuit
	 * break arriving from below (INV-6 -- counted, never inferred). */
	uint32_t tx_port_refused;
	uint32_t vc_downs;

	/* The block-transfer consumers (vms_scs.h SS9; FC-P6.3 introduced the
	 * path, FC-P7.1 widened it to the small table that header explains) and
	 * what actually happened on it -- counted here, where it happens. */
	vms_scs_block_cb block_cb[VMS_SCS_MAX_BLOCK_CONSUMERS];
	void            *block_ctx[VMS_SCS_MAX_BLOCK_CONSUMERS];
	uint32_t         block_completions;
	uint32_t         block_unclaimed;
};

/* ==========================================================================
 * 1. The two status maps -- where the pure enums become SS$_ numbers
 * ========================================================================== */

/*
 * `enum scs_fsm_status` -> SS$_. vms_scs_fsm.h SS2 names the intended OpenVMS
 * status beside each value; where this tree can CITE that status it is used,
 * and where it cannot the nearest ALREADY-GROUNDED one is used and said so
 * (CLAUDE.md Rule 8 -- the same call vms_internal.h's SS__ABORT comment
 * records for the BGn: driver, and SS__DEVNOTMOUNT's for the ACP):
 *
 *   INVAL     -> SS$_BADPARAM   (cited)
 *   NOCONN    -> SS$_BADPARAM   OVMX's choice. vms_scs_fsm.h names
 *                SS$_NOSUCHID, whose only in-tree value (2580,
 *                src/libvms/include/ssdef.h) is the RIGHTS-database
 *                "no such user identifier" -- a different condition, and
 *                asserting it for a connection handle would be a wrong
 *                citation, not a near one. A Con.ID naming no live CDT IS a
 *                bad parameter.
 *   NOTOPEN / NOPATH / PATHLOST -> SS$_DEVOFFLINE. OVMX's choice: OpenVMS's
 *                SS$_INCONSTATE / SS$_NOSUCHNODE / SS$_PATHLOST have no value
 *                anywhere in this tree, and making one up is precisely what
 *                Rule 8 forbids. All three are "the path or the connection
 *                cannot carry this right now", which is what SS$_DEVOFFLINE
 *                (2692, ssdef.h) says. When the lab extracts $SSDEF the way
 *                the $SCSDEF oracle table was extracted, this is a one-line
 *                correction here and in vms_pe.c's pe_send_status().
 *   NOCREDIT  -> SS$_EXQUOTA    (cited; vms_scs_fsm.h's own naming)
 *   NOCDT/NOSB-> SS$_INSFMEM    (cited; vms_scs_fsm.h's own naming)
 *   NOCONID   -> SS$_NOSUCHDEV  the allocator is unseeded, i.e. SCS is not
 *                really up -- the same honest "no such facility" the port
 *                answers with before CLUSTER_START.
 *   NOSYSAP   -> SS$_NOSUCHDEV  nothing is listening on that name here.
 *   BUSY      -> SS$_DEVALLOC   the listening CDT is already holding another
 *                node's connect (cited, and the "already taken" reading this
 *                tree already uses that status for).
 *   TXFAIL/ADDR/CODEC -> SS$_ABORT  the frame did not leave the node.
 */
static uint32_t scs_glue_status(int rc)
{
	switch (rc) {
	case SCS_OK:            return SS__NORMAL;
	case SCS_ERR_INVAL:     return SS__BADPARAM;
	case SCS_ERR_NOCONN:    return SS__BADPARAM;
	case SCS_ERR_NOTOPEN:   return SS__DEVOFFLINE;
	case SCS_ERR_NOCREDIT:  return SS__EXQUOTA;
	case SCS_ERR_NOCDT:     return SS__INSFMEM;
	case SCS_ERR_NOSB:      return SS__INSFMEM;
	case SCS_ERR_NOCONID:   return SS__NOSUCHDEV;
	case SCS_ERR_NOPATH:    return SS__DEVOFFLINE;
	case SCS_ERR_PATHLOST:  return SS__DEVOFFLINE;
	case SCS_ERR_NOSYSAP:   return SS__NOSUCHDEV;
	case SCS_ERR_BUSY:      return SS__DEVALLOC;
	case SCS_ERR_TXFAIL:    return SS__ABORT;
	case SCS_ERR_ADDR:      return SS__ABORT;
	case SCS_ERR_CODEC:     return SS__ABORT;
	default:                return SS__ABORT;
	}
}

/*
 * `enum scs_close_reason` -> SS$_. This is the mapping vms_scs.h's `closed`
 * callback comment names ("On a VC break `reason` maps from SCS_CLOSE_PATHLOST
 * to SS$_PATHLOST in the glue") and design SS3.2.5's path-lost status.
 *
 * IT IS APPLIED WHERE A STATUS IS ACTUALLY RENDERED -- this file's console
 * line and its service returns -- and NOT by rewriting the value handed to a
 * SYSAP's closed(). Every SYSAP in OVMX is an executive component in
 * kernel-core (the directory, CNXMAN, the DLM arm), and design SS3.2.2 keeps
 * kernel-core cluster headers free of SS$_ definitions: pushing an SS$_ number
 * down to them would put the vocabulary in exactly the place the design
 * excludes it from. They receive `enum scs_close_reason`, which is what they
 * can defend. (Reported as a contract-wording divergence for FC-P3.8, which
 * owns the first SYSAP that acts on a reason.)
 */
static uint32_t scs_close_status(uint32_t reason)
{
	switch ((enum scs_close_reason)reason) {
	case SCS_CLOSE_NONE:     return SS__NORMAL;
	case SCS_CLOSE_LOCAL:    return SS__NORMAL;
	case SCS_CLOSE_REMOTE:   return SS__NORMAL;
	case SCS_CLOSE_REJECTED: return SS__ABORT;
	case SCS_CLOSE_TIMEOUT:  return SS__ABORT;
	case SCS_CLOSE_PATHLOST: return SS__DEVOFFLINE;  /* the SS$_PATHLOST slot */
	case SCS_CLOSE_UNLISTEN: return SS__NORMAL;
	default:                 return SS__ABORT;
	}
}

/* ==========================================================================
 * 2. struct scs_fsm_ops -- DOWNWARD, to the port
 *
 * The three sends are the "three one-line bindings" integration note E1
 * describes, and they are three rather than one because the WIRE is two
 * shapes: SCS's connect verbs and its 94-content directory messages are short
 * SCA classes only SCS can build whole (-> the port's FRAME primitive), while
 * an application message is the fixed 190-content class three layers own
 * (-> the port's BODY primitive). `addr` is what stops SCS inventing
 * addressing: the port reads the four real addresses off the circuit's own
 * channel, and a failure here means SCS builds nothing.
 *
 * EACH READS cl->pe FRESH. Nothing here caches a `struct vms_pe *`: if the
 * port has gone (vms_pe_stop), the next send is an honest refusal instead of a
 * dereference of freed memory.
 * ========================================================================== */

static int scs_ops_send_ctrl(void *ctx, vms_scs_sysid_t dst,
			     const uint8_t *frame, uint32_t len)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;
	int status;

	if (scs->cl->pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;
	status = pe_send_frame(scs->cl->pe, dst, frame, len);
	if (status != SS__NORMAL)
		scs->tx_port_refused++;
	return status == SS__NORMAL ? 0 : status;
}

static int scs_ops_send_msg(void *ctx, vms_scs_sysid_t dst,
			    vms_conid_t dst_conid, const uint8_t *body,
			    uint32_t len)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;
	int status;

	if (scs->cl->pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;
	status = pe_send_msg(scs->cl->pe, dst, dst_conid, body, len);
	if (status != SS__NORMAL)
		scs->tx_port_refused++;
	return status == SS__NORMAL ? 0 : status;
}

/*
 * The variable-length twin (FC-P6.3): the SAME port service at a body length
 * the SYSAP chose, for the MSCP end-message classes vms_scs_fsm.h SS1's "THIRD
 * APPLICATION-MESSAGE SHAPE" note grounds. One dereference, same refusal
 * counting -- there is no second send path.
 */
static int scs_ops_send_msg_var(void *ctx, vms_scs_sysid_t dst,
				vms_conid_t dst_conid, const uint8_t *body,
				uint32_t len)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;
	int status;

	if (scs->cl->pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;
	status = pe_send_msg_var(scs->cl->pe, dst, dst_conid, body, len);
	if (status != SS__NORMAL)
		scs->tx_port_refused++;
	return status == SS__NORMAL ? 0 : status;
}

static int scs_ops_addr(void *ctx, vms_scs_sysid_t dst,
			struct vms_scs_addr *out)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;

	if (scs->cl->pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;
	return pe_addr(scs->cl->pe, dst, out) == 0 ? 0 : SS__DEVOFFLINE;
}

/* Timers ride FC-P0.5's cf_timer_* wrappers under CF_OWNER_SCS, never a raw
 * substrate timer -- the same idiom vms_pe.c uses under CF_OWNER_PE, so one
 * place (vms_cluster_fork.c) enforces CONTRACT RULE 2 for every layer. */
static void scs_ops_arm_timer(void *ctx, enum scs_timer which, uint32_t key,
			      uint32_t ms)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;

	(void)cf_timer_arm(scs->cl->fork, CF_OWNER_SCS, (uint32_t)which, key,
			   ms);
	/* CF_E_NOSLOT is an honest, counted failure inside cf_stats;
	 * scs_fsm_ops.arm_timer has no return channel of its own to widen. */
}

static void scs_ops_cancel_timer(void *ctx, enum scs_timer which, uint32_t key)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;

	cf_timer_cancel(scs->cl->fork, CF_OWNER_SCS, (uint32_t)which, key);
}

static uint32_t scs_ops_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)exec_ticks_ms();
}

static void scs_ops_log(void *ctx, const char *msg)
{
	(void)ctx;
	if (msg != (const char *)0)
		exec_console_printf("%s", msg);
}

static void scs_ops_bind(struct vms_scs *scs)
{
	scs->ops.send_ctrl = scs_ops_send_ctrl;
	scs->ops.send_msg = scs_ops_send_msg;
	scs->ops.send_msg_var = scs_ops_send_msg_var;
	scs->ops.addr = scs_ops_addr;
	scs->ops.arm_timer = scs_ops_arm_timer;
	scs->ops.cancel_timer = scs_ops_cancel_timer;
	scs->ops.now_ms = scs_ops_now_ms;
	scs->ops.log = scs_ops_log;
	scs->ops.ctx = scs;
}

/* ==========================================================================
 * 3. struct pe_upper_ops -- UPWARD, from the port
 *
 * The port delivers by (remote system, destination Con.ID); SCS owns the demux
 * from Con.ID to CDT and does it itself, through the CDL (vms_scs_fsm.h SS9).
 * So each of these is one call, and none of them decides anything.
 *
 * NO COPY AND NO STRIP HAPPENS HERE. Design SS3.2.4's receive rule is that the
 * port hands the WHOLE frame upward (vms_pe_fsm.c's vc_deliver does exactly
 * that), and scs_fsm_rx_message parses it through the codec from frame-absolute
 * offsets. The `body` parameter name is pe_upper_ops's own spelling; the bytes
 * are the frame.
 * ========================================================================== */

static void scs_upper_message(void *ctx, vms_scs_sysid_t from,
			      vms_conid_t dst_conid, const uint8_t *frame,
			      uint32_t len)
{
	scs_fsm_rx_message(&((struct vms_scs *)ctx)->fsm, from, dst_conid,
			   frame, len);
}

static void scs_upper_datagram(void *ctx, vms_scs_sysid_t from,
			       const uint8_t *frame, uint32_t len)
{
	scs_fsm_rx_datagram(&((struct vms_scs *)ctx)->fsm, from, frame, len);
}

static void scs_upper_vc_up(void *ctx, vms_scs_sysid_t peer)
{
	scs_fsm_vc_up(&((struct vms_scs *)ctx)->fsm, peer);
}

/*
 * THE VC-BREAK CONTRACT (design SS3.2.5, integration note E10). The whole of
 * SCS's behaviour lives in scs_fsm_vc_down(): every CDT on that SB closes
 * path-lost, the ledgers are discarded, Credit Wait fails, each SYSAP is told
 * -- and NOTHING goes on the wire and nothing is retried. This function adds
 * exactly two things the pure layer cannot: the count, and the SS$_ status a
 * reader sees for the break.
 */
static void scs_upper_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;

	scs->vc_downs++;
	scs_fsm_vc_down(&scs->fsm, peer, reason);
	exec_console_printf("vms: SCS path lost to system %u:%u -> SS$ %u\n",
			    (unsigned)((peer >> 32) & 0xffffffffu),
			    (unsigned)(peer & 0xffffffffu),
			    (unsigned)scs_close_status(
				    (uint32_t)SCS_CLOSE_PATHLOST));
}

/*
 * THE PORT'S THIRD SERVICE, ROUTED (FC-P6.3; the table is FC-P7.1's). A
 * block-transfer completion is not an SCS message: it names a BUFFER, not a
 * Con.ID, and SCS has no CDT to demux it through. So SCS forwards it to every
 * registered consumer and interprets nothing -- each one recognises its own
 * buffer NAME (vms_scs.h SS9, "the fan-out is exact") -- and with none
 * registered the completion is counted and dropped, never guessed at.
 */
static void scs_upper_block_data(void *ctx, vms_scs_sysid_t from, uint32_t name,
				 uint32_t offset, uint32_t len,
				 uint32_t bytes_remaining)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;
	uint32_t i, delivered = 0u;

	(void)from;
	scs->block_completions++;
	for (i = 0; i < VMS_SCS_MAX_BLOCK_CONSUMERS; i++) {
		if (scs->block_cb[i] == (vms_scs_block_cb)0)
			continue;
		scs->block_cb[i](scs->block_ctx[i], name, offset, len,
				 bytes_remaining);
		delivered++;
	}
	if (delivered == 0u)
		scs->block_unclaimed++;
}

static void scs_upper_bind(struct vms_scs *scs)
{
	scs->upper.message = scs_upper_message;
	scs->upper.datagram = scs_upper_datagram;
	scs->upper.vc_up = scs_upper_vc_up;
	scs->upper.vc_down = scs_upper_vc_down;
	scs->upper.ctx = scs;
	/* FC-P6.3 binds the port's THIRD service, exactly as the FC-P2.4 note
	 * here said it would: block-transfer completions are routed to the ONE
	 * registered consumer (vms_scs.h SS9), and with none registered the
	 * forwarder is a no-op -- which is the legitimate NULL case the port
	 * itself documents. */
	scs->upper.block_data = scs_upper_block_data;
}

/* ==========================================================================
 * 4. struct scs_dir_ops -- ACROSS, to the registry and the SCS services
 *
 * Every entry is a one-line dereference into a scs_fsm_* service or the ONE
 * registry read (integration note E20: the directory answers from the SDIR
 * queue LISTEN builds, never from a second name table). The CONNECT entry is
 * the only one with any content, and its content is *VAXcluster Principles*
 * p. 2-51's own name pair: the Process Poller (`SCS$DIR_LOOKUP`) connects TO
 * the Directory Service (`SCS$DIRECTORY`).
 * ========================================================================== */

static int scs_dir_op_lookup(void *ctx, const uint8_t *name,
			     struct scs_sysap_info *out)
{
	return scs_fsm_sysap_lookup(&((struct vms_scs *)ctx)->fsm, name, out);
}

static int scs_dir_op_connect(void *ctx, vms_scs_sysid_t dst,
			      const struct scs_sysap_ops *sysap,
			      uint16_t initial_credits, vms_conid_t *out_conid)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;
	struct scs_connect_args args;

	memset(&args, 0, sizeof(args));
	args.local_name = scs_dir_name_lookup;
	args.remote_name = scs_dir_name_directory;
	args.sysap = sysap;
	args.dst = dst;
	args.initial_credits = initial_credits;
	return scs_fsm_connect(&scs->fsm, &args, out_conid);
}

static int scs_dir_op_send(void *ctx, vms_conid_t conid, const uint8_t *body,
			   uint32_t len)
{
	return scs_fsm_send_msg(&((struct vms_scs *)ctx)->fsm, conid, body, len);
}

static int scs_dir_op_return_credit(void *ctx, vms_conid_t conid, uint16_t n)
{
	return scs_fsm_return_credit(&((struct vms_scs *)ctx)->fsm, conid, n);
}

static int scs_dir_op_disconnect(void *ctx, vms_conid_t conid)
{
	return scs_fsm_disconnect(&((struct vms_scs *)ctx)->fsm, conid);
}

static void scs_dir_ops_bind(struct vms_scs *scs)
{
	scs->dir_ops.sysap_lookup = scs_dir_op_lookup;
	scs->dir_ops.connect = scs_dir_op_connect;
	scs->dir_ops.send = scs_dir_op_send;
	scs->dir_ops.return_credit = scs_dir_op_return_credit;
	scs->dir_ops.disconnect = scs_dir_op_disconnect;
	scs->dir_ops.now_ms = scs_ops_now_ms;
	scs->dir_ops.log = scs_ops_log;
	scs->dir_ops.ctx = scs;
}

/* ==========================================================================
 * 5. The fork thread's work handler (CONTRACT RULE 2: timers RUN here)
 * ========================================================================== */

static void scs_arm_dir_tick(struct vms_scs *scs)
{
	(void)cf_timer_arm(scs->cl->fork, CF_OWNER_SCS,
			   SCS_GLUE_TIMER_DIR_TICK, 0u, SCS_DIR_TICK_MS);
}

/*
 * One timer expiry, routed to the entry point that owns that identity. The
 * FSM's two are per-CDT (`key` is the CDT's CDL index, its own timer_key); the
 * third is this file's directory beat, which re-arms itself exactly as the
 * port's HELLO beat does.
 */
static void scs_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_scs *scs = (struct vms_scs *)ctx;

	if (scs == (struct vms_scs *)0 || w == (const struct cf_work *)0 ||
	    w->kind != CF_WORK_TIMER)
		return;

	switch (w->arg0) {
	case (uint32_t)SCS_TIMER_CONNECT:
		scs_fsm_timer(&scs->fsm, SCS_TIMER_CONNECT, w->arg1);
		break;
	case (uint32_t)SCS_TIMER_DISCONNECT:
		scs_fsm_timer(&scs->fsm, SCS_TIMER_DISCONNECT, w->arg1);
		break;
	case SCS_GLUE_TIMER_DIR_TICK:
		scs_dir_tick(&scs->dir);
		scs_arm_dir_tick(scs);
		break;
	default:
		/* An identity this layer never armed. Ignored honestly rather
		 * than guessed at, exactly as vms_pe.c's handler does. */
		break;
	}
}

/* ==========================================================================
 * 6. Lifecycle
 * ========================================================================== */

/* Bind the storage `struct vms_scs` owns to the pure layers. Each bind is
 * checked: a partially bound FSM would allocate a CDT out of a table it does
 * not have. */
static int scs_bind_tables(struct vms_scs *scs)
{
	if (scs_fsm_init(&scs->fsm, &scs->ops) != SCS_OK)
		return SS__BADPARAM;
	if (scs_fsm_bind_cdl(&scs->fsm, scs->cdl, SCS_CDL_ENTRIES) != SCS_OK)
		return SS__BADPARAM;
	if (scs_fsm_bind_sbs(&scs->fsm, scs->sbs, SCS_SB_SLOTS) != SCS_OK)
		return SS__BADPARAM;
	if (scs_fsm_bind_sendwait(&scs->fsm, scs->sw, SCS_SENDWAIT_SLOTS) !=
	    SCS_OK)
		return SS__BADPARAM;
	if (scs_dir_init(&scs->dir, &scs->dir_ops) != SCS_OK)
		return SS__BADPARAM;
	if (scs_dir_bind_peers(&scs->dir, scs->dir_peers,
			       SCS_DIR_PEER_SLOTS) != SCS_OK)
		return SS__BADPARAM;
	if (scs_dir_bind_inquiries(&scs->dir, scs->dir_inq,
				   SCS_DIR_INQUIRIES) != SCS_OK)
		return SS__BADPARAM;
	return SS__NORMAL;
}

/*
 * Seed the Con.ID allocator from a LIVE per-boot value, which is what
 * vms_scs_fsm.h SS4 requires before one connection can be made -- an unseeded
 * allocator REFUSES rather than minting from 0, because a Con.ID that repeats
 * across incarnations is the shape spec SS4(t) says a real node cannot produce
 * and a placeholder connection identifier is the fabrication that bugchecked a
 * real VAX (INV-6).
 *
 * The value is THE PORT'S OWN INCARNATION QUADWORD (pe_incarnation), so this
 * node has one per-boot number behind both its circuits and its connection
 * identifiers. The low half is folded into the 16-bit seed the allocator takes;
 * a port with no valid incarnation yields NO seed and SCS refuses to start,
 * rather than seeding from a zero.
 */
static int scs_seed_from_port(struct vms_scs *scs)
{
	uint32_t lo = 0u, hi = 0u;
	int status;

	status = pe_incarnation(scs->cl->pe, &lo, &hi);
	if (status != SS__NORMAL)
		return status;
	scs_fsm_seed_conid(&scs->fsm,
			   (uint16_t)((lo ^ (lo >> 16)) & 0xffffu));
	return SS__NORMAL;
}

/*
 * Register `SCS$DIRECTORY` -- the one SYSAP SCS itself owns (p. 2-51:
 * "Technically these are SYSAPs. But they are considered to be
 * responsibilities of the SCS layer"). Until this exists, a member's inbound
 * directory connect has nothing to route to and is answered with a REJECT.
 * The client half (`SCS$DIR_LOOKUP`) is not registered: it LISTENs on nothing
 * -- it is the connect INITIATOR, and its CDT is allocated by CONNECT.
 */
static int scs_register_directory(struct vms_scs *scs)
{
	int rc = scs_fsm_listen(&scs->fsm, scs_dir_name_directory,
				scs_dir_server_ops(&scs->dir),
				(uint16_t)SCS_DIR_CREDITS_DEFAULT);

	return (int)scs_glue_status(rc);
}

/* Undo, in reverse order, everything vms_scs_start built past the allocation. */
static void scs_start_unwind(struct vms_cluster *cl, struct vms_scs *scs)
{
	if (cl->pe != (struct vms_pe *)0)
		pe_set_upper(cl->pe, (const struct pe_upper_ops *)0);
	if (cl->fork != (struct vms_cluster_fork *)0)
		(void)cf_set_work_handler(cl->fork, CF_OWNER_SCS,
					  (cf_work_handler_t)0, (void *)0);
	exec_free(scs);
}

int vms_scs_start(struct vms_cluster *cl)
{
	struct vms_scs *scs;
	int status;

	if (cl == (struct vms_cluster *)0)
		return SS__BADPARAM;
	if (cl->scs != (struct vms_scs *)0)
		return SS__NORMAL;      /* already up: idempotent */
	if (cl->fork == (struct vms_cluster_fork *)0)
		return SS__NOSUCHDEV;   /* FC-P0.5 must be running first */
	if (cl->pe == (struct vms_pe *)0)
		return SS__NOSUCHDEV;   /* no port, no connections (Rule 9) */

	scs = (struct vms_scs *)exec_zalloc(sizeof(*scs));
	if (scs == (struct vms_scs *)0)
		return SS__INSFMEM;
	scs->cl = cl;
	scs_ops_bind(scs);
	scs_dir_ops_bind(scs);
	scs_upper_bind(scs);

	status = scs_bind_tables(scs);
	if (status != SS__NORMAL) {
		exec_free(scs);
		return status;
	}
	status = scs_seed_from_port(scs);
	if (status != SS__NORMAL) {
		exec_free(scs);
		return status;
	}

	(void)cf_set_work_handler(cl->fork, CF_OWNER_SCS, scs_work_handler,
				  scs);
	pe_set_upper(cl->pe, &scs->upper);

	status = scs_register_directory(scs);
	if (status != SS__NORMAL) {
		scs_start_unwind(cl, scs);
		return status;
	}
	scs_arm_dir_tick(scs);

	cl->scs = scs;
	return SS__NORMAL;
}

void vms_scs_stop(struct vms_cluster *cl)
{
	struct vms_scs *scs;

	if (cl == (struct vms_cluster *)0 || cl->scs == (struct vms_scs *)0)
		return;
	scs = cl->scs;

	/* The port stops delivering to a layer that is going away FIRST, so no
	 * frame can arrive mid-teardown. */
	if (cl->pe != (struct vms_pe *)0)
		pe_set_upper(cl->pe, (const struct pe_upper_ops *)0);

	/* Tear every connection down: each SYSAP is told, nothing goes on the
	 * wire (a shutdown is not a dialogue), and each CDT's own timers are
	 * cancelled through ops->cancel_timer as it closes. */
	scs_fsm_stop(&scs->fsm);
	if (cl->fork != (struct vms_cluster_fork *)0) {
		cf_timer_cancel(cl->fork, CF_OWNER_SCS,
				SCS_GLUE_TIMER_DIR_TICK, 0u);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_SCS,
					  (cf_work_handler_t)0, (void *)0);
	}

	cl->scs = (struct vms_scs *)0;
	exec_free(scs);
}

/* ==========================================================================
 * 7. Snapshot -- the same views CLUSTER_DIAG_CONN hands userland (INV-6)
 *
 * Both are PURE PROJECTIONS of real objects, taken under the fork mutex so a
 * reader never observes a dispatch half-applied. Neither adds a field of its
 * own, neither defaults anything, and a CDT the executive does not hold is
 * SS$_NOSUCHDEV with an all-zero row -- never a placeholder connection.
 * ========================================================================== */

int vms_scs_snapshot(struct vms_cluster *cl, struct vms_scs_view *out)
{
	if (cl == (struct vms_cluster *)0 || out == (struct vms_scs_view *)0)
		return SS__BADPARAM;

	memset(out, 0, sizeof(*out));
	if (cl->scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	scs_fsm_view_project(&cl->scs->fsm, out);
	vms_cluster_fork_leave(cl);
	return SS__NORMAL;
}

int vms_scs_cdt_snapshot(struct vms_cluster *cl, uint32_t index,
			 struct vms_scs_cdt_view *out)
{
	struct scs_cdt *cdt;
	int live;

	if (cl == (struct vms_cluster *)0 ||
	    out == (struct vms_scs_cdt_view *)0)
		return SS__BADPARAM;

	memset(out, 0, sizeof(*out));
	if (cl->scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;

	vms_cluster_fork_enter(cl);
	cdt = scs_fsm_cdt_at(&cl->scs->fsm, index);
	live = (cdt != (struct scs_cdt *)0 && cdt->in_use);
	if (live)
		scs_fsm_cdt_project(&cl->scs->fsm, cdt, out);
	vms_cluster_fork_leave(cl);

	return live ? SS__NORMAL : SS__NOSUCHDEV;
}

/*
 * vms_scs_peer_at - vms_scs.h SS8. The `index`-th SB with an OPEN circuit.
 * No fork mutex (see the header): this is a fork-context service, not a
 * snapshot. One dereference plus the vc_up test -- it decides nothing.
 */
/* Withdraw one consumer by its ctx, or every one of them when ctx is NULL. */
static void scs_block_withdraw(struct vms_scs *scs, void *cb_ctx)
{
	uint32_t i;

	for (i = 0; i < VMS_SCS_MAX_BLOCK_CONSUMERS; i++) {
		if (cb_ctx != (void *)0 && scs->block_ctx[i] != cb_ctx)
			continue;
		scs->block_cb[i] = (vms_scs_block_cb)0;
		scs->block_ctx[i] = (void *)0;
	}
}

/*
 * vms_scs_set_block_consumer - vms_scs.h SS9. A small fixed table, keyed on
 * `cb_ctx`: no allocation and no list, because the number of SYSAPs in this
 * executive that own named buffers is a known, small number.
 */
int vms_scs_set_block_consumer(struct vms_cluster *cl, vms_scs_block_cb cb,
			       void *cb_ctx)
{
	struct vms_scs *scs;
	uint32_t i, free_slot = VMS_SCS_MAX_BLOCK_CONSUMERS;

	if (cl == (struct vms_cluster *)0)
		return SS__BADPARAM;
	scs = cl->scs;
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;

	if (cb == (vms_scs_block_cb)0) {
		scs_block_withdraw(scs, cb_ctx);
		return SS__NORMAL;
	}

	for (i = 0; i < VMS_SCS_MAX_BLOCK_CONSUMERS; i++) {
		if (scs->block_ctx[i] == cb_ctx &&
		    scs->block_cb[i] != (vms_scs_block_cb)0) {
			scs->block_cb[i] = cb;   /* re-register the same SYSAP */
			return SS__NORMAL;
		}
		if (scs->block_cb[i] == (vms_scs_block_cb)0 &&
		    free_slot == VMS_SCS_MAX_BLOCK_CONSUMERS)
			free_slot = i;
	}
	if (free_slot == VMS_SCS_MAX_BLOCK_CONSUMERS)
		return SS__EXQUOTA;   /* honest refusal, never an eviction */

	scs->block_cb[free_slot] = cb;
	scs->block_ctx[free_slot] = cb_ctx;
	return SS__NORMAL;
}

int vms_scs_peer_at(struct vms_cluster *cl, uint32_t index,
		    vms_scs_sysid_t *out_sysid)
{
	struct scs_sb *sb;

	if (cl == (struct vms_cluster *)0 ||
	    out_sysid == (vms_scs_sysid_t *)0)
		return SS__BADPARAM;
	if (cl->scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;

	sb = scs_fsm_sb_at(&cl->scs->fsm, index);
	if (sb == (struct scs_sb *)0 || !sb->vc_up)
		return SS__NOSUCHDEV;

	*out_sysid = sb->peer_sysid;
	return SS__NORMAL;
}

/* ==========================================================================
 * 8. The services -- vms_scs.h's names, one dereference each
 *
 * vms_scs.h SS4/SS5/SS6 froze this surface at FC-P0.1 and named the scs_fsm_*
 * twin of every one of them. Each below is that dereference plus the status
 * map; not one of them re-implements a decision, and none of them keeps state.
 *
 * CALLED FROM THE CLUSTER FORK THREAD (a SYSAP's own callback, or CNXMAN's
 * FSM), the same contract vms_pe.h SS5 states for the port services. The two
 * snapshots above are the exception and take the fork mutex themselves.
 * ========================================================================== */

int scs_sysap_listen(struct vms_scs *scs, const uint8_t *name,
		     const struct scs_sysap_ops *ops, uint16_t initial_credits)
{
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_fsm_listen(&scs->fsm, name, ops,
						   initial_credits));
}

int scs_sysap_unlisten(struct vms_scs *scs, const uint8_t *name)
{
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_fsm_unlisten(&scs->fsm, name));
}

/*
 * vms_scs.h's CONNECT names the local SYSAP by NAME and carries neither its
 * callback table nor the credits it extends -- so both are READ FROM THE ONE
 * REGISTRY (scs_fsm_sysap_ops / scs_fsm_sysap_lookup) rather than kept in a
 * second table here (integration note E20). A name nobody registered cannot
 * connect: SS$_NOSUCHDEV, and nothing goes on the wire.
 */
int scs_connect(struct vms_scs *scs, const uint8_t *local_name,
		const uint8_t *remote_name, vms_scs_sysid_t dst,
		const uint8_t *conndata, vms_conid_t *out_conid)
{
	struct scs_connect_args args;
	struct scs_sysap_info info;

	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	if (scs_fsm_sysap_lookup(&scs->fsm, local_name, &info) != SCS_OK)
		return SS__NOSUCHDEV;

	memset(&args, 0, sizeof(args));
	args.local_name = local_name;
	args.remote_name = remote_name;
	args.conndata = conndata;
	args.sysap = scs_fsm_sysap_ops(&scs->fsm, local_name);
	args.dst = dst;
	args.initial_credits = info.initial_credits;
	return (int)scs_glue_status(scs_fsm_connect(&scs->fsm, &args,
						    out_conid));
}

int scs_accept(struct vms_scs *scs, vms_conid_t local_conid)
{
	vms_conid_t conn_conid = 0u;

	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	/* The connection's OWN Con.ID is minted here (ch. 2), and the SYSAP
	 * learns it from opened() -- this frozen entry point has no out
	 * parameter to carry it, and inventing one would change the ABI. */
	return (int)scs_glue_status(scs_fsm_accept(&scs->fsm, local_conid,
						   (const uint8_t *)0,
						   &conn_conid));
}

int scs_reject(struct vms_scs *scs, vms_conid_t local_conid, uint32_t reason)
{
	/* `reason` is the local SYSAP's own reading of why it refused. The
	 * REJECT verb (op 4) has no grounded reason field on the wire (spec
	 * SS4(m)), so it is not sent -- honest omission, never a byte invented
	 * to carry it. */
	(void)reason;
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_fsm_reject(&scs->fsm, local_conid));
}

int scs_disconnect(struct vms_scs *scs, vms_conid_t local_conid,
		   uint32_t reason)
{
	(void)reason;   /* same as scs_reject: no grounded wire field */
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_fsm_disconnect(&scs->fsm, local_conid));
}

int scs_send_msg(struct vms_scs *scs, vms_conid_t local_conid,
		 const uint8_t *body, uint32_t len)
{
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_fsm_send_msg(&scs->fsm, local_conid,
						     body, len));
}

int scs_return_credit(struct vms_scs *scs, vms_conid_t local_conid, uint16_t n)
{
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_fsm_return_credit(&scs->fsm,
							  local_conid, n));
}

int scs_dir_lookup(struct vms_scs *scs, vms_scs_sysid_t dst,
		   const uint8_t *name, scs_dir_result_cb cb, void *cb_ctx)
{
	if (scs == (struct vms_scs *)0)
		return SS__NOSUCHDEV;
	return (int)scs_glue_status(scs_dir_inquire(&scs->dir, dst, name, cb,
						    cb_ctx));
}
