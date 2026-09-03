/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl_io_fsm.h - the MSCP DISK CLASS DRIVER's pure core (plan item
 * FC-P7.1; design docs/design-faithful-cluster-executive.md P7 "Disk class
 * driver: OVMX mounts a VAX-served disk").
 *
 * WHAT THIS IS. The half of `DUDRIVER`'s role that FC-P3.4 deliberately left
 * out. FC-P3.4's `vms_mscp_cl_fsm.h` is the DISCOVERY walk and says so in its
 * first paragraph -- "DISCOVERY ONLY: no ONLINE, no READ, no WRITE". This file
 * is everything after it: one controller block per served node, one unit block
 * per unit that walk found, one request block per outstanding transfer, and a
 * `handlers[state][event]` table that turns a served node's end messages into
 * real devices and real completed I/O.
 *
 * It does NOT replace FC-P3.4 -- it DRIVES it. `struct mscp_cl_cddb` embeds a
 * `struct vms_mscp_cl_fsm` and runs exactly the walk that file already
 * grounds, on this class driver's own `MSCP$DISK` connection. There is one
 * discovery implementation in the tree and this is not a second one.
 *
 * WHAT IT IS NOT. It owns no connection (SCS does), no named buffer (the port
 * does), no device table (vms_devtab.c does) and no clock. Every one of those
 * is reached through `struct mscp_cl_ops` below, which the GLUE
 * (vms_mscp_cl.c) binds to the real executive. That division is what makes the
 * R1 rung possible: this whole file runs against FC-P6.3's REAL server in a
 * host process, with no kernel and no boot.
 *
 * ------------------------------------------------------------------------
 * THE THREE CLASS-DRIVER STRUCTURES (design SS3.4's `DUDRIVER role, CDDB` row)
 * ------------------------------------------------------------------------
 * The published VMS description names a disk class driver's blocks: the CDDB
 * (class driver data block, one per CONTROLLER the driver talks to), the UCB
 * (unit control block, one per unit) and the CDRP (class driver request
 * packet, one per outstanding request). This file carries OVMX ANALOGUES of
 * the three -- `struct mscp_cl_cddb`, `struct mscp_cl_ucb`,
 * `struct mscp_cl_cdrp` -- named after the ROLES that published description
 * gives, never after a field layout this project has not seen (CLAUDE.md Rule
 * 8: those layouts are unpublished and are NOT reproduced; only the division
 * of responsibility is). It is the exact mirror of the naming
 * vms_mscp_srv_fsm.h already made for DSRV/UQB/HQB/HRB.
 *
 * ------------------------------------------------------------------------
 * CONTROLLER STATE, FROM THE OTHER SIDE OF sec 4.1
 * ------------------------------------------------------------------------
 * AA-L619A-TK sec 4.1: "A controller is Controller-Online to a class driver
 * exactly when a connection exists between the class driver and the MSCP
 * server within the controller." The server's own FSM makes that a per-HQB
 * fact; this file makes it a per-CDDB one, driven by the SAME SCS connection
 * lifecycle. And sec 4.3 makes UNIT-online a per-(class driver, unit) fact --
 * "Each unit may be in a different state relative to each Controller-Online
 * class driver" -- so ONLINE-ness lives on the UCB and is set ONLY by a real
 * ONLINE end message from the server, never assumed from discovery.
 *
 * ------------------------------------------------------------------------
 * INV-6 IN THIS FILE, FIELD BY FIELD
 * ------------------------------------------------------------------------
 *   P.CRF     minted by FC-P3.4's own observed convention for the discovery
 *             classes, and by this file's per-class counters for ONLINE/READ/
 *             WRITE -- unique, non-zero, and matched against the echo on every
 *             answer. An end message whose P.CRF matches no outstanding
 *             request is COUNTED and dropped, never applied to a request it
 *             does not belong to.
 *   P.UNIT    the unit number the peer's OWN GET UNIT STATUS end returned.
 *             This file never numbers a unit.
 *   P.BCNT    the caller's real block count times the real 512-byte block, and
 *             the completion reports the byte count the SERVER's end message
 *             carried -- never the count we asked for.
 *   P.LBN     the caller's own logical block number.
 *   P.BUFF    Table A-6's 12-byte buffer descriptor: { offset, SCS buffer
 *             NAME, SCS connection ID }. The NAME is the one OUR port minted
 *             for the caller's real buffer (ops->buf_register) and the Con.ID
 *             is this connection's own local Con.ID as SCS allocated it. A
 *             transfer for which the port would not name a buffer is REFUSED
 *             (counted `buf_register_failures`) -- this file never puts a zero
 *             or an invented name in that descriptor, for the same reason
 *             vms_pe_fsm.h SS3c refuses to.
 *   unit id / size / flags / media
 *             copied out of the peer's own GUS and ONLINE end messages. A
 *             field the peer did not send stays absent, with its `_valid`
 *             companion clear.
 *   the device NAME
 *             see "THE SERVED DEVICE'S NAME" below -- composed of two values
 *             read from real executive state, and when either is absent NO
 *             DEVICE IS CREATED.
 *
 * ------------------------------------------------------------------------
 * *** THE SERVED DEVICE'S NAME, AND THE ONE THING OVMX CANNOT YET SAY ***
 * ------------------------------------------------------------------------
 * Design P7's outcome line spells a served disk `$2$DUA0:` -- the OpenVMS
 * ALLOCATION-CLASS form, where the number is the SERVING node's ALLOCLASS. A
 * class driver can only spell that if it KNOWS the serving node's ALLOCLASS,
 * and this executive has no grounded transport that carries it: the CSB
 * (vms_cluster_snapshot.h `struct vms_csb_view`) records the peer's SCSNODE,
 * VOTES, LOCKDIRWT, software version and incarnation, and no allocation class;
 * the cat-0x01 PARAMS record this tree decodes (vms_cluster_codec_cm.h
 * `struct vms_cm_params`) carries VOTES and a node-parameter block, and no
 * allocation class; MSCP itself carries a unit NUMBER and a unit IDENTIFIER
 * and no allocation class at all.
 *
 * So this file spells the served device the OTHER published OpenVMS way -- the
 * NODE-QUALIFIED form, `<SCSNODE>$DUA<unit>:` -- and both halves are READ from
 * real executive state: the SCSNODE the peer itself advertised (recorded on
 * its CSB by the connection manager, passed in at mscp_cl_fsm_conn_open) and
 * the unit number the peer's own GUS end returned. Nothing is composed.
 *
 * WHAT THAT DOES AND DOES NOT ASSERT. It asserts a node and a unit, both true.
 * It does NOT assert an allocation class -- OVMX does not have the peer's, and
 * `$0$DUA0:` or a guessed `$2$` would be exactly the fabricated operator-
 * visible identity INV-6 exists to stop (two nodes serving unit 0 under
 * different allocation classes would then collide under one name, which is a
 * data-loss shape, not a cosmetic one). Every unit named this way is counted
 * in `alloclass_absent`, so the omission is measurable rather than assumed
 * away, and `mscp_cl_unit_name()` already takes an alloclass + a validity flag
 * so the day a grounded transport for the peer's ALLOCLASS lands, the `$n$`
 * spelling is a one-line change here and nothing downstream re-encodes the
 * name. RAISED for the lab/design lane by FC-P7.1's own report.
 *
 * A UNIT WHOSE NAME CANNOT BE COMPOSED IS NOT REGISTERED AS A DEVICE. If the
 * executive holds no SCSNODE for the serving peer, or the name would not fit a
 * VMS device name, this file creates NO device and counts it
 * (`units_unnamed`). A served disk under a made-up name is a fabricated
 * device.
 *
 * ------------------------------------------------------------------------
 * THE REQUEST DEADLINE IS THE CONTROLLER'S OWN, NOT A NUMBER WE INVENT
 * ------------------------------------------------------------------------
 * sec 6.16 puts a `cntlr. timeout` in the SET CONTROLLER CHARACTERISTICS END
 * MESSAGE -- the controller's own timeout, in seconds -- and this class driver
 * READS it off the SCC end and uses it as the deadline
 * mscp_cl_fsm_tick() reaps an unfinished request at. That is the whole point
 * of asking: the number in force inside this client is the number the server
 * declared, not one this file chose.
 *
 * A server that declares ZERO has declared nothing, and then the deadline is
 * this client's own MSCP_CL_HOST_TIMEOUT_SECS -- an OVMX DESIGN VALUE,
 * labelled as one, and the SAME number this client puts in P.HTMO on its own
 * SET CONTROLLER CHARACTERISTICS command, so what it declares to the server
 * and what it enforces on itself are one value and cannot disagree. Which of
 * the two a given request ran under is counted at the moment its deadline is
 * set -- `deadline_from_ctmo` vs `deadline_from_own_htmo` -- so a run can be
 * read afterwards for whether the server declared anything at all.
 *
 * WHY A DEADLINE IS THE RECOVERY MECHANISM AT ALL. vms_pe_fsm.h SS8d states
 * the consequence plainly: "the port does not retransmit a lost block frame.
 * Recovery is the MSCP layer's -- a transfer whose bytes do not all arrive is
 * a command that does not complete, and MSCP's own host timeout ... is what
 * notices." This is that noticer. A reaped request completes with a REAL
 * status (Table B-1 ST.ABO, Command Aborted) and the caller is told; it never
 * completes with a fabricated success and it never hangs.
 *
 * ------------------------------------------------------------------------
 * WRITE'S DATA DIRECTION -- RULED (design §3.2.6's E41), and why this file
 * still does not initiate a block transfer
 * ------------------------------------------------------------------------
 * READ is fully grounded end to end (docs/design-mscp-direction.md: "READ
 * streams standalone block frames server->client and piggybacks the final
 * partial chunk into the same Ethernet frame as the MSCP end message"), and
 * this file implements it.
 *
 * WRITE's choreography was an open question when FC-P7.1 landed (the capture
 * records only that "WRITE is a two-frame request/response whose two 28-byte
 * headers are BYTE-IDENTICAL -- only the presence of data distinguishes them",
 * and not which side sends the first). Design §3.2.6 rules it from the book:
 * *VAXcluster Principles* pp. 2-32..2-41 gives the block data service two
 * operations on named buffers, and BOTH are initiated by the side that knows
 * both names -- which in MSCP is always the SERVER. WRITE is therefore a
 * server-sent REQUEST DATA, and the client's PORT answers it automatically,
 * with no SYSAP involvement.
 *
 * SO THIS FILE'S BEHAVIOUR IS UNCHANGED AND IS NOW THE RIGHT ONE. It issues a
 * REAL WRITE command carrying a REAL named buffer (registered PE_BLK_ACC_SRC
 * -- which is exactly what lets the peer's request be answered out of it) and
 * a REAL descriptor, and then waits for the server's END MESSAGE, which is the
 * only thing that completes a transfer. It initiates no block transfer of its
 * own, because on this wire the client never does. The data moves one layer
 * down, in the port (FC-P6.5, vms_pe_fsm.h §8d's REQUEST DATA responder).
 *
 * If the bytes never move -- a lost request, a peer that never asks -- the
 * deadline above reaps the request with Command Aborted and the caller is told
 * the truth. `writes_undelivered` counts exactly that outcome, so a WRITE that
 * did not complete is a measured number rather than a hang.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_MSCP_CL_IO_FSM_H
#define OVMX_VMS_MSCP_CL_IO_FSM_H

#include "vms_cluster.h"
#include "vms_cluster_codec_mscp.h"
#include "vms_mscp_cl_fsm.h"   /* FC-P3.4: the discovery walk this file drives */

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 * 1. Sizing
 *
 * All OVMX DESIGN VALUES, labelled as such: no published VMS limit is in this
 * project's sources. Each is a bound big enough for the cluster scale the rest
 * of this stack is sized for and small enough to be one allocation on a VAX.
 * EXCEEDING ONE IS AN HONEST, COUNTED REFUSAL -- never a silently evicted
 * controller, unit or request, for the same reason vms_mscp_srv_fsm.h gives.
 * ========================================================================== */
#define MSCP_CL_MAX_CTLRS 4u    /* served NODES this driver talks to (CDDBs) */
#define MSCP_CL_MAX_UNITS 16u   /* served units across all of them (UCBs)    */
#define MSCP_CL_MAX_REQS  4u    /* outstanding transfers (CDRPs)             */

/* The Files-11 / MSCP logical block size -- the same 512 the executive's block
 * seam is contractually fixed at (exec_kbackend.h SS8) and the same one sec 5.3
 * assumes when it calls P.BCNT "a whole number of blocks". */
#define MSCP_CL_BLOCK_SIZE 512u

/*
 * THIS CLASS DRIVER'S OWN HOST TIMEOUT (see the file header's "THE REQUEST
 * DEADLINE" section). An OVMX design value, labelled as one, that is TRUE
 * rather than decorative: it is both what P.HTMO declares on the wire and the
 * deadline this client enforces when the server declared no timeout of its
 * own.
 */
#define MSCP_CL_HOST_TIMEOUT_SECS 30u

/* The widest served-device name this driver spells: `<SCSNODE>$DUA<unit>:` --
 * VMS_SCSNODE_MAX + "$DUA" + five digits + ":" + NUL. A unit whose name does
 * not fit a VMS device name (VMS_DEVNAM_SIZE) is not registered at all. */
#define MSCP_CL_NAME_MAX 20u

/* ==========================================================================
 * 2. The buffer descriptor this driver PUTS in a transfer command
 *
 * The mirror of vms_mscp_srv_fsm.h SS3, and it carries the same citation:
 * AA-L619A-TK Table A-6 offset 16, twelve bytes, and the vms291 lab-2 serving
 * capture's own field map (docs/design-mscp-direction.md) -- { u32 offset, u32
 * SCS buffer NAME, u32 SCS connection ID }.
 *
 * WHY IT IS COMPOSED HERE AND NOT IN THE CODEC. FC-P6.2 deliberately keeps
 * `struct vms_mscp_xfer_cmd.buffer_desc` an OPAQUE twelve-byte array (its own
 * comment: "P.BUFF, opaque (Appendix D)"), and the SERVER's pure FSM already
 * decodes it at this layer for exactly that reason. FC-P3.4's header states
 * the rule this file follows: do not reach around the frozen P6.2 seam. So the
 * two ends of the descriptor live in the two MSCP FSMs that own it, and this
 * one is the byte-for-byte inverse of vms_mscp_srv_fsm.c's srv_read_bufdesc().
 * ========================================================================== */
struct mscp_cl_bufdesc {
	uint32_t offset;   /* our offset within our own named buffer     */
	uint32_t name;     /* the name OUR port minted (never 0)         */
	uint32_t conid;    /* OUR local Con.ID for this connection       */
};

/* THIS FSM's own direction vocabulary for ops->buf_register (see SS6). The
 * glue maps them to the port's PE_BLK_ACC_* bits; nothing here names the
 * port's own constants. */
#define MSCP_CL_BUF_IN  0x01u  /* the peer may WRITE into it -- our READ  */
#define MSCP_CL_BUF_OUT 0x02u  /* the peer may READ from it  -- our WRITE */

/* ==========================================================================
 * 3. UCB -- one served unit
 *
 * Filled ONLY from the peer's own GET UNIT STATUS end (the `struct
 * vms_mscp_cl_unit` FC-P3.4 hands back) and its own ONLINE end. Nothing here
 * is synthesised.
 * ========================================================================== */
struct mscp_cl_ucb {
	uint8_t  in_use;
	uint8_t  cddb;            /* which controller serves it            */
	uint8_t  online;          /* a REAL ONLINE end message said so     */
	uint8_t  online_pending;  /* an ONLINE command is outstanding      */

	struct vms_mscp_cl_unit unit;  /* the peer's own GUS end values    */

	/* P.UNSZ, the volume's real block count, as the peer's ONLINE end
	 * reported it. `_valid` is 0 until an ONLINE end really carried one --
	 * a GUS end does not carry a unit size, so a discovered-but-not-online
	 * unit honestly has none. */
	uint32_t unit_size;
	uint8_t  unit_size_valid;
	uint8_t  registered;      /* the device row really exists          */
	uint8_t  pad[2];

	/* `<SCSNODE>$DUA<unit>:` -- see the file header. Empty when the name
	 * could not be composed, and then no device was created. */
	char     devnam[MSCP_CL_NAME_MAX];
};

/* ==========================================================================
 * 4. CDDB -- one served controller (one `MSCP$DISK` connection)
 * ========================================================================== */
enum mscp_cl_state {
	MSCP_CL_ST_AVAILABLE = 0,  /* sec 4.1, from the class driver's side */
	MSCP_CL_ST_DISCOVER  = 1,  /* connection open, the SCC/GUS walk runs */
	MSCP_CL_ST_ONLINE    = 2,  /* Controller-Online: the walk finished   */
	MSCP_CL_ST__COUNT
};

struct mscp_cl_cddb {
	uint8_t         in_use;
	uint8_t         state;      /* enum mscp_cl_state                    */
	uint8_t         scsnode_len;
	uint8_t         pad0;
	vms_conid_t     conid;      /* OUR local Con.ID -- what goes in P.BUFF */
	vms_scs_sysid_t peer;

	/* The serving node's own advertised name, as the connection manager
	 * recorded it on that node's CSB. The device-name half this driver
	 * cannot compose for itself (file header). */
	uint8_t         scsnode[VMS_SCSNODE_MAX + 2];

	/* FC-P3.4's discovery walk, on THIS connection. */
	struct vms_mscp_cl_fsm disc;

	/* What the server told us about itself in its SCC end message. Both
	 * are the PEER's values; `ctlr_id_valid` is 0 until one really
	 * arrived, and a zero P.CNTI is not recorded as an identity (sec 6.16
	 * makes it unique, so a zero is an absent one). */
	uint64_t        ctlr_id;
	uint8_t         ctlr_id_valid;
	uint8_t         pad1;
	uint16_t        ctlr_timeout;   /* P.CTMO seconds, 0 == none declared */

	/* Per-class P.CRF message-id counters for the three classes FC-P3.4
	 * does not mint (sec 5.1: unique, non-zero, echoed). */
	uint16_t        online_msgid;
	uint16_t        xfer_msgid;

	uint32_t        cmds_tx;
	uint32_t        ends_rx;
	uint32_t        units_found;
};

/* ==========================================================================
 * 5. CDRP -- one outstanding request
 * ========================================================================== */
struct mscp_cl_cdrp {
	uint8_t  in_use;
	uint8_t  opcode;         /* VMS_MSCP_OP_READ / _WRITE                */
	uint8_t  ucb;            /* UCB slot                                 */
	uint8_t  waiting_online; /* held until the unit's ONLINE end arrives */

	uint32_t handle;         /* the CALLER's own id, echoed on completion */
	uint32_t cmd_ref;        /* P.CRF, 0 while waiting_online             */
	uint32_t lbn;
	uint32_t byte_count;
	uint32_t buf_name;       /* the name OUR port minted; 0 == none yet   */
	uint32_t received;       /* bytes the port ACTUALLY took in (READ)    */
	uint32_t started_ms;     /* when the DEADLINE started running         */
	uint32_t deadline_ms;    /* the controller's own timeout, or ours     */

	uint8_t *buf;            /* the CALLER's memory; this file never owns */
	uint32_t buf_len;
};

/* ==========================================================================
 * 6. The injected ops -- every door out of this file
 * ========================================================================== */
struct mscp_cl_ops {
	/*
	 * Send one MSCP command BODY (byte 0 == frame-absolute 72) on a
	 * connection. Production: scs_send_msg through the glue. Returns 0.
	 */
	int (*send_cmd)(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len);

	/*
	 * Name the CALLER's buffer for the port, so the far server can move
	 * bytes into (READ) or out of (WRITE) it. `access` is
	 * MSCP_CL_BUF_IN / MSCP_CL_BUF_OUT -- THIS FSM's own direction
	 * vocabulary, which the glue maps to the port's PE_BLK_ACC_DST /
	 * PE_BLK_ACC_SRC. A pure FSM that named the port's own bits would be
	 * depending on the layer it is injected over (design SS3.9 rule 1).
	 * Returns 0 with *name_out non-zero, or non-zero -- and then the
	 * transfer is REFUSED, never sent with a zero name.
	 */
	int (*buf_register)(void *ctx, uint8_t *base, uint32_t len,
			    uint8_t access, uint32_t *name_out);
	void (*buf_release)(void *ctx, uint32_t name);

	/*
	 * A served unit became real / went away. The glue enters and removes
	 * the `<SCSNODE>$DUA<unit>:` row in vms_devtab with DVI$_MSCP_SERVED
	 * set. Called ONLY for a UCB whose name was really composed.
	 */
	void (*unit_ready)(void *ctx, const struct mscp_cl_ucb *u);
	void (*unit_gone)(void *ctx, const struct mscp_cl_ucb *u);

	/*
	 * One request finished. `status` is the RAW MSCP P.STS word the server
	 * sent (sec 5.6's major/sub split applies) or, for a request the
	 * deadline reaped, a real Command Aborted composed here -- never a
	 * success this file could not back up. `bytes` is what the server said
	 * it transferred, or 0.
	 */
	void (*io_done)(void *ctx, uint32_t handle, uint16_t status,
			uint32_t bytes);

	/* P.TIME for SET CONTROLLER CHARACTERISTICS (sec 6.16: this host's
	 * current VMS absolute time, or 0). A pure TU may not read a clock --
	 * gate RULE4 -- so the glue reads it through the seam. */
	uint64_t (*time_now)(void *ctx);
	uint32_t (*now_ms)(void *ctx);
	void     (*log)(void *ctx, const char *msg);
	void    *ctx;
};

/* ==========================================================================
 * 7. The event vocabulary and the dispatch table's shape
 *
 * One event per thing that can happen to a class driver: the connection's two
 * ends, and the five MSCP end-message classes FC-P6.2 measures, plus the
 * catch-all for anything else (counted, never applied to a request).
 * handlers[state][event]; an empty cell is COUNTED, not guessed.
 * ========================================================================== */
enum mscp_cl_event {
	MSCP_CL_EV_CONN_OPEN  = 0,
	MSCP_CL_EV_CONN_CLOSE = 1,
	MSCP_CL_EV_SCC_END    = 2,
	MSCP_CL_EV_GUS_END    = 3,
	MSCP_CL_EV_ONLINE_END = 4,
	MSCP_CL_EV_READ_END   = 5,
	MSCP_CL_EV_WRITE_END  = 6,
	MSCP_CL_EV_END_OTHER  = 7,
	MSCP_CL_EV__COUNT
};

/* ==========================================================================
 * 8. The class driver object
 *
 * No globals (design SS3.9 rule 3): every instance is one node's one disk
 * class driver, and the rung-2 simulator runs N of them in one host process.
 * ========================================================================== */
struct mscp_cl_fsm {
	const struct mscp_cl_ops *ops;

	struct mscp_cl_cddb cddb[MSCP_CL_MAX_CTLRS];
	struct mscp_cl_ucb  ucb[MSCP_CL_MAX_UNITS];
	struct mscp_cl_cdrp cdrp[MSCP_CL_MAX_REQS];

	/*
	 * The SPLICE scratches, exactly as vms_mscp_srv_fsm.h SS9 documents
	 * them and for the same reason: the FC-P6.2 codec addresses an MSCP
	 * message at FRAME-ABSOLUTE offsets, while a SYSAP is handed -- and
	 * hands back -- only its own body, byte 0 == abs 72. A command is
	 * built into `cmdframe` and sent from `cmdframe + VMS_OFF_SYSAP_BODY`;
	 * a received body is copied to `endframe + VMS_OFF_SYSAP_BODY` before
	 * the codec parses it. That is why this file contains no wire offset
	 * of its own beyond the codec's OWN published body origin.
	 */
	uint8_t cmdframe[VMS_OFF_SYSAP_BODY + VMS_MSCP_CMD_BODY_LEN];
	uint8_t endframe[VMS_MSCP_END_FRAME_LEN(VMS_MSCP_END_BODY_MAX)];

	/* Real events, counted where they happen (INV-6: counted, never
	 * inferred, and never a reason to invent an answer). */
	uint32_t ends_rx;
	uint32_t ends_unparsed;      /* the codec refused the body            */
	uint32_t ends_no_cddb;       /* an end on a connection we do not hold */
	uint32_t ends_unmatched;     /* a P.CRF matching no outstanding request */
	uint32_t ignored_events;     /* an empty [state][event] cell           */
	uint32_t no_cddb_slot;
	uint32_t no_ucb_slot;
	uint32_t no_cdrp_slot;
	uint32_t send_failures;
	uint32_t codec_failures;
	uint32_t buf_register_failures;
	uint32_t units_registered;
	uint32_t units_unnamed;      /* no SCSNODE / name too long: NO device */
	uint32_t alloclass_absent;   /* named node-qualified; see file header */
	uint32_t onlines_sent;
	uint32_t onlines_refused;    /* the server refused the ONLINE          */
	uint32_t reads_issued;
	uint32_t reads_completed;
	uint32_t writes_issued;
	uint32_t writes_completed;
	uint32_t writes_undelivered; /* reaped WRITEs -- see the file header   */
	uint32_t io_failed;          /* a real non-success P.STS from the peer */
	uint32_t reqs_aborted;       /* the deadline reaped them               */
	uint32_t deadline_from_ctmo; /* deadline came from the server's P.CTMO */
	uint32_t deadline_from_own_htmo;
	uint32_t block_bytes_rx;
	uint32_t block_unmatched;    /* a completion naming no live request    */
	uint32_t short_transfers;    /* the end arrived with bytes still out   */
};

/* ==========================================================================
 * 9. Lifecycle
 * ========================================================================== */

/* Reset to an empty class driver bound to `ops`. Builds and sends nothing. */
void mscp_cl_fsm_init(struct mscp_cl_fsm *f, const struct mscp_cl_ops *ops);

/* ==========================================================================
 * 10. The events
 * ========================================================================== */

/*
 * An `MSCP$DISK` connection to `peer` reached OPEN on our local `conid`.
 * Allocates a CDDB, records the serving node's own advertised SCSNODE (see the
 * file header: without it no device can be named) and STARTS FC-P3.4's
 * discovery walk by sending the first SET CONTROLLER CHARACTERISTICS.
 *
 * `scsnode`/`scsnode_len` is the peer's name as the connection manager
 * recorded it. Pass len 0 when the executive holds none -- the walk still
 * runs and the units are still enumerated, but no device is created for them
 * and each is counted in `units_unnamed`.
 *
 * Returns 0, or non-zero when there was no CDDB slot (counted).
 */
int mscp_cl_fsm_conn_open(struct mscp_cl_fsm *f, vms_conid_t conid,
			  vms_scs_sysid_t peer, const uint8_t *scsnode,
			  uint32_t scsnode_len);

/*
 * That connection closed. Every unit it served is removed (ops->unit_gone --
 * the device really goes away, because the path to it really did) and every
 * request outstanding on it completes with a real Command Aborted. Nothing is
 * left hanging and nothing is left claiming to be reachable.
 */
void mscp_cl_fsm_conn_closed(struct mscp_cl_fsm *f, vms_conid_t conid);

/*
 * One MSCP end-message BODY arrived on `conid` (byte 0 == frame-absolute 72,
 * exactly what the SCS SYSAP `message` callback delivers). Returns 0 when this
 * driver took it; non-zero means the bytes were not an MSCP end message for
 * this driver and the caller should count them as undelivered.
 */
int mscp_cl_fsm_end_msg(struct mscp_cl_fsm *f, vms_conid_t conid,
			const uint8_t *body, uint32_t len);

/*
 * The port finished moving block-transfer bytes into one of OUR named buffers
 * (vms_pe.h SS4 `block_data`). `name` is OUR buffer name, so this routine
 * finds its request by a value this node itself minted -- never by a value the
 * peer chose.
 */
void mscp_cl_fsm_block_data(struct mscp_cl_fsm *f, uint32_t name,
			    uint32_t offset, uint32_t len,
			    uint32_t bytes_remaining);

/*
 * The class driver's own beat. Reaps every request past its deadline (see the
 * file header) and completes it Command Aborted. Returns how many were reaped
 * (0 on a normal beat).
 */
uint32_t mscp_cl_fsm_tick(struct mscp_cl_fsm *f);

/* ==========================================================================
 * 11. The I/O services -- what the executive above asks this driver for
 * ========================================================================== */

/*
 * Read / write `nblocks` 512-byte blocks at `lbn` on the served unit whose
 * device row is `devnam`, to or from `buf` (the CALLER's memory, which must
 * outlive the request -- this driver registers it with the port and never
 * copies it). `handle` is the caller's own identifier, echoed to
 * ops->io_done.
 *
 * Returns 0 when the request was really taken (a command went out, or it is
 * queued behind a real ONLINE that is in flight); non-zero when it was
 * refused, and then ops->io_done is NOT called -- a refusal is a synchronous
 * answer, not a completion.
 *
 * A unit that is not yet Unit-Online to this class driver is brought online
 * FIRST (a real ONLINE command, sec 4.3: "MSCP commands addressed to a unit
 * that is Unit-Offline will be rejected"), and the request waits on its CDRP
 * until that ONLINE really completes -- under the same deadline, so a unit
 * that never comes online is a request that is reaped, not one that hangs.
 */
int mscp_cl_fsm_read(struct mscp_cl_fsm *f, const char *devnam, uint32_t lbn,
		     uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		     uint32_t handle);
int mscp_cl_fsm_write(struct mscp_cl_fsm *f, const char *devnam, uint32_t lbn,
		      uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		      uint32_t handle);

/* ==========================================================================
 * 12. The served unit's NAME
 *
 * A pure derivation of REAL executive values, so the R1 rung proves it without
 * a boot. See the file header for why the node-qualified spelling is what OVMX
 * can honestly say today, and what `alloclass`/`alloclass_valid` are for.
 * ========================================================================== */

/*
 * Write the served unit's VMS device name into `out`, NUL-terminated.
 *
 *   alloclass_valid != 0  ->  `$<alloclass>$DUA<unit>:`   (OpenVMS allocation-
 *                             class form -- the spelling design P7 names)
 *   alloclass_valid == 0  ->  `<SCSNODE>$DUA<unit>:`      (node-qualified)
 *
 * Returns 0 on success. Non-zero -- and `out` left empty -- when the name
 * cannot be composed: no SCSNODE and no allocation class, or a name that would
 * not fit. NO DEVICE IS CREATED for a unit whose name this refuses.
 */
int mscp_cl_unit_name(const uint8_t *scsnode, uint32_t scsnode_len,
		      uint8_t alloclass, int alloclass_valid, uint16_t unit,
		      char *out, uint32_t outsz);

/* ==========================================================================
 * 13. Readback (the same values a diagnostic would project -- INV-6)
 * ========================================================================== */
const struct mscp_cl_cddb *mscp_cl_fsm_cddb_at(const struct mscp_cl_fsm *f,
					       uint32_t index);
const struct mscp_cl_ucb *mscp_cl_fsm_ucb_at(const struct mscp_cl_fsm *f,
					     uint32_t index);
uint32_t mscp_cl_fsm_unit_count(const struct mscp_cl_fsm *f);
const char *mscp_cl_state_name(enum mscp_cl_state s);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_MSCP_CL_IO_FSM_H */
