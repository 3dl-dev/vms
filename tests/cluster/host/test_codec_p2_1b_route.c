// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_p2_1b_route.c - the FC-P2.1b classify-table widen, rung R1.
 *
 * E18 (raised by FC-P2.2): vc_deliver() (vms_pe_fsm.c) only hands a frame up
 * to SCS when the codec grants it VMS_FCAP_CONID. FC-P0.6's frozen classify
 * table granted that capability to VMS_FCLS_SCS_CONN_CTRL for content
 * {110,66,62} only, and VMS_FCLS_SCS_MSG for content 190 only -- so the
 * 58-content shorts (ops 5 REJECT_RSP, 7 DISCONNECT_RSP, 8 CREDIT_REQ, 9
 * CREDIT_RSP) and the 94-content directory lookup (op 10) classified with NO
 * Con.ID capability and were acked-but-unrouted. Spec §4(h)(1b) (vms-591/
 * vms-54f) grounds the SAME [50:58] handle pair across every SCS length
 * class, including these -- this file is the test half of that widen.
 *
 * Three groups:
 *   1. Every 58-content short (5/7/8/9) now classifies VMS_FCLS_SCS_CONN_CTRL
 *      with VMS_FCAP_CONID, and vms_scs_conid() -- the CLASS-GATED accessor
 *      vc_deliver() actually calls -- parses the Con.ID pair from [50:58].
 *   2. The 94-content op-10 SCS$DIRECTORY lookup now classifies
 *      VMS_FCLS_SCS_APPLMSG94 with VMS_FCAP_CONID, same accessor proof.
 *   3. THE NO-REGRESSION SAFETY NET (E5): the 190-content class is still
 *      VMS_FCLS_SCS_MSG untouched, a 94-content MSCP command is untouched
 *      FUNCTIONALLY (vms_mscp_classify() still resolves it, mscp_seq_ok()
 *      widened to accept the new class) -- and, the actual danger this item
 *      had to rule out, a 94-content frame does NOT satisfy
 *      VMS_FCLS_SCS_MSG, so DLM's (vms_cluster_codec_dlm.c dlm_class_ok())
 *      and CM's class gate -- which key on VMS_FCLS_SCS_MSG alone with no
 *      length check of their own -- never see a 94-content MSCP/directory
 *      frame routed into their parsers.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_mscp.h"
#include "vms_cluster_codec_scs.h"

#include <string.h>

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

/* ---- group 1/2: fixture-backed CONID routing proof --------------------- */

/*
 * `want_remote`/`want_local` are read straight off the fixture's own cited
 * [50:58] bytes (see the .spec files' `@64`/`@68` lines) -- never invented
 * here, so this is checking the codec against the SAME wire data the
 * specimen already carries.
 */
static void assert_conid_routes(const char *fname, uint8_t want_cls,
				 const char *cls_label, uint32_t want_remote,
				 uint32_t want_local)
{
	const struct vms_fixture *f = fixture(fname);
	struct vms_frame_info fi;
	uint32_t remote = 0xffffffffu, local = 0xffffffffu;
	char what[192];

	ct_check(f != NULL, fname);
	if (f == NULL) {
		printf("       (fixture missing -- corpus regression)\n");
		return;
	}

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");

	snprintf(what, sizeof(what), "%s: classifies as %s", fname, cls_label);
	ct_check(fi.cls == want_cls, what);

	snprintf(what, sizeof(what),
		 "%s: the class grants VMS_FCAP_CONID (vc_deliver()'s gate)",
		 fname);
	ct_check((fi.caps & VMS_FCAP_CONID) != 0, what);

	/* vms_scs_conid() is the CLASS-GATED accessor vc_deliver() actually
	 * calls (via h_vc_rx_seqmsg -> the conid_valid path). A frame this
	 * item fails to route classifies fine but this call returns
	 * VMS_CODEC_E_CLASS -- that was the E18 bug, reproduced here as the
	 * regression it now cannot silently become again. */
	snprintf(what, sizeof(what),
		 "%s: vms_scs_conid() (vc_deliver()'s own accessor) succeeds",
		 fname);
	ct_check(vms_scs_conid(f->bytes, f->wire_len, &fi, &remote, &local)
		 == VMS_CODEC_OK, what);

	snprintf(what, sizeof(what), "%s: remote Con.ID parses from [50:54]",
		 fname);
	ct_check_eq_u32(remote, want_remote, what);
	snprintf(what, sizeof(what), "%s: local Con.ID parses from [54:58]",
		 fname);
	ct_check_eq_u32(local, want_local, what);
}

static void test_58content_shorts_route(void)
{
	printf("-- group 1: the 58-content shorts (ops 5/7/8/9) now route\n");
	/* scs-reject-response.spec @64 = 08 00 dc e2, @68 = 07 00 00 00 */
	assert_conid_routes("scs-reject-response", VMS_FCLS_SCS_CONN_CTRL,
			    "VMS_FCLS_SCS_CONN_CTRL", 0xe2dc0008u, 0x00000007u);
	/* scs-disc-response.spec @64 = 08 00 05 63, @68 = 07 00 59 33 */
	assert_conid_routes("scs-disc-response", VMS_FCLS_SCS_CONN_CTRL,
			    "VMS_FCLS_SCS_CONN_CTRL", 0x63050008u, 0x33590007u);
}

static void test_op10_directory_lookup_routes(void)
{
	printf("-- group 2: the 94-content op-10 directory lookup now routes\n");
	/* scs-dir-lookup-request.spec @64 = 07 00 59 33, @68 = 08 00 05 63 */
	assert_conid_routes("scs-dir-lookup-request", VMS_FCLS_SCS_APPLMSG94,
			    "VMS_FCLS_SCS_APPLMSG94", 0x33590007u, 0x63050008u);
	/* scs-dir-lookup-response-negative.spec @64 = 08 00 05 63, @68 = 07 00 59 33 */
	assert_conid_routes("scs-dir-lookup-response-negative",
			    VMS_FCLS_SCS_APPLMSG94, "VMS_FCLS_SCS_APPLMSG94",
			    0x63050008u, 0x33590007u);
}

/*
 * Ops 8/9 have no byte-exact fixture in the corpus (sec 4h(1c)/(1f) are
 * census findings, not one captured frame -- same discipline
 * test_codec_scs.c's credit_op_structural() already documents). Hand-built
 * on the SAME terms: vms_scs_ctrl_build() from a typed struct, never a raw
 * byte template.
 */
static void credit_op_routes(uint16_t op, const char *label)
{
	struct vms_scs_ctrl_frame cf;
	struct vms_frame_info fi;
	uint8_t built[256];
	uint32_t written = 0;
	uint32_t remote = 0xffffffffu, local = 0xffffffffu;
	char what[160];

	memset(&cf, 0, sizeof(cf));
	memcpy(cf.hdr.eth_dst, "\x08\x00\x2b\x4a\xb7\x15", 6);
	memcpy(cf.hdr.eth_src, "\x08\x00\x2b\x78\x56\xb9", 6);
	memcpy(cf.hdr.dst_lavc, "\xaa\x00\x04\x00\x01\x04", 6);
	memcpy(cf.hdr.src_lavc, "\xaa\x00\x04\x00\x02\x04", 6);
	cf.hdr.connect_flag = 0x0001;
	cf.hdr.word30 = 0x134bu; /* abs30 msgtype 0x4b, abs31 format 0x13 */
	cf.hdr.sca_len_field = VMS_SCSCTRL_LEN_SHORT - 2u;
	cf.recv_ack = 20;
	cf.send_seq = 20;
	cf.incarn = 1;
	cf.lan_ovrhd = 0x0012;
	cf.tail_const1 = 0x0001;
	cf.tail_const2 = 0x0200;
	cf.inner_len = VMS_SCSCTRL_LEN_SHORT - 44u;
	cf.op = op;
	cf.credit = 1; /* GROUNDED constant, sec 4h(1c)/(1f) */
	cf.conid_remote = 0x08000563u;
	cf.conid_local = 0x07005933u;

	memset(built, 0xAA, sizeof(built));
	snprintf(what, sizeof(what), "%s: builds", label);
	ct_check(vms_scs_ctrl_build(&cf, built, sizeof(built), &written)
		 == VMS_CODEC_OK, what);

	ct_check(vms_frame_classify(built, written, &fi) == VMS_CODEC_OK,
		 "re-classifies");
	snprintf(what, sizeof(what), "%s: classifies VMS_FCLS_SCS_CONN_CTRL",
		 label);
	ct_check(fi.cls == VMS_FCLS_SCS_CONN_CTRL, what);
	snprintf(what, sizeof(what), "%s: grants VMS_FCAP_CONID", label);
	ct_check((fi.caps & VMS_FCAP_CONID) != 0, what);

	snprintf(what, sizeof(what), "%s: vms_scs_conid() succeeds", label);
	ct_check(vms_scs_conid(built, written, &fi, &remote, &local)
		 == VMS_CODEC_OK, what);
	ct_check_eq_u32(remote, cf.conid_remote, "  remote Con.ID round-trips");
	ct_check_eq_u32(local, cf.conid_local, "  local Con.ID round-trips");
}

static void test_credit_ops_route(void)
{
	printf("-- group 1 (hand-built): ops 8/9 CREDIT_REQ/CREDIT_RSP route\n");
	credit_op_routes(VMS_SCS_CTRL_CREDIT_REQ, "op8 CREDIT_REQ");
	credit_op_routes(VMS_SCS_CTRL_CREDIT_RSP, "op9 CREDIT_RSP");
}

/* ---- group 3: the no-regression safety net (E5) ------------------------ */

static void test_190content_vc_class_unchanged(void)
{
	const struct vms_fixture *f = fixture("scs-msg190-vaxcluster-cat01-op14");
	struct vms_frame_info fi;

	printf("-- group 3: the 190-content VC class is UNCHANGED\n");
	ct_check(f != NULL, "scs-msg190-vaxcluster-cat01-op14 fixture present");
	if (f == NULL)
		return;
	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(fi.cls == VMS_FCLS_SCS_MSG,
		 "  still VMS_FCLS_SCS_MSG (DLM's/CM's own class gate, "
		 "untouched by this item)");
	ct_check((fi.caps & VMS_FCAP_CONID) != 0,
		 "  still grants VMS_FCAP_CONID, as before");
}

static void mk_link(struct vms_mscp_link *l)
{
	static const uint8_t dst[6] = { 0x08, 0x00, 0x2b, 0x11, 0x22, 0x33 };
	static const uint8_t src[6] = { 0x08, 0x00, 0x2b, 0x44, 0x55, 0x66 };

	memset(l, 0, sizeof(*l));
	memcpy(l->hdr.eth_dst, dst, 6);
	memcpy(l->hdr.eth_src, src, 6);
	memcpy(l->hdr.dst_lavc, dst, 6);
	memcpy(l->hdr.src_lavc, src, 6);
	l->hdr.connect_flag = 0x0001;
	l->recv_ack = 0x1111;
	l->send_seq = 0x2222;
	l->credit = VMS_MSCP_ENV_CREDIT_OBSERVED;
	l->remote_conid = 0xaaaa5501u;
	l->local_conid = 0xbbbb5502u;
}

/*
 * The actual danger this item had to rule out (see file header): a
 * 94-content MSCP command must classify VMS_FCLS_SCS_APPLMSG94 -- NEVER
 * VMS_FCLS_SCS_MSG, which vms_cluster_codec_dlm.c's dlm_class_ok() and
 * vms_cluster_codec_cm.c gate on with no length check of their own. If
 * this ever regressed to VMS_FCLS_SCS_MSG, a 94-content MSCP frame would
 * be handed to the DLM/CM parsers -- exactly the cross-class misread
 * INV-6 exists to refuse.
 */
static void test_94content_mscp_unchanged_and_isolated_from_dlm(void)
{
	struct vms_mscp_link l;
	struct vms_mscp_scc_cmd c;
	struct vms_frame_info fi;
	uint8_t frame[256];
	uint32_t written = 0, len;
	enum vms_mscp_class cls = VMS_MSCP_CLS_UNKNOWN;

	printf("-- group 3: a 94-content MSCP command stays functionally "
	       "correct and ISOLATED from VMS_FCLS_SCS_MSG (DLM/CM's gate)\n");

	mk_link(&l);
	ct_check(vms_mscp_link_build(&l, VMS_MSCP_CMD_SCA_LEN, frame,
				     sizeof(frame), &written) == VMS_CODEC_OK,
		 "link prefix builds");
	memset(&c, 0, sizeof(c));
	c.hdr.cmd_ref = 0x1u;
	c.hdr.unit = 1u;
	ct_check(vms_mscp_scc_cmd_build(&c, frame, sizeof(frame), &written)
		 == VMS_CODEC_OK, "SCC command body builds");
	len = VMS_OFF_SYSAP_BODY + written;

	ct_check(vms_frame_classify(frame, len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(fi.cls == VMS_FCLS_SCS_APPLMSG94,
		 "  classifies VMS_FCLS_SCS_APPLMSG94 (the new grounded class)");
	ct_check(fi.cls != VMS_FCLS_SCS_MSG,
		 "  and NEVER VMS_FCLS_SCS_MSG -- DLM's/CM's class gate stays "
		 "closed to this frame");
	ct_check((fi.caps & VMS_FCAP_CONID) != 0,
		 "  now ALSO grants VMS_FCAP_CONID (a gain, not a loss)");

	ct_check(vms_mscp_classify(frame, len, &fi, &cls) == VMS_CODEC_OK &&
			 cls == VMS_MSCP_CLS_CMD,
		 "  this item's own classifier still resolves VMS_MSCP_CLS_CMD "
		 "(mscp_seq_ok() widened to accept the new class)");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_p2_1b_route: E18 classify-table widen (FC-P2.1b)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_58content_shorts_route();
	test_credit_ops_route();
	test_op10_directory_lookup_routes();
	test_190content_vc_class_unchanged();
	test_94content_mscp_unchanged_and_isolated_from_dlm();

	return ct_summary("test_codec_p2_1b_route");
}
