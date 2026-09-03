/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_srv.c - the MSCP disk server's executive glue (FC-P6.3).
 *
 * The contract, the registration rule, the SYSGEN reading and the two minted
 * identities are all in vms_mscp_srv.h. The server ITSELF -- UQB/HQB/HRB, the
 * [state][event] table and every answer it gives -- is the pure
 * `struct mscp_srv_fsm` in vms_mscp_srv_fsm.{c,h}. This file owns storage and
 * bindings and decides nothing about the protocol.
 *
 * WHAT THIS FILE DOES NOT DO. It builds no frame and decodes none (the codec
 * owns that); it makes no MSCP decision (the server FSM owns that); it keeps no
 * volume table of its own (the ODS-2 ACP owns that -- there is ONE mounted-
 * volume table and this file READS it, the same E20 discipline vms_scs.c
 * follows for the SYSAP registry); and it reaches the substrate only through
 * exec_kbackend.h and the FC-P0.5 fork API.
 *
 * AND IT NEVER TOUCHES A DISK (FC-P6.6). Every line below runs on the CLUSTER
 * FORK THREAD, which design §3.2.6 forbids from calling exec_blockdev_*: a
 * synchronous served read here would stall the HELLO cadence, the VC
 * retransmit ladder and every barrier step behind it. A served transfer is
 * SUBMITTED from here (srv_op_io_submit -> cf_io_post) and PERFORMED in
 * vms_mscp_srv_io.c on the served-I/O worker thread; its completion comes back
 * to this file's own work handler, in this same fork thread, and the pure
 * server builds the end message from it. The block seam is not even named in
 * this file -- tools/ci/cluster_core_includes_gate.sh RULE 5 keeps it that way.
 *
 * INCLUDES: this TU is on the cluster core list enforced by
 * tools/ci/cluster_core_includes_gate.sh -- exec_kbackend.h and kernel-core
 * headers only, never a substrate header.
 */

#include "vms_internal.h"      /* the SS$_ vocabulary + the host's fixed-width types */
#include "exec_kbackend.h"     /* SS18 exec_console_printf, SS4 exec_zalloc    */
#include "vms_cluster.h"
#include "vms_cluster_fork.h"  /* FC-P0.5: cf_timer_*, cf_set_work_handler,
				* FC-P6.6: cf_io_post + the served-I/O worker */
#include "vms_mscp_srv_io.h"   /* FC-P6.6: the WORKER's half of the server    */
#include "vms_acp_serve.h"     /* the ODS-2 ACP's served-volume projection  */
#include "vms_pe.h"            /* the port's THIRD service (block transfer) */
#include "vms_pe_fsm.h"        /* PE_BLK_ACC_* and the block-frame geometry */
#include "vms_scs.h"
#include "vms_cnxman_join_fsm.h"  /* the ONE `MSCP$DISK` name (see below)   */
#include "vms_mscp_srv_fsm.h"
#include "vms_mscp_srv.h"

/* ==========================================================================
 * 0. Sizing and cadence -- OVMX design values, each labelled as one
 * ========================================================================== */

/*
 * The staging area transfers pass through. The pure server slices it into one
 * slot per HRB (vms_mscp_srv_fsm.h -- an in-flight WRITE owns its slot until
 * the peer's bytes land AND an in-flight I/O owns it until the SERVED-I/O
 * WORKER hands it back, so slots must not overlap), so what is sized here is
 * PER-REQUEST and the allocation is MSCP_SRV_MAX_REQS of them.
 *
 * MSCP_SRV_XFER_BLOCKS, the per-request ceiling, is in vms_mscp_srv.h: the
 * worker TU bounds a request against the SAME number this buffer is sized from.
 */
#define MSCP_SRV_XFER_BYTES  (MSCP_SRV_MAX_REQS * MSCP_SRV_XFER_BLOCKS * \
			      MSCP_SRV_BLOCK_SIZE)

/*
 * The server's beat: re-read the executive's served-unit set, maintain the
 * `MSCP$DISK` registration against it, and reap a request whose data never
 * arrived. One second -- fast enough that a volume mounted during boot begins
 * being served within a beat, cheap enough to cost one wakeup a second on an
 * idle node, exactly as vms_scs.c's own directory tick is sized.
 */
#define MSCP_SRV_TIMER_BEAT 0u
#define MSCP_SRV_BEAT_MS    1000u

/*
 * The receive credits this SYSAP extends to a connecting class driver.
 * GROUNDED as a FLOOR, not invented: AA-L619A-TK sec 3.4 rule 1 requires a
 * server to grant "a minimum of 2 receive buffers/credits or terminate the
 * connection" once the first SET CONTROLLER CHARACTERISTICS has completed.
 * OVMX grants 4 -- the floor plus headroom for the GET UNIT STATUS walk and a
 * transfer in flight -- an OVMX choice above a published minimum.
 */
#define MSCP_SRV_CREDITS 4u

/*
 * P.MEDI, the media type identifier, for a unit OVMX serves.
 *
 * THE ENCODING IS PUBLISHED (AA-L619A-TK sec 4.17): a 32-bit word of five-bit
 * alphabetic fields and a seven-bit two-digit number --
 * D0[31:27] D1[26:22] A0[21:17] A1[16:12] A2[11:7] N[6:0], "A" encoded as 1,
 * and "Zero represents a null or the absence of a character" for A0..A2. The
 * manual's own Appendix C worked value RA80 == 0x25641050 reproduces exactly
 * from that rule ('D'=4, 'U'=21, 'R'=18, 'A'=1, N=80), which is the check that
 * this composition is the manual's and not a guess.
 *
 * WHAT OVMX ASSERTS WITH IT, AND WHAT IT DOES NOT. D0/D1 = "DU" is TRUE and is
 * the whole claim: the unit being served IS an MSCP disk unit, which is the
 * device type every served disk in the reference corpus carries and the one a
 * class driver names `$n$DUAn`. A0..A2 and N are the DEC PRODUCT NAME of the
 * physical drive (RA92, RRD40, ...). OVMX has no such drive -- its volume is a
 * host block device -- so those fields carry the encoding's OWN null
 * ("absence of a character"), rather than a DEC model number this node does not
 * have. Naming somebody else's drive model would be a hardware claim, which is
 * exactly the class of fabrication INV-6 exists to stop.
 */
#define MSCP_SRV_MEDIA_DU_NOMEDIA \
	((uint32_t)(((uint32_t)('D' - 'A' + 1) << 27) | \
		    ((uint32_t)('U' - 'A' + 1) << 22)))

/* ==========================================================================
 * 1. The object
 * ========================================================================== */
struct vms_mscp_srv {
	struct vms_cluster    *cl;
	struct mscp_srv_fsm    fsm;
	struct mscp_srv_ops    ops;
	struct scs_sysap_ops   sysap;

	uint8_t  registered;      /* `MSCP$DISK` is in the SYSAP registry     */
	uint8_t  pending_valid;   /* connect_req recorded a peer for opened() */
	uint8_t  pad[2];
	vms_scs_sysid_t pending_peer;

	/* Real events counted HERE because here is where they happen. */
	uint32_t listens;
	uint32_t unlistens;
	uint32_t listen_failures;
	uint32_t units_without_number;   /* a mounted volume whose device name
					  * carries no unit number: NOT served */

	/* The SERVED-I/O WORKER seam (FC-P6.6), counted where it happens. */
	uint32_t io_posted;            /* transfers handed to the worker       */
	uint32_t io_post_failures;     /* the worker queue refused one         */
	uint32_t io_unit_gone;         /* the volume left between command and
					* submit: no stale (major, minor)     */
	uint32_t blockdev_io_failures; /* completions the block layer failed   */
	uint32_t worker_start_failures;

	uint8_t  xferbuf[MSCP_SRV_XFER_BYTES];
};

/* ==========================================================================
 * 2. struct mscp_srv_ops -- DOWNWARD, to the executive
 *
 * Every entry is a one-line dereference into a real executive service. Nothing
 * here interprets MSCP, and nothing caches: each reads cl->scs / cl->pe / the
 * ACP table FRESH, so a layer that has gone away yields an honest refusal
 * instead of a dereference of freed state.
 * ========================================================================== */

/*
 * The ONE mapping from a mounted ODS-2 volume to a served MSCP unit. Every
 * field is a value $MOUNT validated or an identity composed from this node's
 * real SCSSYSTEMID (vms_mscp_srv.h SS"THE TWO IDENTITIES").
 */
static int srv_unit_from_volume(struct vms_mscp_srv *s,
				const struct vms_acp_volume_info *vi,
				struct mscp_srv_unit_info *out)
{
	uint16_t unit = 0u;
	uint64_t sysid = s->cl->params.scssystemid;

	if (vms_mscp_srv_unit_from_devnam(vi->devnam, &unit) != 0) {
		s->units_without_number++;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	out->unit = unit;
	out->unit_id = ((sysid & 0xffffffffffffULL) << 16) | (uint64_t)unit;
	out->unit_size = vi->volsize;      /* the SCB's own block count */
	out->media_id = MSCP_SRV_MEDIA_DU_NOMEDIA;
	out->media_valid = 1u;
	/*
	 * P.VSER: the DRIVE/volume serial number MSCP means here is not the
	 * ODS-2 label the ACP holds, and this executive has no other source for
	 * it. Honestly absent -- the server emits a counted zero rather than
	 * passing off a label as a serial (INV-6).
	 */
	out->volume_ser = 0u;
	out->volume_ser_valid = 0u;
	/* The volume's own write protection, straight from the ACP. */
	out->write_protect = vi->read_only ? 1u : 0u;
	if (out->write_protect)
		out->unit_flags = (uint16_t)VMS_MSCP_UF_WRITE_PROT_HW;
	return 0;
}

static int srv_op_unit_at(void *ctx, uint32_t index,
			  struct mscp_srv_unit_info *out)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;
	struct vms_acp_volume_info vi;

	if (vms_acp_volume_at(index, &vi) != SS__NORMAL)
		return -1;   /* the honest end of the executive's own list */
	return srv_unit_from_volume(s, &vi, out);
}

/* Resolve a served unit NUMBER back to the backing block device, FRESH off the
 * ACP table -- so a volume dismounted since the last refresh is not written to
 * or read from a stale (major, minor). */
static int srv_backing_for_unit(struct vms_mscp_srv *s, uint16_t unit,
				uint32_t *major, uint32_t *minor)
{
	uint32_t i;

	for (i = 0; i < MSCP_SRV_MAX_UNITS; i++) {
		struct vms_acp_volume_info vi;
		uint16_t u = 0u;

		if (vms_acp_volume_at(i, &vi) != SS__NORMAL)
			break;
		if (vms_mscp_srv_unit_from_devnam(vi.devnam, &u) != 0 ||
		    u != unit)
			continue;
		*major = vi.backing_major;
		*minor = vi.backing_minor;
		return 0;
	}
	(void)s;
	return -1;
}

/*
 * HAND one served-unit transfer to the SERVED-I/O WORKER (FC-P6.6). THIS
 * FUNCTION DOES NO I/O -- design §3.2.6: "the cluster fork thread never calls
 * exec_blockdev_*", and this runs on the fork thread. The blocking half lives
 * in vms_mscp_srv_io.c and runs on the worker; the completion comes back as a
 * CF_WORK_IO_DONE work item, in this same fork thread, and the server FSM
 * builds the end message there.
 *
 * The unit's BACKING DEVICE is resolved HERE, fresh off the ACP's mounted-volume
 * table, for the same reason it always was: a volume dismounted since the last
 * refresh must not be read from a stale (major, minor). Resolving it at submit
 * (a table read under the ACP's own lock, exactly what this layer's beat already
 * does) rather than on the worker also means the worker holds NOTHING but the
 * device numbers, the LBN and the buffer -- no server state at all.
 */
static int srv_op_io_submit(void *ctx, const struct mscp_srv_io_req *req)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;
	uint32_t major = 0u, minor = 0u;
	struct cf_io io;

	if (req == NULL || s->cl->fork == NULL)
		return -1;
	if (srv_backing_for_unit(s, req->unit, &major, &minor) != 0) {
		s->io_unit_gone++;
		return -1;   /* the volume is no longer the executive's: honest */
	}

	vms_mscp_srv_io_pack(&io, (uint16_t)req->op, req->tag, major, minor,
			     req->lbn, req->nblocks, req->buf);
	if (cf_io_post(s->cl->fork, &io) != CF_OK) {
		s->io_post_failures++;
		return -1;   /* the server answers a REAL controller error */
	}
	s->io_posted++;
	return 0;
}

/*
 * The FORK-THREAD half of a completion: one dereference into the pure server,
 * which finds its own request by the tag THIS node minted. Everything the host
 * is told is decided there.
 */
static void srv_io_done(struct vms_mscp_srv *s, uint32_t tag, uint32_t status)
{
	if (status != 0u)
		s->blockdev_io_failures++;
	mscp_srv_fsm_io_done(&s->fsm, tag, status);
}

static int srv_op_send_end(void *ctx, vms_conid_t conid, const uint8_t *body,
			   uint32_t len)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	if (s->cl->scs == NULL)
		return -1;
	return scs_send_msg(s->cl->scs, conid, body, len) == (int)SS__NORMAL
		       ? 0 : -1;
}

/*
 * READ's answer. EVERY REMOTE-SIDE FIELD COMES FROM `desc`, which the server
 * read off the host's own Table A-6 buffer descriptor; this function supplies
 * only what is OURS -- the buffer we just named and its offset. The tail is the
 * transfer's final partial chunk, which FC-P6.1 piggybacks into the same
 * Ethernet frame as the end message (the recorded READ-END form; a tail of zero
 * is the recorded 118-content case and is equally legitimate).
 */
static int srv_op_send_read_data(void *ctx, vms_conid_t conid,
				 vms_scs_sysid_t peer,
				 const struct mscp_srv_bufdesc *desc,
				 const uint8_t *data, uint32_t len,
				 const uint8_t *end_body, uint32_t end_len)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;
	struct pe_blk_xfer x;
	uint32_t name = 0u;
	int rc;

	(void)conid;
	if (s->cl->pe == NULL)
		return -1;
	/* The bytes are in our staging buffer; name it for the port to read
	 * FROM (PE_BLK_ACC_SRC), and release the name as soon as the transfer
	 * is done -- a name that outlived its transfer is a buffer a stale
	 * frame could still land in. */
	if (pe_buf_register(s->cl->pe, (uint8_t *)data, len, PE_BLK_ACC_SRC,
			    &name) != (int)SS__NORMAL || name == 0u)
		return -1;

	memset(&x, 0, sizeof(x));
	x.peer = peer;
	x.dest_conid = desc->conid;      /* the host's own connection id  */
	x.local_name = name;
	x.local_offset = 0u;
	x.remote_name = desc->name;      /* the host's own buffer name    */
	x.remote_offset = desc->offset;
	x.length = len;
	x.chunk = 0u;                    /* the port's own maximum        */

	rc = pe_send_block_read_end(s->cl->pe, &x, len % VMS_BLK_DATA_MAX,
				    end_body, end_len, NULL);
	(void)pe_buf_release(s->cl->pe, name);
	return rc == (int)SS__NORMAL ? 0 : -1;
}

/*
 * WRITE's half: name the staging buffer as a DESTINATION the peer's port may
 * fill. The bytes arrive later and come back through the block-transfer
 * consumer below. The name stays registered until the request completes or the
 * server's own reaper aborts it -- see srv_op_release_buffer.
 */
static int srv_op_recv_write_data(void *ctx, vms_conid_t conid,
				  vms_scs_sysid_t peer,
				  const struct mscp_srv_bufdesc *desc,
				  uint8_t *buf, uint32_t len,
				  uint32_t *name_out)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	(void)conid; (void)peer; (void)desc;
	if (s->cl->pe == NULL)
		return -1;
	return pe_buf_register(s->cl->pe, buf, len, PE_BLK_ACC_DST,
			       name_out) == (int)SS__NORMAL ? 0 : -1;
}

static void srv_op_release_buffer(void *ctx, uint32_t name)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	if (s->cl->pe != NULL)
		(void)pe_buf_release(s->cl->pe, name);
}

static uint32_t srv_op_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)exec_ticks_ms();
}

static void srv_op_log(void *ctx, const char *msg)
{
	(void)ctx;
	if (msg != NULL)
		exec_console_printf("%s", msg);
}

static void srv_ops_bind(struct vms_mscp_srv *s)
{
	s->ops.unit_at = srv_op_unit_at;
	s->ops.io_submit = srv_op_io_submit;
	s->ops.send_end = srv_op_send_end;
	s->ops.send_read_data = srv_op_send_read_data;
	s->ops.recv_write_data = srv_op_recv_write_data;
	s->ops.release_buffer = srv_op_release_buffer;
	s->ops.now_ms = srv_op_now_ms;
	s->ops.log = srv_op_log;
	s->ops.ctx = s;
}

/* ==========================================================================
 * 3. struct scs_sysap_ops -- UPWARD, from SCS
 *
 * The `MSCP$DISK` registration. A class driver CONNECTS IN; this SYSAP never
 * connects out (the client half is FC-P7.1's disk class driver, and CNXMAN's
 * own join walk already owns the `VMS$DISK_CL_DRVR` client name).
 * ========================================================================== */

/*
 * An inbound connect naming `MSCP$DISK`. ACCEPTED -- but only because there is
 * really something behind it: this SYSAP is registered ONLY while a serveable
 * unit exists (vms_mscp_srv.h), so an accept here is never the connection that
 * black-holes the commands that follow.
 *
 * `opened()` carries no peer identity, so the requester's SCSSYSTEMID is
 * recorded HERE and consumed there -- the same pending-slot shape vms_cnxman.c
 * uses for exactly the same gap.
 */
static int srv_sysap_connect_req(void *ctx, vms_conid_t local_conid,
				 vms_scs_sysid_t peer, vms_conid_t peer_conid,
				 const uint8_t *conndata, uint32_t conndata_len)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	(void)local_conid; (void)peer_conid; (void)conndata; (void)conndata_len;
	s->pending_peer = peer;
	s->pending_valid = 1u;
	return 0;   /* ACCEPT */
}

static void srv_sysap_opened(void *ctx, vms_conid_t local_conid)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;
	vms_scs_sysid_t peer = s->pending_valid ? s->pending_peer : 0u;

	s->pending_valid = 0u;
	mscp_srv_fsm_conn_open(&s->fsm, local_conid, peer);
}

static int srv_sysap_message(void *ctx, vms_conid_t local_conid,
			     const uint8_t *body, uint32_t len)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	return mscp_srv_fsm_command(&s->fsm, local_conid, body, len);
}

static void srv_sysap_closed(void *ctx, vms_conid_t local_conid, uint32_t reason)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	(void)reason;
	mscp_srv_fsm_conn_closed(&s->fsm, local_conid);
}

static void srv_sysap_bind(struct vms_mscp_srv *s)
{
	s->sysap.connect_req = srv_sysap_connect_req;
	s->sysap.opened = srv_sysap_opened;
	s->sysap.message = srv_sysap_message;
	s->sysap.closed = srv_sysap_closed;
	s->sysap.send_failed = NULL;
	s->sysap.ctx = s;
}

/* The block-transfer consumer (vms_scs.h SS9): one dereference into the server,
 * which finds its own request by the buffer name THIS node minted. */
static void srv_block_data(void *ctx, uint32_t name, uint32_t offset,
			   uint32_t len, uint32_t bytes_remaining)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	mscp_srv_fsm_block_data(&s->fsm, name, offset, len, bytes_remaining);
}

/* ==========================================================================
 * 4. The registration, maintained against REAL state (vms_mscp_srv.h SS"..ONLY
 * WHILE A SERVEABLE UNIT EXISTS")
 * ========================================================================== */

static void srv_listen(struct vms_mscp_srv *s)
{
	int status;

	if (s->registered || s->cl->scs == NULL)
		return;
	status = scs_sysap_listen(s->cl->scs, cnxman_join_name_mscp_disk,
				  &s->sysap, (uint16_t)MSCP_SRV_CREDITS);
	if (status != (int)SS__NORMAL) {
		s->listen_failures++;
		return;
	}
	s->registered = 1u;
	s->listens++;
	exec_console_printf("vms: MSCP$DISK serving %u unit(s)\n",
			    (unsigned)mscp_srv_fsm_unit_count(&s->fsm));
}

static void srv_unlisten(struct vms_mscp_srv *s)
{
	if (!s->registered)
		return;
	if (s->cl->scs != NULL)
		(void)scs_sysap_unlisten(s->cl->scs, cnxman_join_name_mscp_disk);
	s->registered = 0u;
	s->unlistens++;
	exec_console_printf("vms: MSCP$DISK withdrawn -- no serveable unit\n");
}

/* Announce each served unit by the cluster-wide name design P6 names it. Read
 * from the server's own UQBs, so nothing is announced that is not being
 * served. */
static void srv_announce_units(struct vms_mscp_srv *s)
{
	uint32_t i;

	for (i = 0; i < MSCP_SRV_MAX_UNITS; i++) {
		const struct mscp_srv_uqb *u = mscp_srv_fsm_uqb_at(&s->fsm, i);
		char name[VMS_MSCP_SRV_NAME_MAX];

		if (u == NULL)
			continue;
		vms_mscp_srv_unit_name(s->cl->params.alloclass, u->info.unit,
				       name, sizeof(name));
		exec_console_printf("vms: MSCP$DISK serving %s (%u blocks)\n",
				    name, (unsigned)u->info.unit_size);
	}
}

/* Is this node configured to serve at all? Both parameters are SYSGEN's own
 * (vms_mscp_srv.h names exactly what is and is not decoded here). */
static int srv_serving_enabled(const struct vms_cluster *cl)
{
	return cl->params.mscp_load != 0u && cl->params.mscp_serve_all != 0u;
}

/*
 * ONE BEAT: re-read the served-unit set from the executive, move the
 * registration to match, and reap an unfinished request. This is the whole of
 * "registered only when a serveable unit exists", executed rather than assumed.
 */
static void srv_beat(struct vms_mscp_srv *s)
{
	uint32_t before = mscp_srv_fsm_unit_count(&s->fsm);
	uint32_t now;

	now = srv_serving_enabled(s->cl) ? mscp_srv_fsm_refresh_units(&s->fsm)
					 : 0u;
	if (now == 0u)
		srv_unlisten(s);
	else if (!s->registered)
		srv_listen(s);
	if (now != before && now != 0u)
		srv_announce_units(s);

	(void)mscp_srv_fsm_tick(&s->fsm);
}

static void srv_arm_beat(struct vms_mscp_srv *s)
{
	(void)cf_timer_arm(s->cl->fork, CF_OWNER_MSCP, MSCP_SRV_TIMER_BEAT, 0u,
			   MSCP_SRV_BEAT_MS);
}

/*
 * THE FORK THREAD'S ONE DOOR into this layer. Two kinds arrive: the beat, and
 * the SERVED-I/O WORKER's completion (FC-P6.6) -- which is why an MSCP command
 * that needs the disk can be answered without this thread ever waiting on one.
 */
static void srv_work_handler(void *ctx, const struct cf_work *w)
{
	struct vms_mscp_srv *s = (struct vms_mscp_srv *)ctx;

	if (s == NULL || w == NULL)
		return;
	if (w->kind == CF_WORK_IO_DONE) {
		srv_io_done(s, w->arg0, w->arg1);
		return;
	}
	if (w->kind != CF_WORK_TIMER)
		return;
	if (w->arg0 != MSCP_SRV_TIMER_BEAT)
		return;   /* an identity this layer never armed: ignored */
	srv_beat(s);
	srv_arm_beat(s);
}

/* ==========================================================================
 * 5. Lifecycle
 * ========================================================================== */

int vms_mscp_srv_start(struct vms_cluster *cl)
{
	struct vms_mscp_srv *s;
	int status;

	if (cl == NULL)
		return (int)SS__BADPARAM;
	if (cl->mscp != NULL)
		return (int)SS__NORMAL;          /* already up: idempotent */
	if (!srv_serving_enabled(cl)) {
		/* MSCP_LOAD / MSCP_SERVE_ALL say no. Not a failure: serving is
		 * a ROLE, not a membership requirement. */
		return (int)SS__NORMAL;
	}
	if (cl->fork == NULL || cl->pe == NULL || cl->scs == NULL)
		return (int)SS__NOSUCHDEV;       /* Rule 9: no layer beneath */
	if (cl->params.scssystemid == 0u) {
		/* No SCSSYSTEMID, no controller identifier to mint -- and this
		 * server will not put a zero one on the wire (INV-6). */
		return (int)SS__NOSUCHDEV;
	}

	s = (struct vms_mscp_srv *)exec_zalloc(sizeof(*s));
	if (s == NULL)
		return (int)SS__INSFMEM;
	s->cl = cl;
	srv_ops_bind(s);
	srv_sysap_bind(s);

	mscp_srv_fsm_init(&s->fsm, &s->ops);
	mscp_srv_fsm_set_ctlr_id(&s->fsm, cl->params.scssystemid);
	mscp_srv_fsm_bind_xferbuf(&s->fsm, s->xferbuf, (uint32_t)sizeof(s->xferbuf));

	(void)cf_set_work_handler(cl->fork, CF_OWNER_MSCP, srv_work_handler, s);

	/*
	 * THE SERVED-I/O WORKER (FC-P6.6). Registered BEFORE the thread starts
	 * -- cf_set_io_handler also clears a stop left by a previous cycle -- and
	 * a failure to start it is FATAL to the server, not a silent fallback:
	 * without the worker there is no way to serve a READ that does not stall
	 * the fork thread, and Rule 9 says fail honestly rather than do the
	 * wrong thing quietly.
	 */
	(void)cf_set_io_handler(cl->fork, CF_OWNER_MSCP,
				vms_mscp_srv_io_handler, s);
	status = vms_cluster_fork_worker_start(cl);
	if (status != (int)SS__NORMAL) {
		s->worker_start_failures++;
		(void)cf_set_io_handler(cl->fork, CF_OWNER_MSCP, NULL, NULL);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_MSCP, NULL, NULL);
		exec_free(s);
		return status;
	}

	cl->mscp = s;
	(void)vms_scs_set_block_consumer(cl, srv_block_data, s);

	/*
	 * The first beat runs NOW rather than in a second: a node whose system
	 * disk is already mounted at CLUSTER_START begins serving immediately,
	 * and a node with nothing mounted registers nothing -- which is the
	 * honest state either way.
	 */
	srv_beat(s);
	srv_arm_beat(s);
	return (int)SS__NORMAL;
}

void vms_mscp_srv_stop(struct vms_cluster *cl)
{
	struct vms_mscp_srv *s;

	if (cl == NULL || cl->mscp == NULL)
		return;
	s = cl->mscp;

	/* Stop being told about transfers before anything is torn down, so no
	 * completion can arrive mid-teardown. The withdrawal NAMES this server
	 * (vms_scs.h SS9 keys the table on ctx), so it cannot take the disk class
	 * driver's registration down with it. */
	(void)vms_scs_set_block_consumer(cl, NULL, s);
	srv_unlisten(s);

	/*
	 * THE SERVED-I/O WORKER GOES FIRST, AND IT IS JOINED (FC-P6.6). It is
	 * the only context that can still be writing into `s->xferbuf`, so
	 * everything below -- and the exec_free at the end above all -- is only
	 * safe once it has exited. vms_cluster_fork_worker_stop() does the
	 * predicate-then-join dance; after it returns no callback of ours is
	 * running and none can start.
	 */
	if (cl->fork != NULL) {
		vms_cluster_fork_worker_stop(cl);
		(void)cf_set_io_handler(cl->fork, CF_OWNER_MSCP, NULL, NULL);
		cf_timer_cancel(cl->fork, CF_OWNER_MSCP, MSCP_SRV_TIMER_BEAT,
				0u);
		(void)cf_set_work_handler(cl->fork, CF_OWNER_MSCP, NULL, NULL);
	}

	cl->mscp = NULL;
	exec_free(s);
}

/* ==========================================================================
 * 6. Readback
 * ========================================================================== */

void vms_mscp_srv_status(struct vms_cluster *cl, uint32_t *out_units,
			 uint32_t *out_registered)
{
	uint32_t units = 0u, reg = 0u;

	if (cl != NULL && cl->mscp != NULL) {
		units = mscp_srv_fsm_unit_count(&cl->mscp->fsm);
		reg = cl->mscp->registered;
	}
	if (out_units != NULL)
		*out_units = units;
	if (out_registered != NULL)
		*out_registered = reg;
}
