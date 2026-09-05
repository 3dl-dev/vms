/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_replay.c - FC-P1.5's own done-condition: replay a reference join
 * capture through ONE simulated OVMX `pe` instance and assert its emitted
 * frames' shape, allowlist membership, and send_seq contiguity.
 *
 * THE INPUT SEQUENCE. Two clean-room specimens from tests/cluster/host/
 * fixtures/ -- the SAME manifest-hashed, capture-cited fixture chain the R1
 * codec/pe tests already use, loaded through the same cluster_fixture.h
 * loader (this driver adds no new decoder and no new fixture format):
 *
 *   1. hello-directed-vax2-to-vax1   (formation-ci1-joinwindow.pcap's
 *      predecessor capture scs-idle-baseline.pcap, frame 2) -- VAX2's
 *      directed b2 INIT to VAX1's cluster-LOGICAL address.
 *   2. scs-start-vax2-config-round0  (formation-ci1-joinwindow.pcap, frame
 *      15) -- VAX2's config-round-0 START on the channel the HELLO just
 *      opened.
 *
 * Both are frames a REAL VAX (VAX2) sent to a REAL VAX (VAX1) in the
 * reference lab; this replay drives ONE simulated OVMX node standing in
 * VAX1's address slot (SCSSYSTEMID 1025, the destination the fixtures
 * themselves name) while keeping OVMX's OWN honest identity everywhere
 * else -- hardware MAC, SCSNODE, software version -- exactly the
 * test_pe_formation.c precedent ("This node stands in the specimen's
 * DESTINATION slot ... while keeping its own, different hardware MAC").
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT. VAX1's OWN captured answers
 * (scs-start-ack.spec, frame 20) are loaded and printed alongside for a
 * human to eyeball, but NEVER compared byte-for-byte: replay.h's contract is
 * structure-tolerant (shape + allowlist + seq contiguity), because OVMX's
 * emitted frames legitimately carry OVMX's own identity and its own
 * send_seq numbering, not VAX1's.
 *
 * A GAP THIS FILE FLAGS RATHER THAN PAPERS OVER. Plan row FC-P1.5 names "the
 * vax3 reference join for the START phase" as the rung target; the raw pcap
 * for that specific capture (vax3-2to3-established-join-20260730.pcap) lives
 * only on the lab host (docs/clean-room/PROVENANCE.md: pcaps are host-only,
 * never committed) and has not yet been decoded into a fixture under
 * tests/cluster/host/fixtures/ -- neither exists in this checkout. This
 * driver is written GENERICALLY (replay.h takes any ordered specimen list)
 * and is exercised here against the START-phase sequence that IS already a
 * decoded, manifest-hashed fixture (formation-ci1-joinwindow.pcap), which is
 * the same wire choreography (a directed HELLO, then a config-round-0
 * START) the vax3 capture would also replay. Decoding the vax3-named
 * capture into its own fixtures and swapping the input list here is lab-lane
 * follow-up, not a rewrite of this driver.
 */

#include <stdio.h>
#include <string.h>

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "replay.h"
#include "sim.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_cluster_codec_mscp.h"

/* The lab's known, previously-grounded MACs (identical constants to
 * test_pe_formation.c's -- an established, audited value, not invented
 * here). VAX2's is the phantom peer's address: registering it on the
 * virtual LAN is address bookkeeping only (sim_lan.c has no FSM behind a
 * port unless sim_node_attach bound one), never a simulated VAX. */
static const uint8_t vax2_hw[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

/* VAX1's SCSSYSTEMID, the slot the fixtures address (scs-start.spec: "pl46
 * SCSSYSTEMID 1026 = VAX2 (GROUNDED)"; VAX1's own logical address low bytes
 * "01 04" the same way -> 0x0401 = 1025). */
#define VAX1_SYSID 1025u

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	}
	return NULL;
}

static void print_report(const char *what, const char *report)
{
	printf("-- %s\n%s", what, report);
}

static void run_start_phase_replay(void)
{
	struct sim s;
	struct sim_node_cfg cfg;
	int node_idx;
	struct sim_node *node;
	const struct vms_fixture *hello, *start, *ref_ack;
	struct vms_replay_input in[2];
	struct vms_replay_result out;
	char report[4096];
	const struct vms_wire_allow_table *tables[3];
	uint32_t bad, skipped;

	printf("-- FC-P1.5: replay the reference START-phase join at one "
	       "simulated OVMX instance\n");

	hello = fixture("hello-directed-vax2-to-vax1");
	start = fixture("scs-start-vax2-config-round0");
	ref_ack = fixture("scs-start-ack-round2");
	if (hello == NULL || start == NULL) {
		ct_check(0, "the two input fixtures are present");
		return;
	}
	if (ref_ack != NULL) {
		printf("   (reference joiner's own answer, VAX1's captured "
		       "scs-start-ack-round2, %u bytes -- printed for "
		       "comparison, never byte-diffed against OVMX's own "
		       "answer)\n", (unsigned)ref_ack->wire_len);
	}

	/* ---- one simulated OVMX node, standing in VAX1's address slot ---- */
	sim_init(&s, 1u);
	sim_node_cfg_default(&cfg, "OVMX", VAX1_SYSID, 0u);
	node_idx = sim_add_node(&s, &cfg);
	ct_check(node_idx == 0, "the OVMX node attaches to LAN port 0");

	/* The phantom peer slot: VAX2's real hardware address and the cluster
	 * HELLO group, registered on the wire so OVMX's own directed/
	 * multicast responses have somewhere to be SCHEDULED (sim_lan.c
	 * addressing) -- not a simulated VAX2 FSM. */
	ct_check(sim_lan_add_port(&s.lan, vax2_hw, group1) == 1,
		 "the reference peer's address is registered on port 1");

	ct_check(sim_boot_all(&s) == 1, "the OVMX node boots");
	node = sim_node_at(&s, 0u);
	ct_check(node != NULL, "the booted node is reachable");
	if (node == NULL)
		return;

	/* ---- drive the two captured VAX2 frames, in capture order ---- */
	in[0].bytes = hello->bytes;
	in[0].len   = hello->wire_len;
	in[0].label = hello->name;
	in[1].bytes = start->bytes;
	in[1].len   = start->wire_len;
	in[1].label = start->name;

	ct_check(vms_replay_drive(&s, node, in, 2u, &out) == 0,
		 "the replay drives both frames without overflow");
	printf("   OVMX emitted %u frame(s) in response\n",
	       (unsigned)out.n);
	ct_check(out.n > 0u, "OVMX answered the captured join (INV-6: a real "
		 "emission, not an assumed one)");

	/* ---- assertion 1: shape ---- */
	report[0] = '\0';
	bad = vms_replay_check_shape(&out, report, sizeof(report));
	print_report("shape", report);
	ct_check_eq_u32(bad, 0u, "every emitted frame is a grounded, "
			"length-coherent class");

	/* ---- assertion 2: allowlist (only classes with a SYSAP envelope
	 * carry the obligation; see replay.h for why this driver checks
	 * "grounded in ANY known table" rather than a per-SYSAP resolution) */
	tables[0] = vms_cm_allow_table();
	tables[1] = &vms_dlm_allow_table;
	tables[2] = &vms_mscp_allow_table;
	report[0] = '\0';
	bad = vms_replay_check_allowlist(&out, tables, 3u, &skipped, report,
					 sizeof(report));
	print_report("allowlist", report);
	printf("   %u frame(s) carried no SYSAP envelope (skipped, no "
	       "obligation)\n", (unsigned)skipped);
	ct_check_eq_u32(bad, 0u, "every SCS_MSG-class emission is grounded "
			"in some allowlist table");

	/* ---- assertion 3: seq contiguity ---- */
	report[0] = '\0';
	ct_check(vms_replay_check_seq_contiguous(&out, report, sizeof(report)),
		 "OVMX's own send_seq is contiguous across everything it "
		 "emitted on this circuit");
	print_report("seq", report);
}

/* Determinism: the whole replay run twice, from the same seed, must produce
 * the identical emitted-frame set -- the same "deterministic by seed"
 * property FC-P1.4's own engine test proves, applied to this driver. */
static void run_determinism_check(void)
{
	struct sim s1, s2;
	struct sim_node_cfg cfg;
	struct vms_replay_input in[2];
	struct vms_replay_result out1, out2;
	const struct vms_fixture *hello, *start;
	uint32_t i;

	printf("-- determinism: the same replay twice yields the same "
	       "emitted frames\n");
	hello = fixture("hello-directed-vax2-to-vax1");
	start = fixture("scs-start-vax2-config-round0");
	if (hello == NULL || start == NULL) {
		ct_check(0, "the two input fixtures are present");
		return;
	}
	in[0].bytes = hello->bytes; in[0].len = hello->wire_len;
	in[1].bytes = start->bytes; in[1].len = start->wire_len;

	sim_init(&s1, 42u);
	sim_node_cfg_default(&cfg, "OVMX", VAX1_SYSID, 0u);
	(void)sim_add_node(&s1, &cfg);
	(void)sim_lan_add_port(&s1.lan, vax2_hw, group1);
	(void)sim_boot_all(&s1);
	(void)vms_replay_drive(&s1, sim_node_at(&s1, 0u), in, 2u, &out1);

	sim_init(&s2, 42u);
	sim_node_cfg_default(&cfg, "OVMX", VAX1_SYSID, 0u);
	(void)sim_add_node(&s2, &cfg);
	(void)sim_lan_add_port(&s2.lan, vax2_hw, group1);
	(void)sim_boot_all(&s2);
	(void)vms_replay_drive(&s2, sim_node_at(&s2, 0u), in, 2u, &out2);

	ct_check_eq_u32(out1.n, out2.n, "same emitted-frame count both runs");
	if (out1.n == out2.n) {
		int all_eq = 1;

		for (i = 0; i < out1.n; i++) {
			if (out1.frame[i].len != out2.frame[i].len ||
			    memcmp(out1.frame[i].bytes, out2.frame[i].bytes,
				  out1.frame[i].len) != 0)
				all_eq = 0;
		}
		ct_check(all_eq, "every emitted frame is byte-identical "
			 "between the two runs");
	}
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN] = "";

	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES, err,
				   sizeof(err));
	if (g_n < 0) {
		printf("FAIL loading fixtures: %s\n", err);
		return 1;
	}

	run_start_phase_replay();
	run_determinism_check();

	return ct_summary("test_replay");
}
