/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_srv_fsm.h - the MSCP DISK SERVER's pure core (plan item FC-P6.3;
 * design docs/design-faithful-cluster-executive.md P6 "MSCP server in the
 * executive").
 *
 * WHAT THIS IS. The state, the dispatch table and the answers of the
 * `MSCP$DISK` SYSAP's SERVER half: one HQB per remote class driver, one UQB
 * per served unit, one HRB per outstanding remote request, and a
 * `handlers[state][event]` table that turns a received MSCP command into the
 * end message a real class driver expects. It is the exact counterpart of
 * FC-P3.4's `vms_mscp_cl_fsm.h` (the CLIENT half's discovery walk) and it
 * follows the same two rules that file states: it builds and parses ONLY
 * through the FC-P6.2 codec (design SS3.9 rule 2 -- not one raw wire offset
 * lives here), and it is PURE (design SS3.9 rule 4 -- no seam call, no
 * allocation, no clock but ops->now_ms), so the whole server runs on a host
 * unit test in microseconds and in the rung-2 N-node simulator, not only
 * against a live VAX.
 *
 * WHAT IT IS NOT. It owns no connection (SCS does), no buffer registration
 * (the port does), no block device (the executive's block seam does) and no
 * volume table (the ODS-2 ACP does). Every one of those is reached through
 * `struct mscp_srv_ops` below, and the GLUE (vms_mscp_srv.c) binds them to
 * the real executive. That division is what makes the R1 rung possible: a
 * FAKE volume behind `unit_at` and a FAKE WORKER behind `io_submit` (one that
 * completes when the test says so) drive the whole SCC/GUS/ONLINE/READ/WRITE
 * ladder, including the async waits, with no kernel and no thread at all.
 *
 * ------------------------------------------------------------------------
 * THE THREE SERVER STRUCTURES (design SS3.4's "DSRV, UQB, HQB, HRB" row)
 * ------------------------------------------------------------------------
 * *VMS for Alpha Platforms Internals and Data Structures* / the published
 * VAXcluster description name a real MSCP server's blocks: DSRV (the server
 * itself), UQB (per served UNIT), HQB (per remote class driver, i.e. per
 * HOST), HULB (per host/unit, for load balancing) and HRB (per outstanding
 * remote REQUEST, holding the IRP). This file carries OVMX ANALOGUES of
 * three of them -- `struct mscp_srv_uqb`, `struct mscp_srv_hqb`,
 * `struct mscp_srv_hrb` -- named after the ROLES that published description
 * gives, never after a field layout this project has not seen (CLAUDE.md
 * Rule 8: the structures' internal layouts are unpublished and are NOT
 * reproduced; only the division of responsibility is). There is deliberately
 * no HULB: OVMX serves from one node with no second path to balance against,
 * so a load-balancing block would be a structure with nothing in it.
 *
 * WHY THE PER-UNIT STATE LIVES ON THE HQB. AA-L619A-TK sec 4.3 is explicit:
 * "Each unit may be in one of three states relative to each class driver
 * that is Controller-Online to an MSCP server ... Each unit may be in a
 * different state relative to each Controller-Online class driver." So
 * ONLINE-ness, and the unit flags a host asked for, are (host, unit) facts
 * and are stored on the HQB, indexed by the unit's UQB slot. The UQB holds
 * only what is true of the unit itself whoever is asking.
 *
 * ------------------------------------------------------------------------
 * CONTROLLER STATE (AA-L619A-TK sec 4.1), AND WHY IT IS THE FSM's STATE
 * ------------------------------------------------------------------------
 * "A controller is Controller-Online to a class driver exactly when a
 * connection exists between the class driver and the MSCP server within the
 * controller" (sec 4.1), and "the MSCP server enters the Controller-Online
 * state relative to a host class driver upon successful synchronization with
 * the class driver ... by establishing a connection" (sec 4.1's own
 * continuation). So the two states below are per-HQB and are driven by the
 * SCS connection's own lifecycle: a `MSCP$DISK` CDT that reached OPEN is
 * Controller-Online, and one that has not is Controller-Available. A command
 * arriving with no Controller-Online HQB behind it is answered Invalid
 * Command -- never serviced, and never silently dropped.
 *
 * ------------------------------------------------------------------------
 * INV-6 IN THIS FILE, FIELD BY FIELD
 * ------------------------------------------------------------------------
 * Every value this server puts on the wire is READ from something real:
 *
 *   P.CRF     echoed from the host's own command (sec 5.1 requires it).
 *   P.UNIT    the unit number the host addressed, or -- on the GUS
 *             NEXT-UNIT walk -- the number of the UQB the walk actually
 *             found, which came from the executive's own device name.
 *   P.STS     computed from the request against real state (does the unit
 *             exist? is it online to THIS host? is it write protected?),
 *             never a constant success.
 *   P.UNTI    the UQB's unit identifier, minted by the GLUE from this node's
 *             real SCSSYSTEMID and the unit's real number (see
 *             vms_mscp_srv.h) -- never 0, because sec 6.12 makes a zero unit
 *             identifier mean "virtually no characteristics are valid".
 *   P.UNSZ    the volume's REAL block count as $MOUNT validated it off the
 *             SCB. A unit whose size the executive does not hold is not
 *             served at all.
 *   P.MEDI    the media type identifier, supplied by the glue. When the
 *             executive holds no media identity the glue says so and this
 *             file emits the value it was given and COUNTS it
 *             (`media_absent`) -- it never composes one.
 *   P.UNFL    vms_mscp_online_unfl_compose() of (a) the P.UNFL the HOST sent
 *             in its own ONLINE command for this unit, recorded on the HQB,
 *             and (b) the unit's own real flags. Before a host has sent an
 *             ONLINE, half (a) is genuinely absent and a zero goes out --
 *             counted in `unfl_no_host_value`, never back-filled with the
 *             0x8000 the corpus shows (docs/design-mscp-direction.md records
 *             that bit as HOST-ORIGINATED; asserting it with no host behind
 *             it would be exactly the fabrication INV-6 forbids).
 *   geometry  track/group/cylinder/RCT/RBN/copies are ZERO, which is the
 *             SPEC'S OWN encoding for "inapplicable" (sec 6.12: "track size
 *             ... 0 if inapplicable", "group size 0 if track size is 0",
 *             "cylinder size 0 if group size is 0"), and is what every
 *             geometry-less unit in the reference corpus emits.
 *   P.VSER    the glue's value or an honest, counted zero -- same rule as
 *             P.MEDI.
 *
 * ------------------------------------------------------------------------
 * LOCAL I/O IS ASYNCHRONOUS (FC-P6.6, design §3.2.6's E42 corollary)
 * ------------------------------------------------------------------------
 * A real MSCP server does not stall its port on a disk. It issues local I/O
 * asynchronously -- an IRP to the local driver -- and completes the MSCP
 * command on THAT I/O's completion. FC-P6.3's first cut called the executive's
 * synchronous block seam straight out of the command handler, which on OVMX
 * means the CLUSTER FORK THREAD: the one context that also carries the HELLO
 * cadence, the VC retransmit ladder and every barrier step. A 20 ms served read
 * was therefore a 20 ms cluster stall on this node and, through the barrier, on
 * every other member -- a TIMVCFAIL risk under load, on real hardware.
 *
 * So this file no longer has a `read_blocks`/`write_blocks` op it can call and
 * wait on. It has ONE downward door for storage, `ops->io_submit`, which HANDS
 * the transfer to a served-I/O worker and returns immediately; the worker's
 * answer arrives later at mscp_srv_fsm_io_done(). The HRB is what spans the
 * gap -- which is exactly what a real server's HRB is for, since the published
 * description has it hold the IRP.
 *
 * AN HRB IS THEREFORE IN ONE OF TWO WAITS (enum mscp_srv_req_state):
 *
 *   WAIT_DATA  the peer's bytes have not all arrived (a WRITE between its
 *              command and its last block-transfer frame). The port owns the
 *              staging slot; the reaper may end this request at any time.
 *   WAIT_IO    the WORKER holds the staging slot and will answer exactly once.
 *              NOTHING may free the HRB, release its slot, or answer its
 *              command until that completion lands -- not the reaper, not a
 *              connection close. Both instead RECORD their intent
 *              (`abort_pending`, `abandoned`) and the completion path carries
 *              it out. That is the whole of the concurrency discipline here,
 *              and it is why a completion is matched by `io_tag` -- a number
 *              this server minted and never reuses while it is outstanding --
 *              rather than by an HRB index another request may already own.
 *
 * The fork thread still does ALL the protocol: it builds the end message and
 * drives the SEND DATA from the completion. Only the waiting moved.
 *
 * ------------------------------------------------------------------------
 * WRITE PROTECTION IS ANSWERED, NOT FAKED (the plan row's own clause)
 * ------------------------------------------------------------------------
 * A WRITE to a write-protected unit is answered `ST.WPR` with the sub-code
 * Table B-2 gives for the REASON: 256 (composed 0x2006) when the protection
 * is the unit's own -- the executive's read-only volume -- and 128 (composed
 * 0x1006) when it is the SOFTWARE protection this host itself asked for with
 * the ONLINE command's MD.SWP modifier. Not one block is written, the byte
 * count in the end message is the real 0, and no block transfer is started.
 * The same protection is ADVERTISED ahead of time in P.UNFL (UF.WPH / UF.WPS)
 * on every GUS and ONLINE end, so a host learns it before it tries.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_MSCP_SRV_FSM_H
#define OVMX_VMS_MSCP_SRV_FSM_H

#include "vms_cluster.h"
#include "vms_cluster_codec_mscp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 1. Sizing
 *
 * All three are OVMX DESIGN VALUES, labelled as such: AA-L619A-TK bounds none
 * of them and the published VMS server's own limits are not in this project's
 * sources. Each is a bound big enough for the cluster scale the rest of this
 * stack is sized for (VMS_CLUB_MAX_CSB members) and small enough to be one
 * allocation on a VAX. EXCEEDING ONE IS AN HONEST, COUNTED REFUSAL -- never a
 * silently evicted host, unit or request.
 * ========================================================================== */
#define MSCP_SRV_MAX_UNITS 8u    /* served units (UQBs) */
#define MSCP_SRV_MAX_HOSTS 16u   /* remote class drivers (HQBs) */
/*
 * Outstanding remote requests (HRBs). This one is NOT free: every HRB owns its
 * OWN slice of the staging buffer for the whole life of its transfer (see
 * mscp_srv_fsm_bind_xferbuf), so the count and the buffer's size together fix
 * how much data can be in flight. Four, to match the receive credits the glue
 * extends to a class driver -- so the bound the credit ledger already enforces
 * and the bound this table enforces are the same number rather than two
 * numbers that can disagree.
 */
#define MSCP_SRV_MAX_REQS  4u

/* The Files-11 / MSCP logical block size. The same 512 the executive's block
 * seam is contractually fixed at (exec_kbackend.h SS8) and the same one sec 5.3
 * assumes when it calls P.BCNT "a whole number of blocks". */
#define MSCP_SRV_BLOCK_SIZE 512u

/*
 * THIS SERVER'S OWN CONTROLLER TIMEOUT. sec 6.16 defines the SET CONTROLLER
 * CHARACTERISTICS end message's `cntlr. timeout` as the CONTROLLER's own
 * timeout in seconds (one effective byte, <= 255). An OVMX DESIGN VALUE,
 * labelled as one -- and it is TRUE rather than decorative: it is the deadline
 * mscp_srv_fsm_tick() actually reaps an unfinished request at, so the number
 * this server declares on the wire is the one in force inside it. (The
 * reference corpus shows a real controller declaring 20; that is a sanity
 * check on the order of magnitude, not the source of this value -- OVMX does
 * not replay a captured constant it has no mechanism behind.)
 */
#define MSCP_SRV_CTLR_TIMEOUT_SECS 20u
#define MSCP_SRV_REQ_TIMEOUT_MS    (MSCP_SRV_CTLR_TIMEOUT_SECS * 1000u)

/* ==========================================================================
 * 2. Status composition helpers this server needs
 *
 * sec 5.6 note 1 composes a status word as "(subcode * ST.SUB) + code", which
 * VMS_MSCP_STATUS() already does. Table B-2 additionally specifies that an
 * "Invalid Command" status names the FIELD in error, "offset*256 + code",
 * where `offset` is the command-message offset of the bad field. That is the
 * same word VMS_MSCP_STATUS(ST.CMD, offset*8) composes, so this macro is the
 * one derivation and not a second encoding of the status word.
 * ========================================================================== */
#define MSCP_SRV_SUB_INVALID_FIELD(cmd_off) ((uint16_t)((cmd_off) * 8u))

/* The two command-message offsets this server can find fault with, DERIVED
 * from the codec's frame-absolute ones so the two views cannot drift. */
#define MSCP_SRV_CMDOFF_BCNT \
	((uint16_t)(VMS_OFF_MSCP_XFER_C_BCNT - VMS_OFF_SYSAP_BODY))  /* 12 */
#define MSCP_SRV_CMDOFF_LBN \
	((uint16_t)(VMS_OFF_MSCP_XFER_C_LBN - VMS_OFF_SYSAP_BODY))   /* 28 */

/* ==========================================================================
 * 3. The buffer descriptor a transfer command carries
 *
 * AA-L619A-TK Table A-6 offset 16, twelve bytes, and the vms291 lab-2 serving
 * capture's own field map (docs/design-mscp-direction.md): { u32 offset, u32
 * SCS buffer NAME, u32 SCS connection ID }. EVERY FIELD IS READ off the host's
 * command -- this server never mints a remote buffer name, exactly as
 * vms_pe_fsm.h SS3c refuses to (a fabricated buffer name is the same class of
 * error as a fabricated lock id).
 * ========================================================================== */
struct mscp_srv_bufdesc {
	uint32_t offset;   /* the host's offset within its own named buffer */
	uint32_t name;     /* the host's SCS buffer name                    */
	uint32_t conid;    /* the SCS connection id the host named          */
};

/* ==========================================================================
 * 4. UQB -- one served unit
 *
 * Filled ONLY by mscp_srv_fsm_refresh_units() from ops->unit_at(), i.e. from
 * the executive's own mounted-volume state. A UQB is never created for a
 * volume the executive does not hold.
 * ========================================================================== */
struct mscp_srv_unit_info {
	uint16_t unit;            /* MSCP unit number, from the real device name */
	uint16_t unit_flags;      /* the unit's OWN Table A-5 flags (UF.WPH, ...) */
	uint64_t unit_id;         /* P.UNTI -- never 0 (see the file header)      */
	uint32_t media_id;        /* P.MEDI, meaningful iff media_valid           */
	uint32_t unit_size;       /* P.UNSZ, real 512-byte blocks                 */
	uint32_t volume_ser;      /* P.VSER, meaningful iff volume_ser_valid      */
	uint8_t  media_valid;
	uint8_t  volume_ser_valid;
	uint8_t  write_protect;   /* the VOLUME's own read-only state             */
	uint8_t  pad;
};

struct mscp_srv_uqb {
	uint8_t                   in_use;
	uint8_t                   pad[3];
	struct mscp_srv_unit_info info;
};

/* ==========================================================================
 * 5. HQB -- one remote class driver
 * ========================================================================== */
enum mscp_srv_state {
	MSCP_SRV_ST_AVAILABLE = 0,  /* sec 4.1 "Controller-Available"       */
	MSCP_SRV_ST_ONLINE    = 1,  /* sec 4.1 "Controller-Online"          */
	MSCP_SRV_ST__COUNT
};

struct mscp_srv_hqb {
	uint8_t         in_use;
	uint8_t         state;        /* enum mscp_srv_state                  */
	uint8_t         scc_done;     /* SET CONTROLLER CHARACTERISTICS count */
	uint8_t         pad0;
	vms_conid_t     conid;
	vms_scs_sysid_t peer;

	/* What this host actually SET, read off its own SCC command (sec 6.16).
	 * Zero here means "the host declared nothing", which is what sec 6.16's
	 * own default (all controller flags clear) says. */
	uint16_t        ctlr_flags;
	uint16_t        host_timeout;

	/* The per-(host, unit) facts sec 4.3 requires (see the file header). */
	uint8_t         online[MSCP_SRV_MAX_UNITS];
	uint16_t        host_unfl[MSCP_SRV_MAX_UNITS];

	uint32_t        cmds_rx;
	uint32_t        ends_tx;
	uint32_t        invalid_cmds;
};

/* ==========================================================================
 * 6. HRB -- one outstanding remote request
 *
 * A transfer command that could not be answered in the same call (a READ
 * whose data must be streamed, a WRITE whose data has not arrived yet) keeps
 * its HRB until it completes. Nothing here is a guess: the descriptor was read
 * off the command and the buffer name is the one OUR port minted.
 * ========================================================================== */

/*
 * WHICH WAIT AN HRB IS IN. See the file header's "LOCAL I/O IS ASYNCHRONOUS":
 * the distinction is not cosmetic -- it decides who owns the staging slot, and
 * therefore who is allowed to free the HRB.
 */
enum mscp_srv_req_state {
	MSCP_SRV_REQ_WAIT_DATA = 0,  /* the PORT owns the slot: peer bytes due */
	MSCP_SRV_REQ_WAIT_IO   = 1,  /* the WORKER owns the slot: hands off    */
	MSCP_SRV_REQ__COUNT
};

struct mscp_srv_hrb {
	uint8_t                 in_use;
	uint8_t                 opcode;       /* VMS_MSCP_OP_READ / _WRITE   */
	uint8_t                 hqb;          /* HQB slot                    */
	uint8_t                 uqb;          /* UQB slot                    */
	uint8_t                 state;        /* enum mscp_srv_req_state     */
	/*
	 * The reaper found this request past its deadline while the WORKER
	 * still owned the staging slot, and so could not end it.
	 *
	 * IT DOES NOT ABORT THE I/O, and the completion path does not answer
	 * "Command Aborted" because of it: the local transfer owns its own
	 * completion, exactly as a VMS IRP outstanding to a local driver does,
	 * and the honest answer to the host is what ACTUALLY happened to the
	 * volume -- reporting an abort for a write that landed would be a lie
	 * about the disk. The flag exists so the beat records the lateness
	 * ONCE (`reqs_abort_deferred`) instead of every second.
	 */
	uint8_t                 abort_pending;
	/*
	 * The host's connection went away while the WORKER still owned the
	 * slot. There is nobody left to answer (sec 4.1: no command survives a
	 * connection's incarnation), so the completion frees it in silence.
	 */
	uint8_t                 abandoned;
	uint8_t                 pad0;
	uint32_t                cmd_ref;      /* P.CRF, echoed back          */
	uint16_t                unit;         /* P.UNIT the host addressed   */
	uint16_t                pad1;
	uint32_t                lbn;          /* P.LBN                       */
	uint32_t                byte_count;   /* P.BCNT                      */
	uint32_t                received;     /* bytes the port ACTUALLY took
					       * into our buffer so far      */
	uint32_t                local_name;   /* OUR port's buffer name, 0 = none */
	/*
	 * The tag this server minted for the OUTSTANDING worker request, or 0
	 * when none is outstanding. A completion is matched on THIS, never on
	 * an HRB index: an index is reused the moment a request completes, and
	 * a late completion landing on its successor would answer the wrong
	 * host's command with another host's status.
	 */
	uint32_t                io_tag;
	uint32_t                started_ms;
	struct mscp_srv_bufdesc desc;         /* READ off the command        */
};

/* ==========================================================================
 * 6b. The served-unit I/O this server hands to the WORKER (FC-P6.6)
 *
 * Every field is READ from something real: the unit number from the executive's
 * own device name (through the UQB), the LBN and the block count from the
 * host's own command after this server's gates passed them, and the buffer is
 * the HRB's exclusive staging slot. `tag` is this server's own outstanding-
 * request identity, echoed back by the worker so a completion can be matched to
 * the request that asked for it and to nothing else.
 * ========================================================================== */
enum mscp_srv_io_op {
	MSCP_SRV_IO_READ  = 0,
	MSCP_SRV_IO_WRITE = 1
};

struct mscp_srv_io_req {
	uint32_t tag;        /* the HRB's io_tag; echoed in the completion */
	uint8_t  op;         /* enum mscp_srv_io_op                       */
	uint8_t  pad0[1];
	uint16_t unit;       /* the served unit NUMBER (the UQB's own)     */
	uint32_t lbn;
	uint32_t nblocks;
	/*
	 * The HRB's staging slot. The WORKER owns these bytes from the moment
	 * io_submit returns 0 until the completion is delivered; this file
	 * neither reads nor writes them in that window.
	 */
	uint8_t *buf;
};

/* ==========================================================================
 * 7. The injected ops -- every door out of this file
 * ========================================================================== */
struct mscp_srv_ops {
	/*
	 * The `index`-th SERVEABLE unit, or non-zero when there is none. The
	 * glue reads it out of the executive's mounted-volume table; nothing
	 * here invents a unit, and a node with no volume simply has no UQB
	 * (which is what makes `MSCP$DISK` registration conditional -- see
	 * vms_mscp_srv.h).
	 */
	int (*unit_at)(void *ctx, uint32_t index,
		       struct mscp_srv_unit_info *out);

	/*
	 * HAND whole-block I/O on a served unit's REAL backing device to the
	 * served-I/O worker. THIS CALL DOES NOT PERFORM THE I/O and must never
	 * block: it queues the request and returns.
	 *
	 * Returns 0 when the worker ACCEPTED it -- and then exactly one
	 * mscp_srv_fsm_io_done() with this `tag` will follow, and until it does
	 * the worker owns req->buf. Non-zero means the request was NOT queued
	 * (no worker, or its queue is full), which this file answers with a
	 * real MSCP error status, never with a success it cannot back up.
	 *
	 * There is deliberately no synchronous twin. Design §3.2.6: "the
	 * cluster fork thread never calls exec_blockdev_*", and an op this file
	 * could wait on is exactly how that rule gets broken back.
	 */
	int (*io_submit)(void *ctx, const struct mscp_srv_io_req *req);

	/* Send one MSCP end-message BODY (byte 0 == frame-absolute 72) on a
	 * connection. Production: scs_send_msg through the glue. */
	int (*send_end)(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len);

	/*
	 * READ's answer: stream `len` bytes of `data` to the host's named
	 * buffer and piggyback `end_body` on the final frame (FC-P6.1's
	 * pe_blk_send + pe_blk_send_read_end). `desc` is the descriptor READ
	 * off the host's own command -- this file supplies no remote-side
	 * field of its own. Returns 0, or non-zero when the transfer could not
	 * be made (the caller then answers with a real error status).
	 */
	int (*send_read_data)(void *ctx, vms_conid_t conid,
			      vms_scs_sysid_t peer,
			      const struct mscp_srv_bufdesc *desc,
			      const uint8_t *data, uint32_t len,
			      const uint8_t *end_body, uint32_t end_len);

	/*
	 * WRITE's half: register `buf` as a destination the peer's port may
	 * fill, and report the name OUR port minted in *name_out. The bytes
	 * arrive later and are reported through mscp_srv_fsm_block_data().
	 * Returns 0, or non-zero when no buffer could be named.
	 */
	int (*recv_write_data)(void *ctx, vms_conid_t conid,
			       vms_scs_sysid_t peer,
			       const struct mscp_srv_bufdesc *desc,
			       uint8_t *buf, uint32_t len, uint32_t *name_out);
	void (*release_buffer)(void *ctx, uint32_t name);

	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);
	void    *ctx;
};

/* ==========================================================================
 * 8. The event vocabulary and the dispatch table's shape
 *
 * One event per thing that can happen to a server: the connection's two ends,
 * and the five MSCP commands this item's captures confirm a real class driver
 * sends, plus the catch-all for every other opcode (answered Invalid Command,
 * never ignored). The table is handlers[state][event]; an empty cell is an
 * event that state has no edge for, and it is COUNTED, not guessed.
 * ========================================================================== */
enum mscp_srv_event {
	MSCP_SRV_EV_CONN_OPEN  = 0,
	MSCP_SRV_EV_CONN_CLOSE = 1,
	MSCP_SRV_EV_CMD_SCC    = 2,
	MSCP_SRV_EV_CMD_GUS    = 3,
	MSCP_SRV_EV_CMD_ONLINE = 4,
	MSCP_SRV_EV_CMD_READ   = 5,
	MSCP_SRV_EV_CMD_WRITE  = 6,
	MSCP_SRV_EV_CMD_OTHER  = 7,
	MSCP_SRV_EV__COUNT
};

/* ==========================================================================
 * 9. The server object
 *
 * No globals (design SS3.9 rule 3): every instance is one node's one MSCP
 * server, and the rung-2 simulator runs N of them in one host process.
 * ========================================================================== */
struct mscp_srv_fsm {
	const struct mscp_srv_ops *ops;

	/*
	 * P.CNTI, the controller identifier this server answers SET CONTROLLER
	 * CHARACTERISTICS with. sec 6.16 makes it a unique quadword and says
	 * nothing about its composition, so the GLUE mints it from this node's
	 * REAL SCSSYSTEMID (vms_mscp_srv.h) and hands it here. Zero means the
	 * executive had none -- and then this server is not started at all, so
	 * a zero never reaches the wire.
	 */
	uint64_t ctlr_id;

	struct mscp_srv_uqb uqb[MSCP_SRV_MAX_UNITS];
	struct mscp_srv_hqb hqb[MSCP_SRV_MAX_HOSTS];
	struct mscp_srv_hrb hrb[MSCP_SRV_MAX_REQS];
	uint32_t            n_units;

	/*
	 * The staging buffer transfers' bytes pass through, BOUND by the glue
	 * (a pure TU has no allocator) and SLICED one slot per HRB.
	 *
	 * WHY SLICED AND NOT SHARED. A READ is finished inside one dispatch
	 * (read the blocks, hand them to the transfer, done), but a WRITE holds
	 * its buffer from the command until the peer's bytes arrive -- possibly
	 * across many dispatches. With one shared buffer a second transfer
	 * arriving in that window would overwrite the first one's data and the
	 * volume would receive the wrong bytes: a silent corruption, which is
	 * strictly worse than a refusal. So the buffer is `MSCP_SRV_MAX_REQS`
	 * equal slots, an HRB owns its slot for its whole life, and running out
	 * of HRBs is an honest, COUNTED refusal (`no_hrb_slot`) with a real
	 * MSCP status -- never an eviction.
	 *
	 * `xferbuf_slot` is therefore the LARGEST single transfer this server
	 * can stage; a command asking for more is refused with a real "Invalid
	 * Byte Count" naming P.BCNT, never truncated.
	 */
	uint8_t  *xferbuf;
	uint32_t  xferbuf_len;
	uint32_t  xferbuf_slot;

	/*
	 * The next outstanding-request tag to mint (FC-P6.6). Monotonic, and
	 * ZERO IS NEVER HANDED OUT -- 0 is this file's "no I/O outstanding" on
	 * an HRB, so a tag of 0 would make an idle HRB match a completion.
	 */
	uint32_t next_io_tag;

	/*
	 * The two SPLICE scratches. The FC-P6.2 codec addresses an MSCP
	 * message at FRAME-ABSOLUTE offsets (VMS_OFF_MSCP_* are all
	 * VMS_OFF_SYSAP_BODY + n), while a SYSAP is handed -- and hands back --
	 * only its own body, byte 0 == abs 72. So a received body is copied to
	 * `cmdframe + VMS_OFF_SYSAP_BODY` before the codec parses it and an end
	 * message is built into `endframe` and sent from
	 * `endframe + VMS_OFF_SYSAP_BODY`. That is exactly the splice
	 * vms_cnxman_join_fsm.c already makes for the CLIENT half's commands,
	 * and it is why this file contains no wire offset of its own: the only
	 * position it names is the codec's OWN published body origin.
	 */
	uint8_t  cmdframe[VMS_OFF_SYSAP_BODY + VMS_MSCP_CMD_BODY_LEN];
	uint8_t  endframe[VMS_OFF_SYSAP_BODY + VMS_MSCP_END_BODY_MAX];

	/* Real events, counted where they happen (INV-6: counted, never
	 * inferred, and never a reason to invent an answer). */
	uint32_t cmds_rx;
	uint32_t ends_tx;
	uint32_t end_tx_failed;
	uint32_t cmds_no_hqb;         /* a command with no Controller-Online HQB */
	uint32_t cmds_unparsed;       /* the codec refused the body             */
	uint32_t ignored_events;      /* an empty [state][event] cell            */
	uint32_t no_hqb_slot;
	uint32_t no_hrb_slot;
	uint32_t reads_served;
	uint32_t writes_served;
	uint32_t write_protect_refusals;
	uint32_t blockdev_failures;
	uint32_t xfer_refused;        /* the port would not move the bytes       */
	uint32_t media_absent;        /* a GUS/ONLINE end with no media identity */
	uint32_t vser_absent;
	uint32_t unfl_no_host_value;  /* P.UNFL with no host half yet            */
	uint32_t reqs_aborted;        /* HRBs the timeout reaped (Command Aborted) */
	uint32_t reqs_refused_busy;   /* no HRB free: refused, never overwritten  */

	/* The served-I/O worker (FC-P6.6). Every submitted request ends in
	 * exactly one of io_done_ok / io_done_failed / reqs_abandoned, or is
	 * still outstanding; a completion that matches no outstanding request
	 * is io_done_stale and is DROPPED, never applied to another request. */
	uint32_t io_submitted;        /* handed to the worker                    */
	uint32_t io_refused;          /* the worker would not take it: real error*/
	uint32_t io_done_ok;          /* completions the block layer called good */
	uint32_t io_done_failed;      /* completions the block layer called bad  */
	uint32_t io_done_stale;       /* a completion for no outstanding request */
	uint32_t reqs_abort_deferred; /* reaper hit an HRB the worker still held */
	uint32_t reqs_abandoned;      /* host went away mid-I/O: freed in silence*/
};

/* ==========================================================================
 * 10. Lifecycle and binding
 * ========================================================================== */

/* Reset to an empty server bound to `ops`. Builds and sends nothing. */
void mscp_srv_fsm_init(struct mscp_srv_fsm *f, const struct mscp_srv_ops *ops);

/* Install the controller identifier (see `ctlr_id` above). */
void mscp_srv_fsm_set_ctlr_id(struct mscp_srv_fsm *f, uint64_t ctlr_id);

/*
 * Bind the staging buffer transfers pass through. `len` is sliced into
 * MSCP_SRV_MAX_REQS equal slots, one per HRB (see the struct's own note); a
 * `len` that will not make MSCP_SRV_MAX_REQS non-empty slots binds NOTHING, so
 * a transfer command is then refused honestly rather than served from a buffer
 * that is not there. Must be called before a READ or WRITE can be served.
 */
void mscp_srv_fsm_bind_xferbuf(struct mscp_srv_fsm *f, uint8_t *buf,
			       uint32_t len);

/*
 * Re-read the served-unit set from ops->unit_at(). Returns how many UQBs are
 * now in use. THIS IS THE ONLY WRITER of the UQB table, and it is what makes
 * "MSCP$DISK is registered only when a serveable unit exists" a decision about
 * REAL executive state rather than a configuration flag.
 */
uint32_t mscp_srv_fsm_refresh_units(struct mscp_srv_fsm *f);

/* How many units this server currently holds. */
uint32_t mscp_srv_fsm_unit_count(const struct mscp_srv_fsm *f);

/* ==========================================================================
 * 11. The events
 * ========================================================================== */

/* The `MSCP$DISK` connection reached OPEN / closed. These are the two edges
 * sec 4.1 makes the controller state out of. */
void mscp_srv_fsm_conn_open(struct mscp_srv_fsm *f, vms_conid_t conid,
			    vms_scs_sysid_t peer);
void mscp_srv_fsm_conn_closed(struct mscp_srv_fsm *f, vms_conid_t conid);

/*
 * One MSCP command BODY arrived on `conid` (byte 0 == frame-absolute 72,
 * exactly what the SCS SYSAP `message` callback delivers). Returns 0 when the
 * server took it -- which it does for every command, including the ones it
 * refuses, because a refusal IS an answer. Non-zero means the bytes were not
 * an MSCP command at all and the caller should count them as undelivered.
 */
int mscp_srv_fsm_command(struct mscp_srv_fsm *f, vms_conid_t conid,
			 const uint8_t *body, uint32_t len);

/*
 * The port finished moving block-transfer bytes into one of our named
 * buffers (vms_pe.h's `block_data`). `name` is OUR buffer name, so this
 * routine finds the HRB by a value this node itself minted -- never by a
 * value the peer chose.
 */
void mscp_srv_fsm_block_data(struct mscp_srv_fsm *f, uint32_t name,
			     uint32_t offset, uint32_t len,
			     uint32_t bytes_remaining);

/*
 * THE SERVED-I/O WORKER'S ANSWER (FC-P6.6), delivered on the FORK THREAD as a
 * CF_WORK_IO_DONE work item -- so the protocol this drives (the READ's SEND
 * DATA and its piggybacked end message, the WRITE's end message) is still built
 * in the one serialised context, exactly like every other event.
 *
 * `tag` is the value this server minted in the submitted mscp_srv_io_req, and
 * `status` is the executive block seam's REAL answer -- 0 for a transfer that
 * happened, non-zero for one that did not. This file never invents either: a
 * non-zero status becomes a real Drive Error with a zero byte count, never a
 * success it cannot back up (INV-6).
 *
 * A `tag` matching no outstanding request is COUNTED (`io_done_stale`) and
 * dropped -- the only honest thing to do with an answer to a question nobody is
 * still asking.
 */
void mscp_srv_fsm_io_done(struct mscp_srv_fsm *f, uint32_t tag, uint32_t status);

/*
 * The server's own beat. Reaps any HRB older than MSCP_SRV_REQ_TIMEOUT_MS and
 * answers it "Command Aborted" (Table B-1 ST.ABO) -- the honest end for a
 * transfer whose bytes never arrived, in place of an HRB that leaks and a host
 * that waits forever. Returns how many were reaped (0 on a normal beat).
 *
 * An HRB the WORKER still owns is NOT reaped here: its staging slot is in
 * another thread's hands, so the abort is RECORDED (`abort_pending`, counted in
 * `reqs_abort_deferred`) and carried out by mscp_srv_fsm_io_done() when the
 * slot really comes back. Deferred, never forgotten and never a use-after-free.
 */
uint32_t mscp_srv_fsm_tick(struct mscp_srv_fsm *f);

/* ==========================================================================
 * 12. The served unit's NAME and NUMBER
 *
 * Design P6: "served units named `$ALLOCLASS$DUAn`". Neither of these is a wire
 * field -- MSCP carries a unit NUMBER and a unit IDENTIFIER, and the `$n$DUAn`
 * spelling is what a class driver builds from the SERVING node's allocation
 * class -- but both are pure derivations of REAL executive values (the node's
 * ALLOCLASS SYSGEN parameter and the executive's own device name for the
 * volume), so they live in the pure TU where the R1 rung can prove them
 * without a boot.
 * ========================================================================== */
#define VMS_MSCP_SRV_NAME_MAX 24u

/* How far into a device name the unit-number scan may run. A VMS device name
 * is `ddcu:` (VMS_DEVNAM_SIZE == 16 on both substrates); this bound is the
 * pure TU's own, so the scan is total even on a string that is not
 * NUL-terminated within a device name's width. */
#define MSCP_SRV_DEVNAM_SCAN_MAX 16u

/* Write "$<alloclass>$DUA<unit>:" into `out` (at least VMS_MSCP_SRV_NAME_MAX
 * bytes), NUL-terminated. Total and never-failing. */
void vms_mscp_srv_unit_name(uint8_t alloclass, uint16_t unit, char *out,
			    uint32_t outsz);

/*
 * The MSCP unit NUMBER a VMS device name carries: the trailing decimal run
 * before the optional colon ("DKA0:" -> 0, "VDA12:" -> 12). Returns 0 and sets
 * *out on success, non-zero when the name carries no unit number -- and then no
 * unit is served, because a served unit with a made-up number is a fabricated
 * device (INV-6).
 */
int vms_mscp_srv_unit_from_devnam(const char *devnam, uint16_t *out);

/* ==========================================================================
 * 13. Readback (the same values a diagnostic would project -- INV-6)
 * ========================================================================== */
const struct mscp_srv_hqb *mscp_srv_fsm_hqb_at(const struct mscp_srv_fsm *f,
					       uint32_t index);
const struct mscp_srv_uqb *mscp_srv_fsm_uqb_at(const struct mscp_srv_fsm *f,
					       uint32_t index);
const char *mscp_srv_state_name(enum mscp_srv_state s);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_MSCP_SRV_FSM_H */
