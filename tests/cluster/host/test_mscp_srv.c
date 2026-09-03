/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_mscp_srv.c - the R1 rung for FC-P6.3, the executive-resident MSCP DISK
 * SERVER (src/kernel-core/vms_mscp_srv_fsm.{c,h} + the glue's own bindings).
 *
 * WHAT THIS PROVES, and it is the plan row's own done-condition:
 *
 *   1. A FAKE executive volume behind the injected ops is SERVED: SCC, the GET
 *      UNIT STATUS NEXT-UNIT walk with its Unit-Offline terminator, ONLINE,
 *      READ and WRITE each produce the end message a real class driver expects,
 *      PARSED BACK THROUGH THE FC-P6.2 CODEC rather than eyeballed as bytes.
 *   2. A READ-ONLY volume answers WRITE with the REAL MSCP write-protect status
 *      -- Table B-1 ST.WPR with Table B-2's own reason sub-code -- and writes
 *      NOTHING. Both halves are asserted: the status word AND the fake block
 *      device's write counter staying at zero.
 *   3. `MSCP$DISK` is registered ONLY when a serveable unit exists. The
 *      DECISION is proved against the real server object (unit count from real
 *      ops), and the SHIPPING glue that acts on it (src/kernel-core/
 *      vms_mscp_srv.c) is source-scanned for the listen/unlisten bindings --
 *      the same two-proof shape test_cnxman_glue.c and test_scs_glue_conn.c
 *      use for glue that names exec_kbackend.h and so cannot be host-linked.
 *   4. Every value on the wire traces to executive state: the unit identifier
 *      to the injected volume, the unit size to its real block count, P.CRF to
 *      the host's own command. A field the fake executive does NOT hold (the
 *      volume serial) goes out as a zero AND is counted.
 *
 * WHAT IS NOT PROVEN HERE, and where it is: the R4/R5 legs -- a real VAX
 * MOUNTing an OVMX-served unit, and the byte-exact ONLINE-END oracle -- are
 * FC-P6.4's, and tests/lab/mscp_serve_mount.sh is their harness (lab-deferred,
 * exit 77).
 */

#include <stdio.h>
#include <string.h>

#include "vms_mscp_srv_fsm.h"
#include "vms_mscp_cl_fsm.h"   /* FC-P3.4: the REAL class driver, for the interop leg */
#include "cluster_test.h"

/* ------------------------------------------------------------------ *
 * The fake executive behind the injected ops
 * ------------------------------------------------------------------ */

#define FAKE_BLOCKS 64u
#define FAKE_CONID  0x00010005u
#define FAKE_PEER   0x0000aa00040001ULL

struct fake_unit {
	uint16_t unit;
	uint32_t size;
	uint8_t  write_protect;
	uint8_t  data[FAKE_BLOCKS * MSCP_SRV_BLOCK_SIZE];
};

struct fake_exec {
	struct fake_unit units[MSCP_SRV_MAX_UNITS];
	uint32_t         n_units;

	/* what the server actually did */
	uint32_t reads;
	uint32_t writes;
	uint32_t read_fail_next;      /* force the NEXT read to fail */

	/* the last end message the server sent, spliced back to a frame */
	uint8_t  end_frame[VMS_OFF_SYSAP_BODY + VMS_MSCP_END_BODY_MAX + 64u];
	uint32_t end_len;
	uint32_t ends;

	/* the last block transfer the server asked for */
	uint32_t xfer_len;
	uint32_t xfer_remote_name;
	uint32_t xfer_remote_offset;
	uint32_t xfer_conid;
	uint8_t  xfer_data[FAKE_BLOCKS * MSCP_SRV_BLOCK_SIZE];
	uint32_t xfers;

	/* the destination buffer a WRITE named */
	uint8_t *write_buf;
	uint32_t write_len;
	uint32_t write_name;
	uint32_t buffers_released;

	uint32_t now_ms;
};

static struct fake_unit *fake_find(struct fake_exec *e, uint16_t unit)
{
	uint32_t i;

	for (i = 0; i < e->n_units; i++) {
		if (e->units[i].unit == unit)
			return &e->units[i];
	}
	return NULL;
}

static int fake_unit_at(void *ctx, uint32_t index,
			struct mscp_srv_unit_info *out)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	if (index >= e->n_units)
		return -1;
	memset(out, 0, sizeof(*out));
	out->unit = e->units[index].unit;
	out->unit_size = e->units[index].size;
	/* The same composition the glue makes: a real system id and a real
	 * unit number, never zero. */
	out->unit_id = ((uint64_t)FAKE_PEER << 16) | e->units[index].unit;
	out->media_id = 0x25400000u;   /* "DU", no media name -- the glue's */
	out->media_valid = 1u;
	out->volume_ser_valid = 0u;    /* the fake executive has none */
	out->write_protect = e->units[index].write_protect;
	if (out->write_protect)
		out->unit_flags = VMS_MSCP_UF_WRITE_PROT_HW;
	return 0;
}

static int fake_read_blocks(void *ctx, uint16_t unit, uint32_t lbn,
			    uint32_t nblocks, uint8_t *buf)
{
	struct fake_exec *e = (struct fake_exec *)ctx;
	struct fake_unit *u = fake_find(e, unit);

	if (e->read_fail_next) {
		e->read_fail_next = 0u;
		return -1;
	}
	if (u == NULL || lbn + nblocks > u->size)
		return -1;
	memcpy(buf, u->data + (lbn * MSCP_SRV_BLOCK_SIZE),
	       nblocks * MSCP_SRV_BLOCK_SIZE);
	e->reads++;
	return 0;
}

static int fake_write_blocks(void *ctx, uint16_t unit, uint32_t lbn,
			     uint32_t nblocks, const uint8_t *buf)
{
	struct fake_exec *e = (struct fake_exec *)ctx;
	struct fake_unit *u = fake_find(e, unit);

	if (u == NULL || lbn + nblocks > u->size)
		return -1;
	memcpy(u->data + (lbn * MSCP_SRV_BLOCK_SIZE), buf,
	       nblocks * MSCP_SRV_BLOCK_SIZE);
	e->writes++;
	return 0;
}

/* Capture an end message the way SCS delivers one: the SYSAP body, byte 0 ==
 * frame-absolute 72. Spliced back so the codec (which addresses a message
 * frame-absolutely) can parse it -- the same splice the server itself makes. */
static void fake_capture_end(struct fake_exec *e, const uint8_t *body,
			     uint32_t len)
{
	memset(e->end_frame, 0, sizeof(e->end_frame));
	memcpy(e->end_frame + VMS_OFF_SYSAP_BODY, body, len);
	e->end_len = len;
	e->ends++;
}

static int fake_send_end(void *ctx, vms_conid_t conid, const uint8_t *body,
			 uint32_t len)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	(void)conid;
	fake_capture_end(e, body, len);
	return 0;
}

static int fake_send_read_data(void *ctx, vms_conid_t conid,
			       vms_scs_sysid_t peer,
			       const struct mscp_srv_bufdesc *desc,
			       const uint8_t *data, uint32_t len,
			       const uint8_t *end_body, uint32_t end_len)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	(void)conid; (void)peer;
	e->xfer_len = len;
	e->xfer_remote_name = desc->name;
	e->xfer_remote_offset = desc->offset;
	e->xfer_conid = desc->conid;
	if (len <= sizeof(e->xfer_data))
		memcpy(e->xfer_data, data, len);
	e->xfers++;
	/* The end message rides the transfer's final frame -- capture it the
	 * same way, so the test asserts the SAME bytes that would go out. */
	fake_capture_end(e, end_body, end_len);
	return 0;
}

static int fake_recv_write_data(void *ctx, vms_conid_t conid,
				vms_scs_sysid_t peer,
				const struct mscp_srv_bufdesc *desc,
				uint8_t *buf, uint32_t len, uint32_t *name_out)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	(void)conid; (void)peer; (void)desc;
	e->write_buf = buf;
	e->write_len = len;
	e->write_name = 0x0100002au;   /* a name only the "port" mints */
	*name_out = e->write_name;
	return 0;
}

static void fake_release_buffer(void *ctx, uint32_t name)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	if (name == e->write_name)
		e->write_name = 0u;
	e->buffers_released++;
}

static uint32_t fake_now_ms(void *ctx)
{
	return ((struct fake_exec *)ctx)->now_ms;
}

static void fake_log(void *ctx, const char *msg)
{
	(void)ctx; (void)msg;
}

struct srv_env {
	struct fake_exec    fake;
	struct mscp_srv_ops ops;
	struct mscp_srv_fsm fsm;
	/* Sized exactly as the glue sizes it: MSCP_SRV_MAX_REQS slots of
	 * 8 blocks each, so the R1 rung exercises the SAME per-request
	 * ceiling the shipping server has. */
	uint8_t             xferbuf[MSCP_SRV_MAX_REQS * 8u * MSCP_SRV_BLOCK_SIZE];
	uint8_t             cmd[VMS_OFF_SYSAP_BODY + VMS_MSCP_CMD_BODY_LEN];
};

static void env_bind_ops(struct srv_env *e)
{
	memset(&e->ops, 0, sizeof(e->ops));
	e->ops.unit_at = fake_unit_at;
	e->ops.read_blocks = fake_read_blocks;
	e->ops.write_blocks = fake_write_blocks;
	e->ops.send_end = fake_send_end;
	e->ops.send_read_data = fake_send_read_data;
	e->ops.recv_write_data = fake_recv_write_data;
	e->ops.release_buffer = fake_release_buffer;
	e->ops.now_ms = fake_now_ms;
	e->ops.log = fake_log;
	e->ops.ctx = &e->fake;
}

/* One served unit, unit 0, `nblocks` blocks, seeded with a recognisable
 * pattern so a READ's bytes can be checked against the volume they came from. */
static void env_add_unit(struct srv_env *e, uint16_t unit, uint32_t nblocks,
			 int write_protect)
{
	struct fake_unit *u = &e->fake.units[e->fake.n_units++];
	uint32_t i;

	memset(u, 0, sizeof(*u));
	u->unit = unit;
	u->size = nblocks;
	u->write_protect = (uint8_t)write_protect;
	for (i = 0; i < nblocks * MSCP_SRV_BLOCK_SIZE; i++)
		u->data[i] = (uint8_t)(i + unit);
}

static void env_init(struct srv_env *e)
{
	memset(e, 0, sizeof(*e));
	env_bind_ops(e);
	mscp_srv_fsm_init(&e->fsm, &e->ops);
	mscp_srv_fsm_set_ctlr_id(&e->fsm, (uint64_t)FAKE_PEER);
	mscp_srv_fsm_bind_xferbuf(&e->fsm, e->xferbuf, (uint32_t)sizeof(e->xferbuf));
}

/* Bring the server up with `nunits` served units and an OPEN connection. */
static void env_open(struct srv_env *e)
{
	(void)mscp_srv_fsm_refresh_units(&e->fsm);
	mscp_srv_fsm_conn_open(&e->fsm, FAKE_CONID, FAKE_PEER);
}

/* ------------------------------------------------------------------ *
 * Command builders -- through the FC-P6.2 codec, never a hand-laid array
 * ------------------------------------------------------------------ */

static const uint8_t *env_cmd_body(struct srv_env *e)
{
	return e->cmd + VMS_OFF_SYSAP_BODY;
}

static int env_build_scc(struct srv_env *e, uint32_t cmd_ref, uint16_t flags,
			 uint16_t htmo)
{
	struct vms_mscp_scc_cmd c;

	memset(&c, 0, sizeof(c));
	memset(e->cmd, 0, sizeof(e->cmd));
	c.hdr.cmd_ref = cmd_ref;
	c.ctlr_flags = flags;
	c.host_timeout = htmo;
	return vms_mscp_scc_cmd_build(&c, e->cmd, (uint32_t)sizeof(e->cmd),
				      NULL) == VMS_CODEC_OK;
}

static int env_build_gus(struct srv_env *e, uint32_t cmd_ref, uint16_t unit,
			 uint16_t mods)
{
	struct vms_mscp_gus_cmd c;

	memset(&c, 0, sizeof(c));
	memset(e->cmd, 0, sizeof(e->cmd));
	c.hdr.cmd_ref = cmd_ref;
	c.hdr.unit = unit;
	c.modifiers = mods;
	return vms_mscp_gus_cmd_build(&c, e->cmd, (uint32_t)sizeof(e->cmd),
				      NULL) == VMS_CODEC_OK;
}

static int env_build_online(struct srv_env *e, uint32_t cmd_ref, uint16_t unit,
			    uint16_t mods, uint16_t unfl)
{
	struct vms_mscp_online_cmd c;

	memset(&c, 0, sizeof(c));
	memset(e->cmd, 0, sizeof(e->cmd));
	c.hdr.cmd_ref = cmd_ref;
	c.hdr.unit = unit;
	c.modifiers = mods;
	c.unit_flags = unfl;
	return vms_mscp_online_cmd_build(&c, e->cmd, (uint32_t)sizeof(e->cmd),
					 NULL) == VMS_CODEC_OK;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xffu);
	p[1] = (uint8_t)((v >> 8) & 0xffu);
	p[2] = (uint8_t)((v >> 16) & 0xffu);
	p[3] = (uint8_t)((v >> 24) & 0xffu);
}

static int env_build_xfer(struct srv_env *e, uint8_t opcode, uint32_t cmd_ref,
			  uint16_t unit, uint32_t bcnt, uint32_t lbn,
			  uint32_t buf_off, uint32_t buf_name, uint32_t buf_conid)
{
	struct vms_mscp_xfer_cmd c;

	memset(&c, 0, sizeof(c));
	memset(e->cmd, 0, sizeof(e->cmd));
	c.hdr.cmd_ref = cmd_ref;
	c.hdr.unit = unit;
	c.hdr.opcode = opcode;
	c.byte_count = bcnt;
	c.lbn = lbn;
	/* Table A-6's 12-byte buffer descriptor: { offset, SCS buffer NAME,
	 * SCS connection ID }, exactly as the vms291 capture maps it. */
	put_le32(&c.buffer_desc[0], buf_off);
	put_le32(&c.buffer_desc[4], buf_name);
	put_le32(&c.buffer_desc[8], buf_conid);
	return vms_mscp_xfer_cmd_build(&c, e->cmd, (uint32_t)sizeof(e->cmd),
				       NULL) == VMS_CODEC_OK;
}

static int env_feed(struct srv_env *e)
{
	return mscp_srv_fsm_command(&e->fsm, FAKE_CONID, env_cmd_body(e),
				    VMS_MSCP_CMD_BODY_LEN);
}

/* ------------------------------------------------------------------ *
 * 1. SET CONTROLLER CHARACTERISTICS
 * ------------------------------------------------------------------ */
static void test_scc(void)
{
	struct srv_env e;
	struct vms_mscp_scc_end end;

	printf("-- SCC: answered from the controller's own state\n");
	env_init(&e);
	env_add_unit(&e, 0u, 8u, 0);
	env_open(&e);

	ct_check(env_build_scc(&e, 0x81a30002u, VMS_MSCP_CF_THIS_HOST, 60u),
		 "the SCC command builds through the codec");
	ct_check_eq_u32((unsigned long)env_feed(&e), 0,
			"the server took the command");
	ct_check_eq_u32(e.fake.end_len, VMS_MSCP_SCC_END_LEN,
			"the SCC end message is the MEASURED 28 bytes");
	ct_check_eq_u32((unsigned long)vms_mscp_scc_end_parse(e.fake.end_frame,
							      (uint32_t)sizeof(e.fake.end_frame),
							      &end),
			VMS_CODEC_OK, "and it parses as an SCC END");
	ct_check_eq_u32(end.eh.hdr.cmd_ref, 0x81a30002u,
			"P.CRF is the host's own, echoed (sec 5.1)");
	ct_check_eq_u32(end.eh.hdr.opcode,
			VMS_MSCP_OP_SCC | VMS_MSCP_END_BIT,
			"the endcode is SCC | OP.END");
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_SUCCESS,
			"sec 6.16: Success only");
	ct_check_eq_u32(end.eh.status_subcode, VMS_MSCP_SUB_NORMAL,
			"sub-code Normal");
	ct_check_eq_u32(end.ctlr_flags, VMS_MSCP_CF_THIS_HOST,
			"P.CNTF reports the flags THIS HOST set, read off its command");
	ct_check_eq_u32(end.ctlr_timeout, MSCP_SRV_CTLR_TIMEOUT_SECS,
			"P.CTMO is the deadline the server's own reaper uses");
	ct_check((unsigned long)end.ctlr_id == (unsigned long)FAKE_PEER,
		 "P.CNTI is the controller identity the executive supplied, not 0");

	/* sec 6.16: "host must supply 0; server returns Invalid Command if
	 * non-zero". */
	{
		struct vms_mscp_scc_cmd c;

		memset(&c, 0, sizeof(c));
		memset(e.cmd, 0, sizeof(e.cmd));
		c.hdr.cmd_ref = 0x11u;
		c.version = 1u;
		(void)vms_mscp_scc_cmd_build(&c, e.cmd, (uint32_t)sizeof(e.cmd),
					     NULL);
		(void)env_feed(&e);
		(void)vms_mscp_scc_end_parse(e.fake.end_frame,
					     (uint32_t)sizeof(e.fake.end_frame),
					     &end);
		ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_INVALID_CMD,
				"a non-zero MSCP version is Invalid Command (sec 6.16)");
	}
}

/* ------------------------------------------------------------------ *
 * 2. The GET UNIT STATUS NEXT-UNIT walk and its terminator
 * ------------------------------------------------------------------ */
static void test_gus_walk(void)
{
	struct srv_env e;
	struct vms_mscp_gus_end end;

	printf("-- GUS: the MD.NXU walk, and its Unit-Offline terminator\n");
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 0);
	env_add_unit(&e, 3u, 32u, 0);
	env_open(&e);
	ct_check_eq_u32(mscp_srv_fsm_unit_count(&e.fsm), 2,
			"two REAL volumes became two UQBs");

	/* Step 1: the walk's own seed, unit 1 (never 0 -- FC-P3.4's own rule)
	 * finds the next unit with a number >= 1, which is unit 3. */
	ct_check(env_build_gus(&e, 0x7ee20001u, 1u, VMS_MSCP_MOD_NEXT_UNIT),
		 "GUS #1 builds with MD.NXU");
	(void)env_feed(&e);
	ct_check_eq_u32(e.fake.end_len, VMS_MSCP_GUS_END_LEN,
			"the GUS end message is the MEASURED 52 bytes");
	ct_check_eq_u32((unsigned long)vms_mscp_gus_end_parse(e.fake.end_frame,
							      (uint32_t)sizeof(e.fake.end_frame),
							      &end),
			VMS_CODEC_OK, "and it parses as a GUS END");
	ct_check_eq_u32(end.eh.hdr.unit, 3u,
			"sec 6.12 MD.NXU: the NEXT unit at or above the asked-for number");
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_AVAILABLE,
			"a real unit no class driver has ONLINEd is Unit-Available");
	ct_check(end.unit_id != 0u,
		 "P.UNTI is non-zero, so sec 6.12 says the characteristics ARE valid");
	ct_check_eq_u32(end.shadow_unit, 3u, "sec 6.12: P.SHUN == the unit number");
	ct_check_eq_u32(end.track_size, 0u,
			"geometry is sec 6.12's OWN 'inapplicable' zero, not an invented shape");

	/* Step 2: the walk continues from the previous end's own unit + 1 and
	 * runs off the end -> Unit-Offline, unit id 0. */
	ct_check(env_build_gus(&e, 0x7ee20002u, 4u, VMS_MSCP_MOD_NEXT_UNIT),
		 "GUS #2 builds");
	(void)env_feed(&e);
	(void)vms_mscp_gus_end_parse(e.fake.end_frame,
				     (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_OFFLINE,
			"the walk terminator is Unit-Offline, not an error");
	ct_check_eq_u32(end.eh.status_subcode, VMS_MSCP_SUB_OFL_UNKNOWN,
			"Table B-2 sub-code 0: unit unknown");
	ct_check_eq_u32((unsigned long)end.unit_id, 0,
			"and P.UNTI is 0 -- sec 6.12's 'no characteristics are valid'");
	ct_check_eq_u32(end.eh.hdr.unit, 4u,
			"the unit number echoed is the one the host asked about");

	/* A NON-walk GUS names one unit exactly. */
	ct_check(env_build_gus(&e, 0x7ee20003u, 3u, 0u), "GUS #3 builds (no MD.NXU)");
	(void)env_feed(&e);
	(void)vms_mscp_gus_end_parse(e.fake.end_frame,
				     (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.hdr.unit, 3u, "without MD.NXU the exact unit answers");
	ct_check(env_build_gus(&e, 0x7ee20004u, 1u, 0u), "GUS #4 builds for a gap");
	(void)env_feed(&e);
	(void)vms_mscp_gus_end_parse(e.fake.end_frame,
				     (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_OFFLINE,
			"and a unit number nothing is mounted at is Unit-Offline");
}

/* ------------------------------------------------------------------ *
 * 3. ONLINE
 * ------------------------------------------------------------------ */
static void test_online(void)
{
	struct srv_env e;
	struct vms_mscp_online_end end;

	printf("-- ONLINE: the unit comes online TO THIS class driver\n");
	env_init(&e);
	env_add_unit(&e, 0u, 24u, 0);
	env_open(&e);

	ct_check(env_build_online(&e, 0x900001u, 0u, 0u, 0x8000u),
		 "the ONLINE command builds");
	(void)env_feed(&e);
	ct_check_eq_u32(e.fake.end_len, VMS_MSCP_ONLINE_END_LEN,
			"the ONLINE end message is the MEASURED 44 bytes");
	ct_check_eq_u32((unsigned long)vms_mscp_online_end_parse(e.fake.end_frame,
								 (uint32_t)sizeof(e.fake.end_frame),
								 &end),
			VMS_CODEC_OK, "and it parses as an ONLINE END");
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_SUCCESS, "Success");
	ct_check_eq_u32(end.eh.status_subcode, VMS_MSCP_SUB_NORMAL, "sub-code Normal");
	ct_check_eq_u32(end.unit_size, 24u,
			"P.UNSZ is the VOLUME's real block count, from the executive");
	ct_check_eq_u32(end.unit_flags & 0x8000u, 0x8000u,
			"P.UNFL echoes the host's own word (the measured echo rule)");
	ct_check_eq_u32((unsigned long)end.volume_ser, 0,
			"P.VSER is an honest zero: the executive holds no volume serial");
	ct_check(e.fsm.vser_absent > 0u,
		 "...and the absence is COUNTED, not silently zeroed");

	/* sec 6.13: a second ONLINE is Success (Already Online), a DISTINCT
	 * sub-code, and the state is unaltered. */
	ct_check(env_build_online(&e, 0x900002u, 0u, 0u, 0u), "a second ONLINE builds");
	(void)env_feed(&e);
	(void)vms_mscp_online_end_parse(e.fake.end_frame,
					(uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_SUCCESS, "still Success");
	ct_check_eq_u32(end.eh.status_subcode, VMS_MSCP_SUB_ALREADY_ONLINE,
			"sec 6.13: sub-code Already Online (8), composed 0x0100");

	/* A unit the executive does not hold. */
	ct_check(env_build_online(&e, 0x900003u, 9u, 0u, 0u), "ONLINE of unit 9 builds");
	(void)env_feed(&e);
	(void)vms_mscp_online_end_parse(e.fake.end_frame,
					(uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_OFFLINE,
			"a unit that is not mounted is Unit-Offline, never a fake success");
}

/* ------------------------------------------------------------------ *
 * 4. READ -- real blocks, real byte count, the end on the transfer
 * ------------------------------------------------------------------ */
static void test_read(void)
{
	struct srv_env e;
	struct vms_mscp_xfer_end end;

	printf("-- READ: the volume's real blocks, and the end on the transfer\n");
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 0);
	env_open(&e);

	/* Before ONLINE, sec 4.3: not a status query, so it is refused. */
	ct_check(env_build_xfer(&e, VMS_MSCP_OP_READ, 0x210001u, 0u, 512u, 1u,
				0x40u, 0x0200001bu, 0x00020007u),
		 "the READ command builds through the codec");
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_AVAILABLE,
			"sec 4.3: a READ before ONLINE is refused Unit-Available");
	ct_check_eq_u32(e.fake.reads, 0, "and NOT one block was read");

	/* ONLINE, then READ two blocks from LBN 1 (the home block's own place). */
	(void)env_build_online(&e, 0x900001u, 0u, 0u, 0u);
	(void)env_feed(&e);
	ct_check(env_build_xfer(&e, VMS_MSCP_OP_READ, 0x210002u, 0u, 1024u, 1u,
				0x40u, 0x0200001bu, 0x00020007u),
		 "the second READ builds");
	(void)env_feed(&e);

	ct_check_eq_u32(e.fake.reads, 1, "the block layer was read exactly once");
	ct_check_eq_u32(e.fake.xfers, 1, "one block transfer was started");
	ct_check_eq_u32(e.fake.xfer_len, 1024u, "carrying the REAL byte count");
	ct_check_eq_u32(e.fake.xfer_remote_name, 0x0200001bu,
			"the remote buffer NAME came off the host's own descriptor");
	ct_check_eq_u32(e.fake.xfer_remote_offset, 0x40u,
			"...and so did its offset");
	ct_check_eq_u32(e.fake.xfer_conid, 0x00020007u,
			"...and the connection id it named");
	ct_check(memcmp(e.fake.xfer_data,
			e.fake.units[0].data + MSCP_SRV_BLOCK_SIZE, 1024u) == 0,
		 "the bytes on the wire ARE the volume's bytes at LBN 1");

	ct_check_eq_u32(e.fake.end_len, VMS_MSCP_READ_END_LEN,
			"the READ end message is the MEASURED 32 bytes");
	ct_check_eq_u32((unsigned long)vms_mscp_read_end_parse(e.fake.end_frame,
							       (uint32_t)sizeof(e.fake.end_frame),
							       &end),
			VMS_CODEC_OK, "and it parses as a READ END");
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_SUCCESS, "Success");
	ct_check_eq_u32(end.byte_count, 1024u,
			"P.BCNT is what ACTUALLY moved");
	ct_check_eq_u32(end.eh.hdr.cmd_ref, 0x210002u, "P.CRF echoed");

	/* A block-layer failure is a REAL error status, not a zero-length
	 * success. */
	e.fake.read_fail_next = 1u;
	(void)env_build_xfer(&e, VMS_MSCP_OP_READ, 0x210003u, 0u, 512u, 0u,
			     0u, 0x0200001bu, 0x00020007u);
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_DRIVE_ERR,
			"a failed block read is a real Drive Error");
	ct_check_eq_u32(end.byte_count, 0,
			"with a zero byte count -- never a success it cannot back up");
	ct_check_eq_u32(e.fsm.blockdev_failures, 1, "and it is counted");
}

/* ------------------------------------------------------------------ *
 * 5. READ / WRITE argument gates (Table B-2's field-naming Invalid Command)
 * ------------------------------------------------------------------ */
static void test_xfer_gates(void)
{
	struct srv_env e;
	struct vms_mscp_xfer_end end;

	printf("-- transfer gates: an Invalid Command NAMES the field in error\n");
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 0);
	env_open(&e);
	(void)env_build_online(&e, 0x900001u, 0u, 0u, 0u);
	(void)env_feed(&e);

	/* A byte count that is not a whole number of blocks (sec 5.3). */
	(void)env_build_xfer(&e, VMS_MSCP_OP_READ, 0x1u, 0u, 500u, 0u, 0u,
			     0x11u, 0x22u);
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status, VMS_MSCP_STATUS(VMS_MSCP_ST_INVALID_CMD,
			MSCP_SRV_SUB_INVALID_FIELD(MSCP_SRV_CMDOFF_BCNT)),
			"a partial-block P.BCNT is Invalid Command naming offset 12 (0x0C01)");

	/* An LBN past the volume. */
	(void)env_build_xfer(&e, VMS_MSCP_OP_READ, 0x2u, 0u, 512u, 99u, 0u,
			     0x11u, 0x22u);
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status, VMS_MSCP_STATUS(VMS_MSCP_ST_INVALID_CMD,
			MSCP_SRV_SUB_INVALID_FIELD(MSCP_SRV_CMDOFF_LBN)),
			"an LBN past the volume names offset 28 (0x1C01)");

	/* A byte count past the staging buffer: refused, never truncated. */
	(void)env_build_xfer(&e, VMS_MSCP_OP_READ, 0x3u, 0u,
			     e.fsm.xferbuf_slot + MSCP_SRV_BLOCK_SIZE,
			     0u, 0u, 0x11u, 0x22u);
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_INVALID_CMD,
			"a transfer past ONE staging SLOT is refused, not truncated");

	/* A host that named no buffer: nowhere to put the data, and this server
	 * will not mint a name for it. */
	(void)env_build_xfer(&e, VMS_MSCP_OP_READ, 0x4u, 0u, 512u, 0u, 0u,
			     0u, 0x22u);
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_HOST_BUF_ERR,
			"an absent host buffer name is Host Buffer Access Error");
	ct_check_eq_u32(e.fake.xfers, 0, "and no transfer was started");
}

/* ------------------------------------------------------------------ *
 * 6. WRITE on a WRITABLE volume: the data really lands
 * ------------------------------------------------------------------ */
static void test_write(void)
{
	struct srv_env e;
	struct vms_mscp_xfer_end end;
	uint32_t i;

	printf("-- WRITE: the peer's bytes reach the volume, then the end goes out\n");
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 0);
	env_open(&e);
	(void)env_build_online(&e, 0x900001u, 0u, 0u, 0u);
	(void)env_feed(&e);

	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x220001u, 0u, 512u, 4u,
			     0x80u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	ct_check(e.fake.write_name != 0u,
		 "a DESTINATION buffer was named for the peer to fill");
	ct_check_eq_u32(e.fake.write_len, 512u, "sized to the command's byte count");
	ct_check_eq_u32(e.fake.ends, 1,
			"NO end message yet -- the data has not arrived (only the ONLINE end so far)");

	/* The peer's port fills the buffer and the completion arrives. */
	for (i = 0; i < 512u; i++)
		e.fake.write_buf[i] = (uint8_t)(0xA0u + (i & 0x0fu));
	mscp_srv_fsm_block_data(&e.fsm, e.fake.write_name, 0u, 512u, 512u);

	ct_check_eq_u32(e.fake.writes, 1, "the block layer was written exactly once");
	ct_check(memcmp(e.fake.units[0].data + (4u * MSCP_SRV_BLOCK_SIZE),
			e.fake.write_buf, 512u) == 0,
		 "and the peer's bytes are on the volume at the LBN it asked for");
	ct_check_eq_u32(e.fake.end_len, VMS_MSCP_WRITE_END_LEN,
			"the WRITE end message is the MEASURED 36 bytes (NOT READ's 32)");
	ct_check_eq_u32((unsigned long)vms_mscp_write_end_parse(e.fake.end_frame,
								(uint32_t)sizeof(e.fake.end_frame),
								&end),
			VMS_CODEC_OK, "and it parses as a WRITE END");
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_SUCCESS, "Success");
	ct_check_eq_u32(end.byte_count, 512u, "P.BCNT is what really landed");
	ct_check_eq_u32(end.eh.hdr.cmd_ref, 0x220001u, "P.CRF echoed");
	ct_check(e.fake.buffers_released > 0u,
		 "the named buffer was released with its request");

	/* A completion that delivered LESS than one block did not deliver the
	 * data: answered Host Buffer Access Error, never a zero-length success,
	 * and nothing partial is committed to the volume. */
	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x220002u, 0u, 512u, 5u,
			     0u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	mscp_srv_fsm_block_data(&e.fsm, e.fake.write_name, 0u, 100u, 100u);
	(void)vms_mscp_write_end_parse(e.fake.end_frame,
				       (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_HOST_BUF_ERR,
			"a short transfer is a Host Buffer Access Error");
	ct_check_eq_u32(end.byte_count, 0, "with a zero byte count");
	ct_check_eq_u32(e.fake.writes, 1,
			"and NOT one partial block reached the volume");
}

/* ------------------------------------------------------------------ *
 * 6b. Concurrent WRITEs get DISTINCT staging slots, and running out is a
 *     refusal -- never an overwrite
 *
 * An in-flight WRITE owns its staging slot from the command until the peer's
 * bytes arrive. With one shared buffer a second WRITE in that window would
 * scribble on the first one's data and the volume would receive the WRONG
 * BYTES -- a silent corruption. This proves the slots are disjoint, that the
 * table's exhaustion is an honest MSCP refusal, and that the two transfers
 * land where they were addressed.
 * ------------------------------------------------------------------ */
struct slot_probe {
	uint8_t *buf[MSCP_SRV_MAX_REQS + 1u];
	uint32_t name[MSCP_SRV_MAX_REQS + 1u];
	uint32_t n;
};

static struct slot_probe g_slots;

static int probe_recv_write_data(void *ctx, vms_conid_t conid,
				 vms_scs_sysid_t peer,
				 const struct mscp_srv_bufdesc *desc,
				 uint8_t *buf, uint32_t len, uint32_t *name_out)
{
	struct fake_exec *e = (struct fake_exec *)ctx;

	(void)conid; (void)peer; (void)desc; (void)len;
	if (g_slots.n <= MSCP_SRV_MAX_REQS) {
		g_slots.buf[g_slots.n] = buf;
		g_slots.name[g_slots.n] = 0x02000000u + g_slots.n;
		*name_out = g_slots.name[g_slots.n];
		e->write_buf = buf;
		e->write_name = *name_out;
		g_slots.n++;
		return 0;
	}
	return -1;
}

static void test_write_slots(void)
{
	struct srv_env e;
	struct vms_mscp_xfer_end end;
	uint32_t i, j;

	printf("-- staging slots: concurrent WRITEs never share bytes\n");
	memset(&g_slots, 0, sizeof(g_slots));
	env_init(&e);
	e.ops.recv_write_data = probe_recv_write_data;
	env_add_unit(&e, 0u, 64u, 0);
	env_open(&e);
	(void)env_build_online(&e, 0x900001u, 0u, 0u, 0u);
	(void)env_feed(&e);

	/* Fill every HRB with an outstanding WRITE. */
	for (i = 0; i < MSCP_SRV_MAX_REQS; i++) {
		(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x300000u + i, 0u,
				     512u, i, 0u, 0x0200002cu, 0x00020007u);
		(void)env_feed(&e);
	}
	ct_check_eq_u32(g_slots.n, MSCP_SRV_MAX_REQS,
			"every HRB took a staging slot");
	for (i = 0; i < g_slots.n; i++) {
		for (j = i + 1u; j < g_slots.n; j++)
			ct_check(g_slots.buf[i] != g_slots.buf[j],
				 "...and no two of them are the same memory");
	}

	/* One more: no HRB left. It is REFUSED with a real end message. */
	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x3000ffu, 0u, 512u, 9u,
			     0u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	ct_check_eq_u32(g_slots.n, MSCP_SRV_MAX_REQS,
			"the overflow WRITE named NO buffer");
	ct_check_eq_u32((unsigned long)vms_mscp_write_end_parse(e.fake.end_frame,
								(uint32_t)sizeof(e.fake.end_frame),
								&end),
			VMS_CODEC_OK, "it got a real WRITE END back");
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_CTLR_ERR,
			"...refused, not staged over another request's bytes");
	ct_check_eq_u32(e.fsm.reqs_refused_busy, 1, "and counted");

	/* Now complete two of them with DIFFERENT data and check both landed
	 * at their own LBN -- the corruption a shared buffer would cause. */
	memset(g_slots.buf[0], 0x11, 512u);
	memset(g_slots.buf[1], 0x22, 512u);
	mscp_srv_fsm_block_data(&e.fsm, g_slots.name[0], 0u, 512u, 512u);
	mscp_srv_fsm_block_data(&e.fsm, g_slots.name[1], 0u, 512u, 512u);
	ct_check_eq_u32(e.fake.writes, 2, "both completed");
	ct_check_eq_u32(e.fake.units[0].data[0], 0x11,
			"LBN 0 got the FIRST request's bytes");
	ct_check_eq_u32(e.fake.units[0].data[MSCP_SRV_BLOCK_SIZE], 0x22,
			"LBN 1 got the SECOND request's bytes");
}

/* ------------------------------------------------------------------ *
 * 7. WRITE PROTECTION -- the plan row's own clause
 * ------------------------------------------------------------------ */
static void test_write_protect(void)
{
	struct srv_env e;
	struct vms_mscp_xfer_end xend;
	struct vms_mscp_online_end oend;

	printf("-- write protect: the REAL status, and not one block written\n");

	/* (a) the VOLUME's own read-only state -> Table B-2 sub-code 256. */
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 1 /* read-only */);
	env_open(&e);
	(void)env_build_online(&e, 0x900001u, 0u, 0u, 0u);
	(void)env_feed(&e);
	(void)vms_mscp_online_end_parse(e.fake.end_frame,
					(uint32_t)sizeof(e.fake.end_frame), &oend);
	ct_check_eq_u32(oend.unit_flags & VMS_MSCP_UF_WRITE_PROT_HW,
			VMS_MSCP_UF_WRITE_PROT_HW,
			"the protection is ADVERTISED in P.UNFL before the host tries");

	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x220009u, 0u, 512u, 0u,
			     0u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	ct_check_eq_u32((unsigned long)vms_mscp_write_end_parse(e.fake.end_frame,
								(uint32_t)sizeof(e.fake.end_frame),
								&xend),
			VMS_CODEC_OK, "the refusal is a real WRITE END");
	ct_check_eq_u32(xend.eh.status,
			VMS_MSCP_STATUS(VMS_MSCP_ST_WRITE_PROT,
					VMS_MSCP_SUB_WP_HARDWARE),
			"Table B-2: unit write protected, reason 256 -> 0x2006");
	ct_check_eq_u32(xend.byte_count, 0, "byte count 0 -- nothing moved");
	ct_check_eq_u32(e.fake.writes, 0, "and NOT one block was written");
	ct_check_eq_u32(e.fake.write_name, 0,
			"no destination buffer was even named");
	ct_check_eq_u32(e.fsm.write_protect_refusals, 1, "the refusal is counted");

	/* (b) SOFTWARE protection the HOST asked for with MD.SWP -> sub-code
	 * 128, on a volume that is NOT read-only. */
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 0 /* writable */);
	env_open(&e);
	(void)env_build_online(&e, 0x900002u, 0u, VMS_MSCP_MOD_SET_WRITE_PROT,
			       0u);
	(void)env_feed(&e);
	(void)vms_mscp_online_end_parse(e.fake.end_frame,
					(uint32_t)sizeof(e.fake.end_frame), &oend);
	ct_check_eq_u32(oend.unit_flags & VMS_MSCP_UF_WRITE_PROT_SW,
			VMS_MSCP_UF_WRITE_PROT_SW,
			"MD.SWP became a REAL UF.WPS in the ONLINE end");

	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x22000au, 0u, 512u, 0u,
			     0u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	(void)vms_mscp_write_end_parse(e.fake.end_frame,
				       (uint32_t)sizeof(e.fake.end_frame), &xend);
	ct_check_eq_u32(xend.eh.status,
			VMS_MSCP_STATUS(VMS_MSCP_ST_WRITE_PROT,
					VMS_MSCP_SUB_WP_SOFTWARE),
			"Table B-2: reason 128 -> 0x1006, the host's OWN request honoured");
	ct_check_eq_u32(e.fake.writes, 0, "still not one block written");

	/* (c) BOTH reasons: the sub-code is Table B-2's own BIT FLAGS. */
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 1 /* read-only */);
	env_open(&e);
	(void)env_build_online(&e, 0x900003u, 0u, VMS_MSCP_MOD_SET_WRITE_PROT,
			       0u);
	(void)env_feed(&e);
	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x22000bu, 0u, 512u, 0u,
			     0u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	(void)vms_mscp_write_end_parse(e.fake.end_frame,
				       (uint32_t)sizeof(e.fake.end_frame), &xend);
	ct_check_eq_u32(xend.eh.status,
			VMS_MSCP_STATUS(VMS_MSCP_ST_WRITE_PROT,
					VMS_MSCP_SUB_WP_SOFTWARE +
					VMS_MSCP_SUB_WP_HARDWARE),
			"both reasons OR together (Table B-2: the sub-code is bit flags)");

	/* A READ of a write-protected volume still works: protection is about
	 * WRITING (sec 6.14 does not list Write Protected among READ's codes). */
	(void)env_build_xfer(&e, VMS_MSCP_OP_READ, 0x21000cu, 0u, 512u, 0u,
			     0u, 0x0200001bu, 0x00020007u);
	(void)env_feed(&e);
	(void)vms_mscp_read_end_parse(e.fake.end_frame,
				      (uint32_t)sizeof(e.fake.end_frame), &xend);
	ct_check_eq_u32(xend.eh.status_major, VMS_MSCP_ST_SUCCESS,
			"a READ of a write-protected volume still succeeds");
}

/* ------------------------------------------------------------------ *
 * 8. The controller-state gate, and an opcode with no class of its own
 * ------------------------------------------------------------------ */
static void test_controller_gate(void)
{
	struct srv_env e;
	struct vms_mscp_end_hdr eh;
	struct vms_mscp_xfer_end probe;

	printf("-- sec 4.1: no connection, no controller -- and no invented answer\n");
	env_init(&e);
	env_add_unit(&e, 0u, 8u, 0);
	(void)mscp_srv_fsm_refresh_units(&e.fsm);

	/* No conn_open: there is no HQB, so there is no Controller-Online
	 * controller for the command to be addressed to. */
	(void)env_build_scc(&e, 0x1u, 0u, 0u);
	ct_check(env_feed(&e) != 0,
		 "a command with no connection behind it is NOT taken");
	ct_check_eq_u32(e.fake.ends, 0, "and nothing was answered");
	ct_check_eq_u32(e.fsm.cmds_no_hqb, 1, "it is counted instead");

	/* With the connection open, an opcode this server has no class for is
	 * ANSWERED Invalid Command -- carrying that opcode's OWN endcode. */
	mscp_srv_fsm_conn_open(&e.fsm, FAKE_CONID, FAKE_PEER);
	(void)env_build_gus(&e, 0x5u, 0u, 0u);
	e.cmd[VMS_OFF_MSCP_OPCD] = 0x11u;   /* an opcode with no class here */
	(void)env_feed(&e);
	ct_check_eq_u32(e.fake.end_len, VMS_MSCP_GENERIC_END_LEN,
			"the refusal is Table A-7's generic 32-byte end message");
	memset(&eh, 0, sizeof(eh));
	ct_check_eq_u32((unsigned long)vms_mscp_read_end_parse(e.fake.end_frame,
							       (uint32_t)sizeof(e.fake.end_frame),
							       &probe),
			VMS_CODEC_E_CLASS,
			"it is NOT a READ end: the endcode is the arriving opcode's own");
	ct_check_eq_u32(e.fake.end_frame[VMS_OFF_MSCP_OPCD],
			0x11u | VMS_MSCP_END_BIT,
			"...which is 0x11 | OP.END, never another class's endcode");

	/* The connection closes: sec 4.1's "no outstanding commands leftover
	 * from a previous incarnation". */
	mscp_srv_fsm_conn_closed(&e.fsm, FAKE_CONID);
	ct_check(mscp_srv_fsm_hqb_at(&e.fsm, 0) == NULL,
		 "the HQB is gone with its connection");
	(void)env_build_gus(&e, 0x6u, 0u, 0u);
	ct_check(env_feed(&e) != 0, "and a command on it is no longer serviced");
}

/* ------------------------------------------------------------------ *
 * 9. "MSCP$DISK only when a serveable unit exists" -- the DECISION
 * ------------------------------------------------------------------ */
static void test_registration_predicate(void)
{
	struct srv_env e;

	printf("-- registration: a function of REAL executive state\n");

	/* No volume mounted -> no UQB -> the glue has nothing to advertise. */
	env_init(&e);
	ct_check_eq_u32(mscp_srv_fsm_refresh_units(&e.fsm), 0,
			"no mounted volume -> no served unit");

	/* A volume the executive reports with a ZERO size or a zero unit id is
	 * not a volume: sec 6.12 makes a zero unit identifier mean "virtually
	 * no characteristics are valid", so it is refused rather than served. */
	env_init(&e);
	env_add_unit(&e, 0u, 0u /* zero blocks */, 0);
	ct_check_eq_u32(mscp_srv_fsm_refresh_units(&e.fsm), 0,
			"a zero-length volume is NOT served");

	/* A real one is. */
	env_init(&e);
	env_add_unit(&e, 0u, 8u, 0);
	ct_check_eq_u32(mscp_srv_fsm_refresh_units(&e.fsm), 1,
			"a real mounted volume IS served");

	/* ...and when it goes away, the count really drops (which is what the
	 * glue's beat unlistens on). */
	e.fake.n_units = 0u;
	ct_check_eq_u32(mscp_srv_fsm_refresh_units(&e.fsm), 0,
			"a dismounted volume stops being served");
}

/* ------------------------------------------------------------------ *
 * 10. The served unit's name and number
 * ------------------------------------------------------------------ */
static void test_unit_naming(void)
{
	char name[VMS_MSCP_SRV_NAME_MAX];
	uint16_t unit = 0xffffu;

	printf("-- $<ALLOCLASS>$DUAn, built from real values\n");

	vms_mscp_srv_unit_name(1u, 0u, name, sizeof(name));
	ct_check(strcmp(name, "$1$DUA0:") == 0, "alloclass 1, unit 0 -> $1$DUA0:");
	vms_mscp_srv_unit_name(255u, 4095u, name, sizeof(name));
	ct_check(strcmp(name, "$255$DUA4095:") == 0,
		 "alloclass 255, unit 4095 -> $255$DUA4095:");

	ct_check_eq_u32((unsigned long)vms_mscp_srv_unit_from_devnam("DKA0:", &unit),
			0, "DKA0: carries a unit number");
	ct_check_eq_u32(unit, 0, "...and it is 0");
	ct_check_eq_u32((unsigned long)vms_mscp_srv_unit_from_devnam("VDA12:", &unit),
			0, "VDA12: parses");
	ct_check_eq_u32(unit, 12, "...as unit 12");
	ct_check(vms_mscp_srv_unit_from_devnam("MBA:", &unit) != 0,
		 "a name with NO unit number is refused -- never served under a made-up one");
	ct_check(vms_mscp_srv_unit_from_devnam("SYS$SYSDEVICE:", &unit) != 0,
		 "and neither is a name whose digits are not a trailing unit");
}

/* ------------------------------------------------------------------ *
 * 11. The reaper: a request whose data never came
 * ------------------------------------------------------------------ */
static void test_request_timeout(void)
{
	struct srv_env e;
	struct vms_mscp_xfer_end end;

	printf("-- the reaper: an unfinished request is ABORTED, not leaked\n");
	env_init(&e);
	env_add_unit(&e, 0u, 16u, 0);
	env_open(&e);
	(void)env_build_online(&e, 0x900001u, 0u, 0u, 0u);
	(void)env_feed(&e);

	(void)env_build_xfer(&e, VMS_MSCP_OP_WRITE, 0x220050u, 0u, 512u, 0u,
			     0u, 0x0200002cu, 0x00020007u);
	(void)env_feed(&e);
	ct_check_eq_u32(mscp_srv_fsm_tick(&e.fsm), 0,
			"a fresh request is not reaped");

	e.fake.now_ms += MSCP_SRV_REQ_TIMEOUT_MS;
	ct_check_eq_u32(mscp_srv_fsm_tick(&e.fsm), 1,
			"...but one past the controller timeout is");
	(void)vms_mscp_write_end_parse(e.fake.end_frame,
				       (uint32_t)sizeof(e.fake.end_frame), &end);
	ct_check_eq_u32(end.eh.status_major, VMS_MSCP_ST_ABORTED,
			"and it is answered Command Aborted, so the host is not left waiting");
	ct_check_eq_u32(e.fake.writes, 0, "nothing was written from an empty buffer");
	ct_check_eq_u32(e.fsm.reqs_aborted, 1, "counted");
}

/* ------------------------------------------------------------------ *
 * 12. The SHIPPING glue's bindings, read out of the real source
 *
 * vms_mscp_srv.c names exec_kbackend.h and the FC-P0.5 fork API, so it cannot
 * be host-linked (test_cnxman_glue.c / test_scs_glue_conn.c set the precedent).
 * This proof reads the SHIPPED file and asserts the bindings the plan row and
 * the design require are the ones that are actually there.
 * ------------------------------------------------------------------ */
static char *slurp(const char *path)
{
	static char buf[262144];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL)
		return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

static void test_glue_source(void)
{
	const char *srcs[] = { OVMX_KCORE_DIR "/vms_mscp_srv.c" };
	char *s;

	printf("-- the SHIPPING glue: its bindings, read out of the source\n");
	s = slurp(srcs[0]);
	if (s == NULL) {
		ct_check(0, "vms_mscp_srv.c is readable");
		return;
	}

	ct_check(strstr(s, "vms_acp_volume_at") != NULL,
		 "units come from the ODS-2 ACP's own mounted-volume table");
	ct_check(strstr(s, "exec_blockdev_read_block") != NULL,
		 "READ goes to the executive's block seam");
	ct_check(strstr(s, "exec_blockdev_write_block") != NULL,
		 "WRITE goes to the executive's block seam");
	ct_check(strstr(s, "pe_send_block_read_end") != NULL,
		 "READ data rides the port's THIRD service (FC-P6.1)");
	ct_check(strstr(s, "pe_buf_register") != NULL,
		 "buffers are named by the PORT, never by this layer");
	ct_check(strstr(s, "scs_send_msg") != NULL,
		 "end messages go out through SCS on a real CDT");
	ct_check(strstr(s, "scs_sysap_listen") != NULL &&
		 strstr(s, "scs_sysap_unlisten") != NULL,
		 "MSCP$DISK is both registered AND withdrawn");
	ct_check(strstr(s, "cnxman_join_name_mscp_disk") != NULL,
		 "under the ONE `MSCP$DISK` name the stack already defines");
	ct_check(strstr(s, "mscp_srv_fsm_refresh_units") != NULL &&
		 strstr(s, "srv_unlisten") != NULL,
		 "the registration is re-evaluated against the live unit count");
	ct_check(strstr(s, "params.mscp_load") != NULL &&
		 strstr(s, "params.mscp_serve_all") != NULL,
		 "MSCP_LOAD and MSCP_SERVE_ALL are read from SYSGEN, not defaulted");
	ct_check(strstr(s, "params.scssystemid") != NULL,
		 "the controller identity is this node's real SCSSYSTEMID");
	ct_check(strstr(s, "vms_scs_set_block_consumer") != NULL,
		 "block-transfer completions are routed to this server");
	ct_check(strstr(s, "CF_OWNER_MSCP") != NULL,
		 "the beat rides the FC-P0.5 fork context, not a raw timer");

	/* The negative half: the glue must NOT be where MSCP is decided. */
	ct_check(strstr(s, "VMS_MSCP_STATUS(") == NULL,
		 "the glue composes NO MSCP status -- the pure server does");
	ct_check(strstr(s, "_end_build") == NULL,
		 "and it builds NO end message");
}

/* ------------------------------------------------------------------ *
 * 13. R4-LITE: a SIMULATED CLASS DRIVER runs the discovery sequence
 *
 * The plan row offers R4-lite as "a simulated class driver in cluster_sim runs
 * the mount-verify sequence". This is that, at the rung where it costs nothing
 * and proves the most: the class driver is FC-P3.4's REAL discovery FSM
 * (vms_mscp_cl_fsm.c -- the same one CNXMAN's join drives), talking to
 * FC-P6.3's REAL server, command for command. Neither side is a fixture: the
 * client builds what it would put on the wire, the server answers from its own
 * state, and the client PARSES that answer back through its own classifier --
 * which means the answer has to be a well-formed, correctly-classed MSCP end
 * message of the right SCA length, or the walk stops dead.
 *
 * The lab leg it does NOT replace -- a real VAX MOUNTing an OVMX-served unit,
 * and the byte-exact ONLINE-END oracle -- is FC-P6.4's, and its harness is
 * tests/lab/mscp_serve_mount.sh (lab-deferred, exit 77).
 * ------------------------------------------------------------------ */
static void interop_wrap_end(const struct fake_exec *fake, uint8_t *frame,
			     uint32_t cap, uint32_t *len_out)
{
	struct vms_mscp_link link;
	uint16_t sca = VMS_MSCP_END_SCA_LEN(fake->end_len);

	memset(&link, 0, sizeof(link));
	memset(frame, 0, cap);
	/* The envelope a live connection would supply. The class driver's
	 * classifier needs a REAL SCA header of the end message's own length --
	 * which is exactly what makes this an interop proof and not a
	 * structure-blind byte comparison. */
	(void)vms_mscp_link_build(&link, sca, frame, cap, len_out);
	memcpy(frame + VMS_OFF_SYSAP_BODY,
	       fake->end_frame + VMS_OFF_SYSAP_BODY, fake->end_len);
	*len_out = VMS_ETH_HDR_LEN + sca;
}

/*
 * A RECORDED INTEROP FACT, asserted so it cannot drift silently.
 *
 * FC-P3.4's discovery walk seeds unit word 1, never 0 (VMS_MSCP_CL_GUS_SEED_UNIT
 * -- vms_mscp_cl_fsm.h's own note: "seeding 0x0000 makes the server answer
 * OFFLINE immediately and the enumeration terminates after one exchange").
 * sec 6.12's MD.NXU rule is "the next unit with a unit number >= the specified
 * unit number", so a unit numbered ZERO is BY CONSTRUCTION invisible to that
 * walk -- and OVMX derives a served unit's number from the executive's own
 * device name, where DKA0: really is unit 0.
 *
 * NEITHER SIDE IS PATCHED HERE. The client's seed is a MEASURED convention off
 * a real joiner, and the server's unit number is a REAL device fact; changing
 * either to make this case work would be inventing behaviour on one side to
 * flatter the other. It is asserted instead, so it is a stated consequence
 * rather than a surprise, and it is on FC-P6.4's lab list (does a real VMS
 * class driver reach a unit 0 at all, and if so with what seed?).
 */
static void test_unit0_is_unreachable_by_the_seeded_walk(void)
{
	struct srv_env e;
	struct vms_mscp_cl_fsm cl;
	struct vms_mscp_link link;
	uint8_t frame[512];
	uint32_t len = 0u;
	struct vms_mscp_cl_unit unit;
	int terminator = 0;

	env_init(&e);
	env_add_unit(&e, 0u, 64u, 0);
	env_open(&e);
	vms_mscp_cl_fsm_init(&cl);
	memset(&link, 0, sizeof(link));
	cl.state = VMS_MSCP_CL_ST_GUS_READY;   /* past the two SCCs */

	(void)vms_mscp_cl_fsm_build_gus(&cl, &link, frame,
					(uint32_t)sizeof(frame), NULL);
	(void)mscp_srv_fsm_command(&e.fsm, FAKE_CONID,
				   frame + VMS_OFF_SYSAP_BODY,
				   VMS_MSCP_CMD_BODY_LEN);
	interop_wrap_end(&e.fake, frame, (uint32_t)sizeof(frame), &len);
	memset(&unit, 0, sizeof(unit));
	(void)vms_mscp_cl_fsm_on_gus_end(&cl, frame, len, &unit, &terminator);
	ct_check(terminator == 1,
		 "RECORDED: the seed-1 walk cannot see a unit numbered 0 "
		 "(sec 6.12 MD.NXU is >=; FC-P6.4 lab question)");
	ct_check_eq_u32(cl.units_found, 0, "...so it enumerates nothing");
}

static void test_client_interop(void)
{
	struct srv_env e;
	struct vms_mscp_cl_fsm cl;
	struct vms_mscp_link link;
	uint8_t frame[512];
	uint32_t len = 0u;
	struct vms_mscp_cl_unit unit;
	int terminator = 0;
	int steps = 0;

	printf("-- R4-lite: FC-P3.4's REAL class driver walks FC-P6.3's REAL server\n");
	env_init(&e);
	env_add_unit(&e, 1u, 100u, 0);
	env_add_unit(&e, 3u, 200u, 0);
	env_open(&e);

	vms_mscp_cl_fsm_init(&cl);
	memset(&link, 0, sizeof(link));

	/* Step 1-2: SET CONTROLLER CHARACTERISTICS, twice (sec 4(n)). */
	while (cl.state != VMS_MSCP_CL_ST_GUS_READY && steps++ < 8) {
		ct_check_eq_u32((unsigned long)vms_mscp_cl_fsm_build_scc(
					&cl, &link, 0u, 0u, 0u, frame,
					(uint32_t)sizeof(frame), NULL),
				VMS_CODEC_OK, "the class driver built an SCC");
		ct_check_eq_u32((unsigned long)mscp_srv_fsm_command(
					&e.fsm, FAKE_CONID,
					frame + VMS_OFF_SYSAP_BODY,
					VMS_MSCP_CMD_BODY_LEN),
				0, "...the server took it");
		interop_wrap_end(&e.fake, frame, (uint32_t)sizeof(frame), &len);
		ct_check_eq_u32((unsigned long)vms_mscp_cl_fsm_on_scc_end(&cl,
									  frame, len),
				VMS_CODEC_OK,
				"...and the class driver ACCEPTED the answer "
				"(classified, P.CRF matched)");
	}
	ct_check_eq_u32(cl.state, VMS_MSCP_CL_ST_GUS_READY,
			"two SCCs later the walk is ready");

	/* Step 3: the GET UNIT STATUS NEXT-UNIT walk, to its terminator. */
	steps = 0;
	while (!vms_mscp_cl_fsm_done(&cl) && steps++ < 16) {
		ct_check_eq_u32((unsigned long)vms_mscp_cl_fsm_build_gus(
					&cl, &link, frame,
					(uint32_t)sizeof(frame), NULL),
				VMS_CODEC_OK, "the class driver built a GUS");
		(void)mscp_srv_fsm_command(&e.fsm, FAKE_CONID,
					   frame + VMS_OFF_SYSAP_BODY,
					   VMS_MSCP_CMD_BODY_LEN);
		interop_wrap_end(&e.fake, frame, (uint32_t)sizeof(frame), &len);
		memset(&unit, 0, sizeof(unit));
		ct_check_eq_u32((unsigned long)vms_mscp_cl_fsm_on_gus_end(
					&cl, frame, len, &unit, &terminator),
				VMS_CODEC_OK, "...and consumed the GUS END");
		if (!terminator)
			ct_check(unit.unit_id != 0u,
				 "...a real unit, with a non-zero identifier");
	}
	ct_check(vms_mscp_cl_fsm_done(&cl),
		 "the walk reached its Unit-Offline terminator");
	ct_check_eq_u32(cl.units_found, 2,
			"and enumerated BOTH served volumes -- no more, no fewer");
	test_unit0_is_unreachable_by_the_seeded_walk();
}

int main(void)
{
	printf("test_mscp_srv: FC-P6.3, the executive-resident MSCP disk server\n");
	test_scc();
	test_gus_walk();
	test_online();
	test_read();
	test_xfer_gates();
	test_write();
	test_write_slots();
	test_write_protect();
	test_controller_gate();
	test_registration_predicate();
	test_unit_naming();
	test_request_timeout();
	test_glue_source();
	test_client_interop();
	return ct_summary("test_mscp_srv");
}
