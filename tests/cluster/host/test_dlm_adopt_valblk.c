/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_adopt_valblk.c - rd vms-26e: A TREE ADOPTED FROM A REAL VAX KEEPS
 * ITS VALUE BLOCK, AND THE NEW MASTER'S GRANT CARRIES IT.
 *
 * ==========================================================================
 * THE DEFECT, AS THE WIRE RECORDED IT (RA7 fire A, rd vms-6ad1)
 * ==========================================================================
 * tests/lab/captures/vms-6ad1-ra7-residue-20261021/fireA/trimmed.pcap.gz:
 *
 *   frame  8  VAX1, RA7T's master, GRANTS OVMXE's CR request: the value block
 *             "RMLVB-0123456789" at body[36:52], sequence 1 at body[52:56]
 *             (fixture ra7a-82-01-grant-lvb).
 *   frame 15  remastering RA7T onto OVMXE, VAX1 sends it an op-0x1a about the
 *             lock OVMXE itself holds: the same block, the same sequence
 *             (fixture ra7a-02-1a-valblk).
 *   frame 25  VAX2 asks the NEW master for RA7T at CR with LCK$M_VALBLK
 *             (fixture ra7a-02-01-vax2-enq-valblk).
 *   frame 26  OVMXE GRANTS it with body[36:56] all zero (fixture
 *             ra7a-82-01-ovmx-grant-zero-lvb) -- and VAX2's SDA reads
 *             Valblk 0 / Seqnum 0.
 *
 * ROOT CAUSE: the new master never READ the op-0x1a. A holder that is not the
 * new master relays the old master's block in its op-0x12 record; the new
 * master sends itself no record, so for a lock it holds itself the op-0x1a is
 * the ONLY place the resource's block reaches it. Ignored, the adopted RSB kept
 * a zero block at sequence 0 -- which the master grant path then read back,
 * faithfully, into every grant. And that grant path also wrote no sequence at
 * body[52:56] at all.
 *
 * ==========================================================================
 * WHAT IS REAL HERE
 * ==========================================================================
 * The ENGINE (src/kernel-core/vms_lock.c on the host backend), the CODEC that
 * parses every inbound frame and builds the outbound grant, and the four
 * frames, which are the captured bytes, loaded through the manifest-gated
 * fixture loader. ONE cell is substituted, and it is said where: the 0x1a's
 * body[24:28] names the holder's own handle, and the holder here is THIS
 * executive, which minted its own handle for its proxy rather than OVMXE's
 * 0x01000361. Every other byte is VAX1's or VAX2's.
 *
 * The CONTROL (section 2) runs the same sequence without the op-0x1a and must
 * reproduce the captured defect byte for byte over body[28:56] -- which is what
 * proves the main assertion can fail.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"

#include "vms_internal.h"     /* -> lock_shim/vms_internal.h (the real engine) */
#include "exec_kbackend.h"    /* -> lock_shim/exec_kbackend_linux.h            */
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_adopt.h"
#include "vms_dlm_master.h"
#include "vms_dlm_proxy.h"

#include <stdio.h>
#include <string.h>

/* The executive globals vms_lock.c reads, and the AST hook it calls. */
uint32_t vms_local_csid = 1;

void vms_ast_notify_arrival(struct vms_proc *proc)
{
	(void)proc;
}

#define CSID_LOCAL   1u   /* OVMXE: the requester, then the new master       */
#define CSID_VAX1    2u   /* RA7T's old master, and this node's directory    */
#define CSID_VAX2    3u   /* the other holder, who asks the new master       */

#define TREE "RA7T"
/* RA7T's identity on every fire-A frame: body[44:46] group 0, body[46] mode 3
 * (user) -- the RMHOLD processes run in user mode. */
#define TREE_GROUP 0u
#define TREE_MODE  3u

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++)
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	return NULL;
}

static const uint8_t *fxbody(const struct vms_fixture *f)
{
	return f->bytes + VMS_OFF_SYSAP_BODY;
}

/* ==========================================================================
 * The requester seam: posts are recorded and never answered by themselves
 * (vms_dlm_scs.h RULE A). The directory member is VAX1, as it was in RA7.
 * ========================================================================== */
static uint32_t g_posts;

static uint32_t eng_post(void *ctx, const struct vms_dlm_proxy_post *p)
{
	(void)ctx;
	(void)p;
	g_posts++;
	return SS__NORMAL;
}

static uint32_t eng_dir_resolve(void *ctx, uint32_t hash16, uint32_t *out)
{
	(void)ctx;
	(void)hash16;
	*out = CSID_VAX1;
	return SS__NORMAL;
}

static uint32_t eng_dir_generation(void *ctx)
{
	(void)ctx;
	return 1u;
}

static struct vms_dlm_requester_ops eng_ops;
static struct vms_proc g_proc, g_delivery;

static void proc_init(struct vms_proc *p)
{
	static uint32_t next_vms_pid = 0x10000001u;
	int i;

	memset(p, 0, sizeof(*p));
	exec_lock_init(&p->mode_lock);
	exec_lock_init(&p->lock_list_lock);
	exec_list_head_init(&p->locks);
	p->vms_pid = next_vms_pid++;
	for (i = 0; i < 4; i++) {
		exec_lock_init(&p->ast[i].lock);
		exec_list_head_init(&p->ast[i].pending);
	}
}

static int engine_up(void)
{
	vms_local_csid = CSID_LOCAL;
	g_posts = 0u;
	if (vms_lock_init() != 0) {
		ct_check(0, "vms_lock_init");
		return 0;
	}
	memset(&eng_ops, 0, sizeof(eng_ops));
	eng_ops.post = eng_post;
	eng_ops.dir_resolve = eng_dir_resolve;
	eng_ops.dir_generation = eng_dir_generation;
	vms_lock_dlm_set_requester_ops(&eng_ops);
	proc_init(&g_proc);
	g_proc.current_mode = TREE_MODE;   /* uic 0 -> group 0, as on the wire */
	proc_init(&g_delivery);
	vms_lock_dlm_set_delivery_proc(&g_delivery);
	/* The cluster's hash for RA7T, under the identity the request carries
	 * (the test stands in for the system that put it on the wire). */
	(void)vms_lock_dlm_learn_dir_hash(TREE, 4u, TREE_GROUP, TREE_MODE,
					  0x4a37u);
	return 1;
}

static void engine_down(void)
{
	vms_lock_dlm_set_delivery_proc(NULL);
	vms_lock_dlm_set_requester_ops(NULL);
	vms_lock_cleanup();
}

/* ==========================================================================
 * The sequence, each step through a shipping door
 * ========================================================================== */

/* OVMXE's own CR + VALBLK request on RA7T: a real proxy LKB. */
static uint32_t ovmx_requests_ra7t(void)
{
	struct vms_enq_args e;

	memset(&e, 0, sizeof(e));
	e.lkmode = LCK_K_CRMODE;
	e.flags = LCK_M_VALBLK;
	strscpy(e.resnam, TREE, sizeof(e.resnam));
	vms_ioctl_enq(&g_proc, (unsigned long)(void *)&e);
	return e.lkid;
}

/* Frame 8, VAX1's real grant, parsed by the shipping codec and delivered
 * through the engine's real grant receive. Its body[24:28] names OVMXE's
 * handle; the correlation to THIS executive's proxy is the requester FSM's
 * job, so the parsed grant is delivered to the lock this executive minted. */
static uint32_t vax1_grants(uint32_t own_lkid, uint32_t *vax1_lkid)
{
	const struct vms_fixture *f = fixture("ra7a-82-01-grant-lvb");
	struct vms_dlm_enq_response r;
	struct vms_dlm_proxy_grant g;

	if (f == NULL ||
	    vms_dlm_enq_response_parse_body(fxbody(f), VMS_CM_BODY_LEN, &r) !=
	    VMS_CODEC_OK || r.outcome != VMS_DLM_ENQ_GRANTED)
		return 0u;
	memset(&g, 0, sizeof(g));
	g.req_lkid = own_lkid;
	g.master_lkid = r.master_lkid;
	g.master_csid = CSID_VAX1;
	g.granted_mode = r.granted_mode;
	g.granted_mode_present = r.granted_mode_present;
	g.valblk_present = r.valblk_present;
	memcpy(g.valblk, r.valblk, sizeof(g.valblk));
	*vax1_lkid = r.master_lkid;
	return vms_lock_dlm_proxy_grant_recv(&g);
}

static struct vms_dlm_adopt_tree ra7t_tree(void)
{
	struct vms_dlm_adopt_tree t;

	memset(&t, 0, sizeof(t));
	strscpy(t.resnam, TREE, sizeof(t.resnam));
	t.resnam_len = 4u;
	t.res_group = TREE_GROUP;
	t.res_mode = TREE_MODE;
	t.old_master_csid = CSID_VAX1;
	t.ident_valid = 1u;
	return t;
}

/* Frame 15, VAX1's real op-0x1a, parsed by the shipping codec. The one
 * substituted cell is body[24:28], the holder's own handle (see the top). */
static int vax1_op1a(uint32_t own_lkid, uint32_t from_csid,
		     struct vms_dlm_adopt_valblk *out)
{
	const struct vms_fixture *f = fixture("ra7a-02-1a-valblk");
	struct vms_dlm_rm_valblk w;
	uint8_t body[VMS_CM_BODY_LEN];

	if (f == NULL)
		return 0;
	memcpy(body, fxbody(f), sizeof(body));
	body[24] = (uint8_t)own_lkid;
	body[25] = (uint8_t)(own_lkid >> 8);
	body[26] = (uint8_t)(own_lkid >> 16);
	body[27] = (uint8_t)(own_lkid >> 24);
	if (vms_dlm_rm_valblk_parse_body(body, sizeof(body), &w) !=
	    VMS_CODEC_OK)
		return 0;
	memset(out, 0, sizeof(*out));
	out->from_csid = from_csid;
	out->master_lkid = w.master_lkid;
	out->holder_lkid = w.holder_lkid;
	out->valblk_present = w.valblk_present;
	out->valblk_seq = w.valblk_seq;
	memcpy(out->valblk, w.valblk, sizeof(out->valblk));
	return 1;
}

/* Frame 25, VAX2's real request, served by the shipping master seam the way
 * the wire arm fills it (vms_dlm_scs.c dlm_arm_fill_master_req), and answered
 * by the shipping grant builder the way dlm_arm_reply_grant calls it. */
static int vax2_asks(uint8_t *frame, struct vms_dlm_master_result *res)
{
	const struct vms_fixture *f = fixture("ra7a-02-01-vax2-enq-valblk");
	struct vms_dlm_enq_request e;
	struct vms_dlm_master_request mr;
	uint8_t op = 0u;
	uint32_t written = 0u;

	if (f == NULL ||
	    vms_dlm_enq_request_parse_body(fxbody(f), VMS_CM_BODY_LEN, &op,
					   &e) != VMS_CODEC_OK)
		return 0;
	memset(&mr, 0, sizeof(mr));
	mr.op = VMS_DLM_MREQ_ENQ;
	mr.req_csid = CSID_VAX2;
	mr.req_lkid = e.req_lkid;
	mr.master_lkid = e.master_lkid;
	mr.lkmode = e.mode;
	memcpy(mr.resnam, e.name, e.name_len);
	mr.resnam_len = e.name_len;
	mr.res_group = e.res_group;
	mr.res_mode = e.res_acmode;
	mr.res_ident_valid = e.res_ident_valid;
	memset(res, 0, sizeof(*res));
	if (vms_lock_dlm_master_serve(&mr, res) != SS__NORMAL ||
	    res->outcome != VMS_DLM_MASTER_GRANTED)
		return 0;
	memset(frame, 0, VMS_CM_FRAME_LEN);
	return vms_dlm_enq_response_build_grant(fxbody(f), VMS_CM_BODY_LEN,
						res->master_lkid,
						res->valblk_present ?
						res->valblk : NULL,
						res->valblk_seq, frame,
						VMS_CM_FRAME_LEN, &written) ==
	       VMS_CODEC_OK;
}

/* ==========================================================================
 * 1. THE FIX: the op-0x1a's block is the adopted RSB's, and the grant says so
 * ========================================================================== */
static void test_adopted_tree_grants_its_value_block(void)
{
	const struct vms_fixture *op1a = fixture("ra7a-02-1a-valblk");
	const struct vms_fixture *defect =
		fixture("ra7a-82-01-ovmx-grant-zero-lvb");
	struct vms_dlm_adopt_tree t = ra7t_tree();
	struct vms_dlm_adopt_valblk v;
	struct vms_dlm_master_result res;
	struct vms_dlm_enq_response parsed;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t own, vax1_lkid = 0u, n_locks = 0u, applied0;
	const uint8_t *out;

	printf("-- rd vms-26e: RA7 fire A replayed -- the adopted tree's value "
	       "block reaches the next grant\n");
	ct_check(op1a != NULL && defect != NULL, "the fire-A specimens load");
	if (op1a == NULL || defect == NULL || !engine_up())
		return;

	own = ovmx_requests_ra7t();
	ct_check(own != 0u && g_posts >= 1u,
		 "OVMXE's CR request on RA7T is a real proxy that left as a post");
	ct_check_eq_u32(vax1_grants(own, &vax1_lkid), SS__NORMAL,
			"VAX1's real grant (frame 8) is received");
	ct_check_eq_u32(vax1_lkid, 0x0700065fu,
			"  naming 0x0700065f, the handle VAX1 minted");

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL,
			"VAX1 hands RA7T to this node (adopt_open)");
	ct_check(vax1_op1a(own, CSID_VAX1, &v),
		 "VAX1's real op-0x1a (frame 15) parses");
	ct_check_eq_u32(v.master_lkid, vax1_lkid,
			"  it names VAX1's own handle for our lock");
	ct_check(v.valblk_present && v.valblk_seq == 1u &&
		 memcmp(v.valblk, "RMLVB-0123456789", 16) == 0,
		 "  and carries RMLVB-0123456789 at sequence 1");
	applied0 = vms_lock_dlm_adopt_valblks_applied();
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&v), SS__NORMAL,
			"*** the engine TAKES the op-0x1a: our proxy, VAX1's "
			"handle, VAX1's open hand-over ***");
	ct_check_eq_u32(vms_lock_dlm_adopt_valblks_applied(), applied0 + 1u,
			"  and counts one block written onto a resource");
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t, &n_locks), SS__NORMAL,
			"the adoption completes (VAX1's op-0x13)");

	ct_check(vax2_asks(frame, &res),
		 "VAX2's real VALBLK request (frame 25) is GRANTED by this "
		 "node, through the master seam and the grant builder");
	ct_check(res.valblk_present &&
		 memcmp(res.valblk, "RMLVB-0123456789", 16) == 0 &&
		 res.valblk_seq == 1u,
		 "*** the master seam READ RMLVB-0123456789 / 1 off the RSB ***");

	out = frame + VMS_OFF_SYSAP_BODY;
	ct_check(memcmp(out + 36, fxbody(op1a) + 36, 20) == 0,
		 "*** the grant's body[36:56] is exactly the 20 bytes VAX1's "
		 "op-0x1a carried: the block AND its sequence ***");
	ct_check(memcmp(out + 36, fxbody(defect) + 36, 20) != 0,
		 "*** and NOT the captured defect's twenty zeros ***");
	ct_check(vms_dlm_enq_response_parse_body(out, VMS_CM_BODY_LEN,
						 &parsed) == VMS_CODEC_OK &&
		 parsed.outcome == VMS_DLM_ENQ_GRANTED &&
		 parsed.valblk_seq == 1u &&
		 memcmp(parsed.valblk, "RMLVB-0123456789", 16) == 0,
		 "  the shipping parser reads the grant back as GRANTED, "
		 "RMLVB-0123456789, sequence 1");
	/* Outside the value block the fix changes nothing: the grant is still
	 * the one the defect specimen shows, record and handles. */
	ct_check(memcmp(out + 24, fxbody(defect) + 24, 12) == 0,
		 "  body[24:36] (VAX2's handle, the grant record) is byte-equal "
		 "to the captured OVMXE grant");

	engine_down();
}

/* ==========================================================================
 * 2. THE CONTROL: without the op-0x1a, the captured defect, byte for byte
 * ========================================================================== */
static void test_without_op1a_the_defect_reproduces(void)
{
	const struct vms_fixture *defect =
		fixture("ra7a-82-01-ovmx-grant-zero-lvb");
	struct vms_dlm_adopt_tree t = ra7t_tree();
	struct vms_dlm_master_result res;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t own, vax1_lkid = 0u, n_locks = 0u;

	printf("-- control: the same replay WITHOUT the op-0x1a reproduces "
	       "frame 26\n");
	if (defect == NULL || !engine_up())
		return;
	own = ovmx_requests_ra7t();
	(void)vax1_grants(own, &vax1_lkid);
	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "adopt_open");
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t, &n_locks), SS__NORMAL,
			"adopt_end, no op-0x1a delivered");
	ct_check(vax2_asks(frame, &res), "VAX2's request is granted");
	ct_check(memcmp(frame + VMS_OFF_SYSAP_BODY + 28,
			fxbody(defect) + 28, 28) == 0,
		 "*** body[28:56] equals the captured OVMXE grant: the "
		 "defect is exactly 'the op-0x1a was never read' ***");
	engine_down();
}

/* ==========================================================================
 * 3. WHO MAY WRITE A BLOCK: the refusals write nothing
 * ========================================================================== */
static uint8_t ra7t_block_seq(uint32_t *seq)
{
	uint8_t frame[VMS_CM_FRAME_LEN];
	struct vms_dlm_master_result res;

	*seq = 0u;
	if (!vax2_asks(frame, &res))
		return 0xffu;
	*seq = res.valblk_seq;
	return res.valblk_present;
}

static void test_refusals_write_nothing(void)
{
	struct vms_dlm_adopt_tree t = ra7t_tree();
	struct vms_dlm_adopt_valblk v, bad;
	uint32_t own, vax1_lkid = 0u, n_locks = 0u, applied0, seq = 0u;

	printf("-- the op-0x1a is checked against the lock and the hand-over\n");
	if (!engine_up())
		return;
	own = ovmx_requests_ra7t();
	(void)vax1_grants(own, &vax1_lkid);
	ct_check(vax1_op1a(own, CSID_VAX1, &v), "the op-0x1a parses");

	applied0 = vms_lock_dlm_adopt_valblks_applied();
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&v), SS__IVLOCKID,
			"*** no hand-over open: refused (a block for a tree "
			"nobody handed this node) ***");

	ct_check_eq_u32(vms_lock_dlm_adopt_open(&t), SS__NORMAL, "adopt_open");
	bad = v;
	bad.from_csid = CSID_VAX2;
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&bad), SS__IVLOCKID,
			"*** from a system that is not the old master: "
			"refused ***");
	bad = v;
	bad.master_lkid = vax1_lkid + 1u;
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&bad), SS__IVLOCKID,
			"*** naming a handle VAX1 did not grant us: refused ***");
	bad = v;
	bad.holder_lkid = own + 0x100u;
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&bad), SS__IVLOCKID,
			"*** naming a lock that is no proxy of ours: refused ***");
	bad = v;
	bad.from_csid = 0u;
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&bad), SS__BADPARAM,
			"  from nobody: a malformed call");
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(NULL), SS__BADPARAM,
			"  NULL: a malformed call");
	ct_check_eq_u32(vms_lock_dlm_adopt_valblks_applied(), applied0,
			"*** and not one of them wrote a block ***");

	/* An ABSENT block (twenty zeros) is accepted and leaves the RSB alone:
	 * here the RSB holds none, so after the adoption a grant carries none. */
	bad = v;
	bad.valblk_present = 0u;
	memset(bad.valblk, 0, sizeof(bad.valblk));
	bad.valblk_seq = 0u;
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&bad), SS__NORMAL,
			"an op-0x1a with no block is the right lock's: accepted");
	ct_check_eq_u32(vms_lock_dlm_adopt_valblks_applied(), applied0,
			"  and writes nothing");
	ct_check_eq_u32(vms_lock_dlm_adopt_end(&t, &n_locks), SS__NORMAL,
			"adopt_end");
	ct_check_eq_u32(ra7t_block_seq(&seq), 0u,
			"*** the adopted RSB holds no block it was not told ***");
	ct_check_eq_u32(seq, 0u, "  and no sequence");

	/* ...and once the tree is HELD, a late op-0x1a has no hand-over to
	 * land in (our lock is master-side now, no proxy). */
	ct_check_eq_u32(vms_lock_dlm_adopt_valblk(&v), SS__IVLOCKID,
			"*** a late op-0x1a, after the adoption closed: "
			"refused ***");
	ct_check_eq_u32(ra7t_block_seq(&seq), 0u,
			"  and the RSB is still as it was");
	engine_down();
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_dlm_adopt_valblk: rd vms-26e, RA7 fire A on the real "
	       "engine\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}
	test_adopted_tree_grants_its_value_block();
	test_without_op1a_the_defect_reproduces();
	test_refusals_write_nothing();
	return ct_summary("test_dlm_adopt_valblk");
}
