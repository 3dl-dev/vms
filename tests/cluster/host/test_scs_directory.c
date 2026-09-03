// SPDX-License-Identifier: GPL-2.0
/*
 * test_scs_dir.c - FC-P2.3's R1: the SYSAP registry read, the SCS$DIRECTORY
 * server's two answers, the SCS$DIR_LOOKUP client's round trip, and the
 * transient connection of *VAXcluster Principles* p. 2-51.
 *
 * WHAT "THE DIR SPECIMENS" ARE HERE. The reference directory frames are the
 * ones wire spec sec 4(h)(2)/(2a) decodes and
 * docs/design-cluster-join-choreography.md byte-verifies:
 *
 *   - a REQUEST carries marker `[58:62] = 0` and an ALL-ZERO 16-byte result;
 *   - a RESPONSE carries marker `[58:62] = 1` -- for a hit AND for a miss;
 *   - a MISS's result is the literal ASCII `"NOT PRESENT HERE"`;
 *   - an `MSCP$DISK` HIT's result is the queried name echoed blank-padded
 *     (`4d534350244449534b20202020202020`);
 *   - every one of them is MTYPE 10 on the 94-content SCA class.
 *
 * Each is asserted below against bytes OVMX actually put on the wire, read
 * back through the codec at the body level -- never against what a builder
 * intended.
 */

#include "cluster_test.h"
#include "scs_dir_test_harness.h"

/* ------------------------------------------------------------------ */

static uint8_t name_mscp_disk[VMS_SCS_PROCNAME_LEN];
static uint8_t name_mscp_tape[VMS_SCS_PROCNAME_LEN];
static uint8_t name_vaxcluster[VMS_SCS_PROCNAME_LEN];

static void names_init(void)
{
	(void)scs_dir_name_pad(name_mscp_disk, "MSCP$DISK");
	(void)scs_dir_name_pad(name_mscp_tape, "MSCP$TAPE");
	(void)scs_dir_name_pad(name_vaxcluster, "VMS$VAXcluster");
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/* Two nodes, statically allocated: a struct scsdh_node carries a whole CDL. */
static struct scsdh_node node_a;
static struct scsdh_node node_b;

static void two_nodes(void)
{
	scsh_wire_reset();
	scsdh_node_init(&node_a, 0x0101u, 0x4e63u);
	scsdh_node_init(&node_b, 0x0202u, 0xe2dcu);
	scsh_link(&node_a.n, &node_b.n);
}

/* ==========================================================================
 * 1. THE REGISTRY READ -- what a HIT actually is
 * ========================================================================== */
static void test_registry_read(void)
{
	struct scs_sysap_info info;

	printf("registry read (the list of listening SYSAPs, p. 2-50)\n");
	two_nodes();
	ct_check(scsdh_listen_directory(&node_b) == SCS_OK,
		 "SCS$DIRECTORY registers");
	ct_check(scsdh_listen_name(&node_b, name_mscp_disk) == SCS_OK,
		 "MSCP$DISK registers");

	ct_check(scs_fsm_sysap_lookup(&node_b.n.fsm, name_mscp_disk, &info) ==
		 SCS_OK, "a registered name is FOUND");
	ct_check(bytes_eq(info.name, name_mscp_disk, VMS_SCS_PROCNAME_LEN),
		 "... and the entry carries that exact 16-byte name");
	ct_check(info.listen_conid != 0u,
		 "... and the listening CDT's Con.ID (p. 2-48's SDIR content)");
	ct_check(info.dir_data_valid == 0u,
		 "... with NO declared directory data (honest omission)");

	ct_check(scs_fsm_sysap_lookup(&node_b.n.fsm, name_mscp_tape,
				      &info) == SCS_ERR_NOSYSAP,
		 "an unregistered name is NOT found");

	/* INV-6: an answer may not outlive the registration it describes. */
	ct_check(scs_fsm_unlisten(&node_b.n.fsm, name_mscp_disk) == SCS_OK,
		 "MSCP$DISK withdraws");
	ct_check(scs_fsm_sysap_lookup(&node_b.n.fsm, name_mscp_disk,
				      &info) == SCS_ERR_NOSYSAP,
		 "... and is immediately no longer found");
}

/* ==========================================================================
 * 2. THE SPECIMEN BODIES -- built and read back through the codec
 * ========================================================================== */
static void check_specimen_request(void)
{
	struct vms_scs_dir_msg m, back;
	uint8_t body[SCS_DIR_BODY_LEN];
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(m); i++)
		((uint8_t *)&m)[i] = 0u;
	m.marker = VMS_SCS_DIR_MARKER_REQUEST;
	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
		m.lookup.queried_name[i] = name_mscp_tape[i];
	m.lookup.result_kind = (uint8_t)VMS_SCS_DIR_RESULT_EMPTY;

	ct_check(vms_scs_dir_msg_build(&m, body, (uint32_t)sizeof(body)) ==
		 VMS_CODEC_OK, "a REQUEST body builds");
	ct_check(vms_scs_dir_msg_parse(body, (uint32_t)sizeof(body), &back) ==
		 VMS_CODEC_OK, "... and parses back");
	ct_check_eq_u32(back.marker, VMS_SCS_DIR_MARKER_REQUEST,
			"REQUEST marker at [58:62]");
	ct_check(bytes_eq(back.lookup.queried_name, name_mscp_tape,
			  VMS_SCSCTRL_NAME_LEN),
		 "... carries the queried name");
	ct_check_eq_u32(back.lookup.result_kind, VMS_SCS_DIR_RESULT_EMPTY,
			"... and an ALL-ZERO result (sec 4(h)(2))");
}

static void check_specimen_miss(void)
{
	struct vms_scs_dir_msg m, back;
	uint8_t body[SCS_DIR_BODY_LEN];
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(m); i++)
		((uint8_t *)&m)[i] = 0u;
	m.marker = VMS_SCS_DIR_MARKER_RESPONSE;
	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
		m.lookup.queried_name[i] = name_mscp_tape[i];
	m.lookup.result_kind = (uint8_t)VMS_SCS_DIR_RESULT_NOT_PRESENT;

	ct_check(vms_scs_dir_msg_build(&m, body, (uint32_t)sizeof(body)) ==
		 VMS_CODEC_OK, "a MISS body builds");
	ct_check(bytes_eq(&body[VMS_OFF_SCSDIRBODY_RESULT],
			  vms_scs_dir_not_present_here, VMS_SCSCTRL_NAME_LEN),
		 "... with the literal \"NOT PRESENT HERE\" at the result field");
	ct_check(vms_scs_dir_msg_parse(body, (uint32_t)sizeof(body), &back) ==
		 VMS_CODEC_OK, "... and parses back");
	ct_check_eq_u32(back.marker, VMS_SCS_DIR_MARKER_RESPONSE,
			"RESPONSE marker -- the SAME 1 a hit carries");
	ct_check_eq_u32(back.lookup.result_kind,
			VMS_SCS_DIR_RESULT_NOT_PRESENT, "read back as a MISS");
}

static void check_specimen_hit(void)
{
	struct vms_scs_dir_msg m, back;
	uint8_t body[SCS_DIR_BODY_LEN];
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(m); i++)
		((uint8_t *)&m)[i] = 0u;
	m.marker = VMS_SCS_DIR_MARKER_RESPONSE;
	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++) {
		m.lookup.queried_name[i] = name_mscp_disk[i];
		m.lookup.result[i] = name_mscp_disk[i];
	}
	m.lookup.result_kind = (uint8_t)VMS_SCS_DIR_RESULT_AFFIRMATIVE;

	ct_check(vms_scs_dir_msg_build(&m, body, (uint32_t)sizeof(body)) ==
		 VMS_CODEC_OK, "a HIT body builds");
	ct_check(vms_scs_dir_msg_parse(body, (uint32_t)sizeof(body), &back) ==
		 VMS_CODEC_OK, "... and parses back");
	ct_check_eq_u32(back.lookup.result_kind,
			VMS_SCS_DIR_RESULT_AFFIRMATIVE, "read back as a HIT");
	ct_check(!bytes_eq(back.lookup.result, vms_scs_dir_not_present_here,
			   VMS_SCSCTRL_NAME_LEN),
		 "... and is a HIT precisely because it is NOT the negative "
		 "literal");
}

static void test_specimens(void)
{
	printf("the directory specimens (sec 4(h)(2)/(2a))\n");
	check_specimen_request();
	check_specimen_miss();
	check_specimen_hit();
	ct_check_eq_u32(SCS_DIR_BODY_LEN, 36u,
			"the directory SYSAP body is 36 bytes (abs 72..107)");
	ct_check_eq_u32(SCS_DIR_FRAME_LEN, 108u,
			"... i.e. the 94-content SCA class");
}

/* ==========================================================================
 * 3. THE ROUND TRIP -- client asks, server answers off the registry
 * ========================================================================== */

/* The last directory RESPONSE `d` transmitted, decoded off the wire. */
static int last_response(const struct scsdh_node *d,
			 struct vms_scs_dir_msg *out)
{
	uint32_t i = d->n_tap;

	while (i > 0u) {
		i--;
		if (scsdh_tap_dir(d, i, out) == 0 &&
		    out->marker == VMS_SCS_DIR_MARKER_RESPONSE)
			return 0;
	}
	return -1;
}

static void test_round_trip(void)
{
	struct vms_scs_dir_msg ans;

	printf("client connect + lookup round trip\n");
	two_nodes();
	(void)scsdh_listen_directory(&node_b);
	(void)scsdh_listen_name(&node_b, name_mscp_disk);

	/* (a) a name nobody registered -> NOT PRESENT HERE */
	ct_check(scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_tape,
				scsdh_result, &node_a) == SCS_OK,
		 "MSCP$TAPE inquiry accepted");
	(void)scsh_pump();
	ct_check_eq_u32(node_a.n_results, 1u, "the client got exactly ONE answer");
	ct_check(node_a.last_present == 0, "... and it is NOT PRESENT HERE");
	ct_check(bytes_eq(node_a.last_name, name_mscp_tape,
			  VMS_SCS_PROCNAME_LEN),
		 "... about the name it asked");
	ct_check(last_response(&node_b, &ans) == 0,
		 "the server's answer is on the wire");
	ct_check(bytes_eq(ans.lookup.result, vms_scs_dir_not_present_here,
			  VMS_SCSCTRL_NAME_LEN),
		 "... carrying the literal negative, byte for byte");
	ct_check_eq_u32(node_b.dir.srv_misses, 1u, "the server counted a miss");

	/* (b) a registered name -> HIT, and the result is the registry's own
	 * name, which is the af2-verified MSCP$DISK affirmative shape. */
	ct_check(scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
				scsdh_result, &node_a) == SCS_OK,
		 "MSCP$DISK inquiry accepted");
	(void)scsh_pump();
	ct_check_eq_u32(node_a.n_results, 2u, "a second answer arrived");
	ct_check(node_a.last_present != 0, "... and it is a HIT");
	ct_check(last_response(&node_b, &ans) == 0, "the HIT is on the wire");
	ct_check(bytes_eq(ans.lookup.result, name_mscp_disk,
			  VMS_SCSCTRL_NAME_LEN),
		 "... result = the registered name echoed blank-padded");
	ct_check_eq_u32(node_b.dir.srv_hits, 1u, "the server counted a hit");

	/* Every directory frame rode the 94-content class. */
	{
		uint32_t i, dirframes = 0u, wrongclass = 0u;
		struct vms_scs_dir_msg m;

		for (i = 0; i < node_b.n_tap; i++) {
			if (scsdh_tap_dir(&node_b, i, &m) != 0)
				continue;
			dirframes++;
			if (scsdh_tap_content(&node_b, i) !=
			    VMS_SCSCTRL_LEN_LOOKUP)
				wrongclass++;
		}
		ct_check_eq_u32(dirframes, 2u, "the server sent 2 answers");
		ct_check_eq_u32(wrongclass, 0u,
				"... every one on the 94-content class");
	}
}

/* ==========================================================================
 * 4. A SYSAP-DECLARED AFFIRMATIVE DESCRIPTOR (and nothing baked in)
 * ========================================================================== */
static void test_declared_descriptor(void)
{
	static const uint8_t declared[VMS_SCS_PROCNAME_LEN] = {
		0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
		0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0
	};
	struct vms_scs_dir_msg ans;

	printf("an affirmative result the OWNING SYSAP declared\n");
	two_nodes();
	(void)scsdh_listen_directory(&node_b);
	(void)scsdh_listen_name(&node_b, name_vaxcluster);
	ct_check(scs_fsm_sysap_set_dir_data(&node_b.n.fsm, name_vaxcluster,
					    declared) == SCS_OK,
		 "the SYSAP declares its 16-byte directory data");

	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_vaxcluster,
			     scsdh_result, &node_a);
	(void)scsh_pump();
	ct_check(node_a.last_present != 0, "VMS$VAXcluster answers HIT");
	ct_check(last_response(&node_b, &ans) == 0, "the answer is on the wire");
	ct_check(bytes_eq(ans.lookup.result, declared, VMS_SCSCTRL_NAME_LEN),
		 "... carrying the DECLARED bytes, not the name and not a "
		 "captured template");

	ct_check(scs_fsm_sysap_set_dir_data(&node_b.n.fsm, name_vaxcluster,
					    (const uint8_t *)0) == SCS_OK,
		 "the declaration can be withdrawn");
	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_vaxcluster,
			     scsdh_result, &node_a);
	(void)scsh_pump();
	ct_check(last_response(&node_b, &ans) == 0, "and the next answer");
	ct_check(bytes_eq(ans.lookup.result, name_vaxcluster,
			  VMS_SCSCTRL_NAME_LEN),
		 "... falls back to the registered name, never to zeros");
}

/* ==========================================================================
 * 5. THE TRANSIENT CONNECTION (p. 2-51)
 * ========================================================================== */
static void test_transient_connection(void)
{
	struct scs_dir_peer *p;

	printf("the transient directory connection\n");
	two_nodes();
	(void)scsdh_listen_directory(&node_b);
	(void)scsdh_listen_name(&node_b, name_mscp_disk);

	ct_check(scs_dir_peer_by_sysid(&node_a.dir, node_b.n.sysid) ==
		 (struct scs_dir_peer *)0,
		 "no directory connection exists before an inquiry");

	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_tape,
			     scsdh_result, &node_a);
	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
			     scsdh_result, &node_a);
	(void)scsh_pump();

	p = scs_dir_peer_by_sysid(&node_a.dir, node_b.n.sysid);
	ct_check(p != (struct scs_dir_peer *)0, "a peer row now exists");
	ct_check_eq_u32(node_a.n_results, 2u, "both inquiries were answered");
	ct_check_eq_u32(p->state, SCS_DIR_IDLE,
			"... and the connection did NOT survive the round");
	ct_check_eq_u32(p->outstanding, 0u, "nothing left outstanding");
	ct_check_eq_u32(p->queued, 0u, "nothing left queued");
	ct_check_eq_u32(node_a.dir.cli_rounds, 1u, "exactly ONE round happened");
	ct_check_eq_u32(node_a.dir.cli_hits, 1u, "one hit");
	ct_check_eq_u32(node_a.dir.cli_misses, 1u, "one miss");
	ct_check_eq_u32(node_a.dir.cli_timeouts, 0u, "no timeouts");

	/* p. 2-51: they disconnect from EACH OTHER. Both ends let the CDT go. */
	ct_check(scs_fsm_cdt_by_conid(&node_a.n.fsm, p->conid) ==
		 (struct scs_cdt *)0,
		 "the client's CDT is released");
	ct_check_eq_u32(node_b.dir.srv_hits + node_b.dir.srv_misses, 2u,
			"the server answered both from the registry");

	/* A second round re-opens: that is what "periodically" means. */
	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
			     scsdh_result, &node_a);
	(void)scsh_pump();
	ct_check_eq_u32(node_a.dir.cli_rounds, 2u,
			"a later inquiry opens a NEW transient connection");
}

/* ==========================================================================
 * 6. SILENCE IS NOT A "No" (INV-6)
 *
 * Node B registers the NAME `SCS$DIRECTORY` with a SYSAP that has no message
 * input routine, so the connection forms and the inquiry is delivered to
 * nothing. That is the shape of a peer that accepts and then says nothing.
 * ========================================================================== */
static void test_timeout_is_not_absence(void)
{
	struct scs_dir_peer *p;
	struct scs_dir_cfg cfg;

	printf("an unanswered inquiry times out and reports NOTHING\n");
	scsh_wire_reset();
	scsdh_node_init(&node_a, 0x0303u, 0x1111u);
	scsdh_node_init(&node_b, 0x0404u, 0x2222u);
	scsh_link(&node_a.n, &node_b.n);

	cfg.lookup_timeout_ms = 1000u;
	cfg.credits = SCS_DIR_CREDITS_DEFAULT;
	cfg.pad0 = 0u;
	scs_dir_set_cfg(&node_a.dir, &cfg);

	/* registered, listening, and mute */
	ct_check(scsdh_listen_name(&node_b, scs_dir_name_directory) == SCS_OK,
		 "a mute SCS$DIRECTORY is registered on the peer");

	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_mscp_disk,
			     scsdh_result, &node_a);
	/* A SECOND inquiry, which the one-at-a-time rule keeps off the wire
	 * behind the first: it must expire on the same clock, or it would sit
	 * in the table forever behind an answer that never comes. */
	(void)scs_dir_inquire(&node_a.dir, node_b.n.sysid, name_vaxcluster,
			     scsdh_result, &node_a);
	(void)scsh_pump();
	p = scs_dir_peer_by_sysid(&node_a.dir, node_b.n.sysid);
	ct_check(p != (struct scs_dir_peer *)0, "the peer row exists");
	ct_check_eq_u32(p->state, SCS_DIR_OPEN, "the connection opened");
	ct_check_eq_u32(p->outstanding, 1u, "ONE inquiry is on the wire");
	ct_check_eq_u32(p->queued, 1u, "... and one waits behind it");
	ct_check_eq_u32(node_a.n_results, 0u, "no answer yet, and no callback");

	node_a.n.now_ms = 500u;
	scs_dir_tick(&node_a.dir);
	ct_check_eq_u32(node_a.dir.cli_timeouts, 0u,
			"before the deadline nothing expires");

	node_a.n.now_ms = 1500u;
	scs_dir_tick(&node_a.dir);
	(void)scsh_pump();
	ct_check_eq_u32(node_a.dir.cli_timeouts, 2u,
			"past it, BOTH the sent and the queued inquiry expire");
	ct_check_eq_u32(node_a.n_results, 0u,
			"and the callback was NEVER invoked -- silence is not "
			"a 'No'");
	ct_check_eq_u32(node_a.dir.cli_misses, 0u,
			"... and it was not counted as a miss either");
	ct_check_eq_u32(p->state, SCS_DIR_IDLE,
			"a round with nothing left on it closes");
	ct_check_eq_u32(p->queued, 0u, "no inquiry is left parked");
}

/* ==========================================================================
 * 7. A REQUEST ON THE WRONG HALF IS COUNTED, NOT ANSWERED
 * ========================================================================== */
static void test_wrong_role(void)
{
	struct vms_scs_dir_msg m;
	uint8_t body[SCS_DIR_BODY_LEN];
	uint32_t i;

	printf("role confusion is counted, never guessed at\n");
	two_nodes();
	(void)scsdh_listen_directory(&node_b);

	/* Hand the SERVER an answer instead of an inquiry. */
	for (i = 0; i < (uint32_t)sizeof(m); i++)
		((uint8_t *)&m)[i] = 0u;
	m.marker = VMS_SCS_DIR_MARKER_RESPONSE;
	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
		m.lookup.queried_name[i] = name_mscp_disk[i];
	m.lookup.result_kind = (uint8_t)VMS_SCS_DIR_RESULT_NOT_PRESENT;
	(void)vms_scs_dir_msg_build(&m, body, (uint32_t)sizeof(body));

	ct_check(node_b.dir.server_ops.message(&node_b.dir, 0u, body,
					       SCS_DIR_BODY_LEN) != 0,
		 "the server refuses an ANSWER as not consumed");
	ct_check_eq_u32(node_b.dir.rx_wrong_role, 1u, "... and counts it");
	ct_check_eq_u32(node_b.dir.srv_hits + node_b.dir.srv_misses, 0u,
			"... and answers nothing");

	ct_check(node_b.dir.server_ops.message(&node_b.dir, 0u, body, 4u) != 0,
		 "a short body is refused");
	ct_check_eq_u32(node_b.dir.rx_malformed, 1u, "... and counted");
}

int main(void)
{
	names_init();
	test_registry_read();
	test_specimens();
	test_round_trip();
	test_declared_descriptor();
	test_transient_connection();
	test_timeout_is_not_absence();
	test_wrong_role();
	return ct_summary("scs_dir");
}
