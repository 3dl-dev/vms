/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fuzz_dlm_dir_directed.c - rd vms-b4d: never-crash-a-peer fuzz of the
 * directory half of the op-0x01 RECEIVE path, with the vector's answer.
 *
 * What a peer's op-0x01 reaches on this node, minus the glue TU that does not
 * link on the host (vms_dlm_scs.c dlm_arm_dir_lookup): the shipping identity
 * parse (vms_dlm_res_ident_parse_body), the shipping directory table asked
 * with the vector's answer (vms_dlm_dir_lookup_directed), and -- when an answer
 * is due -- the shipping answer builder (vms_dlm_dir_answer_build) over the
 * request's own bytes. Bodies are random, truncated, or byte-mutated copies of
 * a real-shaped op-0x01 built by the shipping builder; the table is seeded with
 * random entries; "directed here" is random.
 *
 * THE BAR, fuzz_common.h's plus this item's property:
 *   - every body lives in an EXACT-sized heap allocation (ASan/UBSan twin of
 *     the codec, and this TU's own copy of vms_dlm_dir.c, are instrumented):
 *     no over-read, no UB, no crash;
 *   - a name the vector does NOT direct here, for which the table holds no
 *     entry (or one naming the requester), is NEVER answered "you master it"
 *     (0xf9), records nothing, and is counted -- whatever the bytes;
 *   - non-vacuity: the parse is reached, and both a directed YOU and an
 *     undirected refusal really happen inside the run.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_dir.h"

#include <stdio.h>

#define CSID_SELF 0x00010004u
#define CSID_VAX1 0x00010002u
#define CSID_VAX2 0x00010003u

#define DIR_CAP 32u

static struct vms_dlm_dir g_dir;
static struct vms_dlm_dir_entry g_store[DIR_CAP];

static uint8_t g_seed[VMS_CM_BODY_LEN];

/* A real-shaped op-0x01 body, built by the shipping builder. */
static int build_seed(void)
{
	struct vms_dlm_enq_request r;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0;

	memset(&r, 0, sizeof(r));
	r.mode = 5u;
	r.req_lkid = 0x0a0b0001u;
	r.name_len = 4u;
	memcpy(r.name, "WKLD", 4);
	r.dir_hash = 0x12345678u;
	r.dir_hash_valid = 1u;
	r.res_group = 0u;
	r.res_acmode = 0u;
	r.res_ident_valid = 1u;
	r.req_pid = 0x20200204u;
	memset(frame, 0, sizeof(frame));
	if (vms_dlm_enq_request_build(&r, VMS_DLM_WIREOP_ENQ, frame,
				      (uint32_t)sizeof(frame), &written) !=
	    VMS_CODEC_OK)
		return -1;
	memcpy(g_seed, frame + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	return 0;
}

static vms_csid_t pick_csid(void)
{
	switch (rand() % 6) {
	case 0: return CSID_VAX1;
	case 1: return CSID_VAX2;
	case 2: return CSID_SELF;
	case 3: return 0u;
	default: return (vms_csid_t)(0x00010000u | (uint32_t)(rand() % 8));
	}
}

int main(void)
{
	uint8_t scratch[VMS_CM_BODY_LEN + 16];
	uint8_t tx[VMS_CM_FRAME_LEN];
	uint32_t parsed = 0, directed_you = 0, undirected_refused = 0;
	uint32_t violations = 0, recorded_undirected = 0, built = 0;
	int i;

	printf("=== fuzz_dlm_dir_directed (rd vms-b4d) ===\n");
	srand(0xb4d);
	ct_check(build_seed() == 0, "fuzz-b4d: the shipping builder makes the op-0x01 seed");
	(void)vms_dlm_dir_init(&g_dir, g_store, DIR_CAP);

	for (i = 0; i < FZ_ITERS; i++) {
		struct vms_dlm_res_ident id;
		const struct vms_dlm_dir_entry *e;
		vms_csid_t requester, master = 0u, before_master = 0u;
		uint32_t n, refused_before, written = 0;
		int had_entry, directed, k;
		enum vms_dlm_dir_outcome o;
		uint8_t *b;

		if (i % 1024 == 0)
			(void)vms_dlm_dir_init(&g_dir, g_store, DIR_CAP);

		/* The body: a mutated seed (mostly), or random bytes. */
		if (rand() % 4 != 0) {
			n = (rand() % 8 == 0) ? fz_len(VMS_CM_BODY_LEN) :
						VMS_CM_BODY_LEN;
			if (n > VMS_CM_BODY_LEN)
				n = VMS_CM_BODY_LEN;
			memcpy(scratch, g_seed, n);
			for (k = rand() % 4; k > 0 && n != 0u; k--)
				scratch[rand() % n] ^= (uint8_t)(1 + rand() % 255);
		} else {
			n = fz_len(VMS_CM_BODY_LEN + 8u);
			if (n > sizeof(scratch))
				n = sizeof(scratch);
			fz_fill(scratch, n);
		}
		b = fz_exact(scratch, n);
		if (b == NULL)
			continue;
		if (vms_dlm_res_ident_parse_body(b, n, &id) != VMS_CODEC_OK) {
			free(b);
			continue;
		}
		parsed++;

		/* Random table state for this name: none, the requester, another
		 * member, or this node. */
		requester = pick_csid();
		switch (rand() % 4) {
		case 0: break;
		case 1: if (requester != 0u)
				(void)vms_dlm_dir_register(&g_dir, &id, requester);
			break;
		case 2: (void)vms_dlm_dir_register(&g_dir, &id, pick_csid()); break;
		default: (void)vms_dlm_dir_claim_self(&g_dir, &id, CSID_SELF); break;
		}
		e = vms_dlm_dir_find(&g_dir, &id);
		had_entry = e != NULL;
		before_master = had_entry ? e->master : 0u;
		directed = rand() & 1;
		refused_before = g_dir.undirected_refused;

		o = vms_dlm_dir_lookup_directed(&g_dir, &id, requester, CSID_SELF,
						directed, &master);

		if (!directed && requester != 0u &&
		    (!had_entry || before_master == requester)) {
			/* THE PROPERTY. */
			if (o != VMS_DLM_DIR_ANSWER_NONE)
				violations++;
			if (g_dir.undirected_refused != refused_before + 1u)
				violations++;
			if (!had_entry && vms_dlm_dir_find(&g_dir, &id) != NULL)
				recorded_undirected++;
			undirected_refused++;
		}
		if (!directed && o == VMS_DLM_DIR_ANSWER_YOU)
			violations++;
		if (directed && o == VMS_DLM_DIR_ANSWER_YOU)
			directed_you++;

		/* The answer the arm would build, from the request's own bytes. */
		if (o == VMS_DLM_DIR_ANSWER_YOU || o == VMS_DLM_DIR_ANSWER_REDIRECT) {
			memset(tx, 0, sizeof(tx));
			if (vms_dlm_dir_answer_build(b, n,
				    o == VMS_DLM_DIR_ANSWER_REDIRECT ?
				    VMS_DLM_DIR_REDIRECT : VMS_DLM_DIR_YOU_MASTER,
				    (uint32_t)master, tx, (uint32_t)sizeof(tx),
				    &written) == VMS_CODEC_OK)
				built++;
		}
		free(b);
	}

	printf("  parsed %u, directed YOU %u, undirected refusals %u, answers built %u\n",
	       parsed, directed_you, undirected_refused, built);
	ct_check(parsed > 1000u, "fuzz-b4d: the op-0x01 identity parse is genuinely reached");
	ct_check(directed_you > 0u, "fuzz-b4d: a directed name with no entry is answered you-master-it");
	ct_check(undirected_refused > 0u, "fuzz-b4d: undirected lookups with no entry really occur");
	ct_check_eq_u32(violations, 0u,
			"fuzz-b4d: an undirected name with no entry is never answered you-master-it");
	ct_check_eq_u32(recorded_undirected, 0u,
			"fuzz-b4d: an undirected lookup records nothing");
	return ct_summary("fuzz_dlm_dir_directed");
}
