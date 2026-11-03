/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_transition_enq.c - rd vms-966e: the real VAX's op-0x01 that arrives
 * at barrier step 0x0a, while a third member's join is still running, is
 * ANSWERED.
 *
 * THE STALL (cluster demo gate, 2026-11-03; frames committed under
 * tests/lab/captures/vms-966e-transition-enq-20261103/). Two runs, two join
 * orders, one shape:
 *
 *   r3 (A,B,C), OVMXB the directory:  4159 VAXC op-0x01 CR on
 *       F11B$aVAXCSYS<..>F, lock 0x0b000190 -> 4168 OVMXB 0xf9 "you master
 *       it"; VAXC keeps the lock (no op-0x04 follows). OVMXA joins. 4820
 *       VAXC releases barrier step 0x0a; 4821, 1 ms later, VAXC sends the SAME
 *       lock's op-0x01 to OVMXB. Never answered; VAXC never resends and hangs
 *       in STARTUP before Username:.
 *   r5 (C,A,B), OVMXA the directory: 3180 -> 3186 0xf9, OVMXB joins, 3858
 *       step 0x0a, 3859 the same lock's op-0x01 to OVMXA. Never answered.
 *
 * WHY IT WAS DROPPED. The join's Phase 2 rebuilt this node's vector at the GO
 * over THREE members, and over three the name's directory is VAXC ITSELF (the
 * real VAX agrees: its own op-0x0d rebuild records in the same transition --
 * 4701 to OVMXA, 4702 to OVMXB -- go exactly where this vector sends them, and
 * nothing goes out for the third of the names it directs itself). So the
 * lookup read as UNDIRECTED, and an undirected lookup whose table entry names
 * the requester is refused with no answer (vms_dlm_dir_lookup_directed, rd
 * vms-b4d). The refusal exists so a node that is NOT the directory never makes
 * a second master. Here the requester IS the directory and this table's only
 * master for the name is the requester: answering 0xf9 from the entry repeats
 * what this node already told it and records nothing.
 *
 * Everything below runs the SHIPPING objects: the CLUB, cnxman_ldwv_rebuild
 * (the Phase 2 vector), the directory table, and the codec -- on the real
 * frames. The arm's one line that picks VMS_DLM_DIR_AT_REQUESTER is pinned by
 * test_dlm_scs_arm.c (the arm is not host-linkable).
 */
#include "cluster_test.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_dlm_ldwv.h"
#include "vms_dlm_dir.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"

#include <stdio.h>
#include <string.h>

/* CSV slots in join order: the founder first. Which slot each system really
 * holds is not on these frames; the ORDER is, and it is checked below against
 * where the real VAX's own rebuild records went. */
#define CSID_VAXC   0x00010001u
#define CSID_FIRST  0x00010002u   /* r3 OVMXB, r5 OVMXA: the first joiner  */
#define CSID_SECOND 0x00010003u   /* r3 OVMXA, r5 OVMXB: the joiner whose
				   * transition is running at step 0x0a     */

#define CAPDIR "../../../lab/captures/vms-966e-transition-enq-20261103/"

static struct vms_cluster g_cl;
static struct cnxman_ops g_ops;

static uint32_t cnx_now(void *ctx) { (void)ctx; return 0u; }
static void cnx_log(void *ctx, const char *msg) { (void)ctx; (void)msg; }

/* One frame of a committed transcript (the ci6_frame() shape, rd vms-1e58):
 * the 14-byte Ethernet header left zero, every other byte read from the file. */
static int cap_frame(const char *file, unsigned want, uint8_t *frame)
{
	char path[700], line[256];
	FILE *fp;
	int in = 0, got = 0;

	memset(frame, 0, VMS_CM_FRAME_LEN);
	snprintf(path, sizeof(path), "%s/%s%s", OVMX_FIXTURE_DIR, CAPDIR, file);
	fp = fopen(path, "r");
	if (fp == NULL) {
		ct_check(0, "the committed vms-966e frame transcript opens");
		return -1;
	}
	while (fgets(line, sizeof(line), fp) != NULL) {
		unsigned n = 0, off = 0, v = 0;
		int used = 0;
		const char *c;

		if (sscanf(line, "frame %u", &n) == 1) {
			if (in)
				break;
			in = (n == want);
			continue;
		}
		if (!in || sscanf(line, " %x%n", &off, &used) != 1)
			continue;
		for (c = line + used; sscanf(c, " %2x%n", &v, &used) == 1;
		     c += used, off++) {
			if (14u + off < VMS_CM_FRAME_LEN)
				frame[14u + off] = (uint8_t)v;
			got++;
		}
	}
	fclose(fp);
	ct_check(got + 14 == (int)VMS_CM_FRAME_LEN,
		 "the transcript frame is the 204-byte DLM class, read whole");
	return got + 14 == (int)VMS_CM_FRAME_LEN ? 0 : -1;
}

static const uint8_t *body_of(const uint8_t *frame)
{
	return frame + VMS_OFF_SYSAP_BODY;
}

static void csb_member(vms_csid_t csid, int local)
{
	struct vms_csb *csb;

	if (local)
		csb = cnxman_club_local(&g_cl.club);
	else
		csb = cnxman_club_alloc_csb(&g_cl.club, (vms_scs_sysid_t)csid, 1);
	cnxman_csb_set_csid(csb, csid);
	cnxman_csb_set_lockdirwt(csb, 0u);   /* the V7.3 default, every member */
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
}

/* This node (the first joiner) and VAXC: the vector before the join. */
static void club_two(void)
{
	memset(&g_cl, 0, sizeof(g_cl));
	memset(&g_ops, 0, sizeof(g_ops));
	g_ops.now_ms = cnx_now;
	g_ops.log = cnx_log;
	g_ops.ctx = &g_cl;
	memcpy(g_cl.params.scsnode, "OVMXB", 5);
	g_cl.params.scsnode_len = 5;
	g_cl.params.scssystemid = (uint64_t)CSID_FIRST;
	g_cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&g_cl);
	csb_member(CSID_VAXC, 0);
	csb_member(CSID_FIRST, 1);
	g_cl.club.local_csid = CSID_FIRST;
	g_cl.club.local_csid_valid = 1u;
	ct_check(cnxman_ldwv_rebuild(&g_cl.club, &g_ops) == VMS_LDWV_OK,
		 "the two-member vector builds");
}

/* ...and the join's Phase 2, at the GO: the vector over all three. */
static void club_join_second(void)
{
	csb_member(CSID_SECOND, 0);
	ct_check(cnxman_ldwv_rebuild(&g_cl.club, &g_ops) == VMS_LDWV_OK,
		 "the three-member vector builds at the join's Phase 2");
}

static uint16_t key_of(const uint8_t *frame)
{
	struct vms_dlm_res_ident id;

	if (vms_dlm_res_ident_parse_body(body_of(frame), VMS_CM_BODY_LEN,
					 &id) != VMS_CODEC_OK)
		return 0u;
	return vms_ldwv_key(id.hash);
}

/* The directed value the arm computes (vms_dlm_scs.c dlm_arm_dir_lookup). */
static int directed_for(const struct vms_dlm_res_ident *id, vms_csid_t from)
{
	const struct vms_ldwv *v = &g_cl.club.ldwv;

	if (vms_ldwv_is_ours(v, vms_ldwv_key(id->hash)))
		return 1;
	if (vms_ldwv_names(v, vms_ldwv_key(id->hash), from))
		return VMS_DLM_DIR_AT_REQUESTER;
	return 0;
}

static void one_run(const char *file, const char *run, unsigned lookup,
		    unsigned answer, unsigned step0a, unsigned again,
		    unsigned rec_first, unsigned rec_second,
		    const char *answered)
{
	uint8_t f_look[VMS_CM_FRAME_LEN], f_ans[VMS_CM_FRAME_LEN];
	uint8_t f_step[VMS_CM_FRAME_LEN], f_again[VMS_CM_FRAME_LEN];
	uint8_t f_r1[VMS_CM_FRAME_LEN], f_r2[VMS_CM_FRAME_LEN];
	uint8_t tx[VMS_CM_FRAME_LEN];
	struct vms_dlm_dir dir;
	struct vms_dlm_dir_entry store[16];
	struct vms_dlm_res_ident id, id2;
	enum vms_dlm_dir_outcome o;
	vms_csid_t m = 0u;
	uint32_t written = 0u, entries;

	printf("-- %s: the op-0x01 at barrier step 0x0a is answered --\n", run);
	if (cap_frame(file, lookup, f_look) != 0 ||
	    cap_frame(file, answer, f_ans) != 0 ||
	    cap_frame(file, step0a, f_step) != 0 ||
	    cap_frame(file, again, f_again) != 0)
		return;
	if (rec_first != 0u &&
	    (cap_frame(file, rec_first, f_r1) != 0 ||
	     cap_frame(file, rec_second, f_r2) != 0))
		return;

	/* The frames are what this test says they are. */
	ct_check(f_look[VMS_OFF_SYSAP_BODY + 8] == 0x02 &&
		 f_look[VMS_OFF_SYSAP_BODY + 9] == 0x01 &&
		 f_again[VMS_OFF_SYSAP_BODY + 8] == 0x02 &&
		 f_again[VMS_OFF_SYSAP_BODY + 9] == 0x01,
		 "both requests are cat-0x02 op-0x01");
	ct_check(f_step[VMS_OFF_SYSAP_BODY + 8] == 0x01 &&
		 f_step[VMS_OFF_SYSAP_BODY + 9] == 0x0c &&
		 f_step[VMS_OFF_CM_STEP] == 0x0a,
		 "the frame before the stalled one releases barrier step 0x0a");
	ct_check(memcmp(body_of(f_look) + 24, body_of(f_again) + 24, 4) == 0,
		 "the stalled request names the SAME lock (body[24:28]) the "
		 "earlier lookup did");
	ct_check(f_ans[VMS_OFF_DLM_DIR_STATUS] == VMS_DLM_DIR_YOU_MASTER &&
		 memcmp(body_of(f_ans) + 4, body_of(f_look) + 4, 4) == 0,
		 "the earlier lookup was answered 0xf9 on its own token");
	ct_check(vms_dlm_res_ident_parse_body(body_of(f_look), VMS_CM_BODY_LEN,
					      &id) == VMS_CODEC_OK &&
		 vms_dlm_res_ident_parse_body(body_of(f_again), VMS_CM_BODY_LEN,
					      &id2) == VMS_CODEC_OK,
		 "both name a root resource the shipping parser reads");
	ct_check(id.name_len == id2.name_len && id.hash == id2.hash &&
		 memcmp(id.name, id2.name, id.name_len) == 0,
		 "...the same resource, with the same wire hash");

	/* BEFORE THE JOIN: the lookup is this node's, answered and recorded. */
	club_two();
	ct_check(vms_ldwv_is_ours(&g_cl.club.ldwv, key_of(f_look)),
		 "two members: the vector sends the lookup HERE, where VAXC "
		 "sent it");
	(void)vms_dlm_dir_init(&dir, store, 16u);
	o = vms_dlm_dir_lookup_directed(&dir, &id, CSID_VAXC, CSID_FIRST,
					directed_for(&id, CSID_VAXC), &m);
	ct_check(o == VMS_DLM_DIR_ANSWER_YOU && m == CSID_VAXC,
		 "the first lookup is answered you-master-it, as the wire shows");
	entries = dir.used;

	/* THE JOIN'S PHASE 2. */
	club_join_second();
	if (rec_first != 0u) {
		ct_check(vms_ldwv_is_ours(&g_cl.club.ldwv, key_of(f_r1)),
			 "VAXC's own rebuild record to this node is directed "
			 "here by the three-member vector");
		ct_check(vms_ldwv_names(&g_cl.club.ldwv, key_of(f_r2),
					CSID_SECOND),
			 "...and its record to the joiner is directed at the "
			 "joiner: the vector is the real VAX's own");
	}
	ct_check(!vms_ldwv_is_ours(&g_cl.club.ldwv, key_of(f_again)),
		 "three members: the vector no longer directs the name here");
	ct_check(vms_ldwv_names(&g_cl.club.ldwv, key_of(f_again), CSID_VAXC),
		 "...it directs it at VAXC, the requester itself");
	ct_check(directed_for(&id2, CSID_VAXC) == VMS_DLM_DIR_AT_REQUESTER,
		 "the arm's reading: directed at the requester");

	/* *** THE STALLED REQUEST *** */
	m = 0u;
	o = vms_dlm_dir_lookup_directed(&dir, &id2, CSID_VAXC, CSID_FIRST,
					directed_for(&id2, CSID_VAXC), &m);
	ct_check(o == VMS_DLM_DIR_ANSWER_YOU && m == CSID_VAXC, answered);
	ct_check_eq_u32(dir.used, entries,
			"nothing new is recorded for it");
	ct_check_eq_u32(dir.answered_at_requester, 1u,
			"it is counted as the requester's-own-directory answer");
	ct_check_eq_u32(dir.undirected_refused, 0u, "and not as a refusal");
	memset(tx, 0, sizeof(tx));
	ct_check(vms_dlm_dir_answer_build(body_of(f_again), VMS_CM_BODY_LEN,
					  VMS_DLM_DIR_YOU_MASTER, CSID_VAXC, tx,
					  (uint32_t)sizeof(tx), &written) ==
		 VMS_CODEC_OK &&
		 tx[VMS_OFF_DLM_DIR_STATUS] == VMS_DLM_DIR_YOU_MASTER &&
		 memcmp(body_of(tx) + 4, body_of(f_again) + 4, 4) == 0,
		 "the grounded builder makes the 0xf9 answer on the stalled "
		 "request's own token");

	/* THE b4d FLOOR STANDS. */
	(void)vms_dlm_dir_init(&dir, store, 16u);
	o = vms_dlm_dir_lookup_directed(&dir, &id2, CSID_VAXC, CSID_FIRST,
					VMS_DLM_DIR_AT_REQUESTER, &m);
	ct_check_eq_u32(o, VMS_DLM_DIR_ANSWER_NONE,
			"an EMPTY table is still never answered you-master-it, "
			"even with the requester its own directory");
	ct_check(vms_dlm_dir_find(&dir, &id2) == NULL,
		 "...and nothing is recorded");
	(void)vms_dlm_dir_register(&dir, &id2, CSID_SECOND);
	m = 0u;
	o = vms_dlm_dir_lookup_directed(&dir, &id2, CSID_VAXC, CSID_FIRST,
					VMS_DLM_DIR_AT_REQUESTER, &m);
	ct_check(o == VMS_DLM_DIR_ANSWER_REDIRECT && m == CSID_SECOND,
		 "an entry naming ANOTHER master is still that master's "
		 "redirect");
	(void)vms_dlm_dir_init(&dir, store, 16u);
	(void)vms_dlm_dir_register(&dir, &id2, CSID_VAXC);
	o = vms_dlm_dir_lookup_directed(&dir, &id2, CSID_VAXC, CSID_FIRST, 0,
					&m);
	ct_check_eq_u32(o, VMS_DLM_DIR_ANSWER_NONE,
			"directed at a THIRD system, an entry naming the "
			"requester is still not answered (rd vms-b4d)");
	ct_check(!vms_ldwv_names(&g_cl.club.ldwv, key_of(f_again),
				 CSID_SECOND) &&
		 !vms_ldwv_names(&g_cl.club.ldwv, key_of(f_again), 0u),
		 "the vector names exactly one system for the name");
}

int main(void)
{
	one_run("r3-frames.txt", "r3 (A,B,C), OVMXB", 4159u, 4168u, 4820u,
		4821u, 4702u, 4701u,
		"*** r3 (A,B,C), OVMXB: VAXC's op-0x01 at step 0x0a is ANSWERED you-master-it from the entry naming it -- the demo got no answer ***");
	one_run("r5-frames.txt", "r5 (C,A,B), OVMXA", 3180u, 3186u, 3858u,
		3859u, 0u, 0u,
		"*** r5 (C,A,B), OVMXA: VAXC's op-0x01 at step 0x0a is ANSWERED you-master-it from the entry naming it -- the demo got no answer ***");
	return ct_summary("test_dlm_transition_enq");
}
