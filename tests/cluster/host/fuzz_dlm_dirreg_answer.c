/* SPDX-License-Identifier: GPL-2.0 */
/*
 * fuzz_dlm_dirreg_answer.c - rd vms-fdeb: never-crash-a-peer fuzz of the
 * receive side of a MASTER's directory re-registration -- what a peer's 82/0d
 * (or anything else that reaches vms_dlm_rm_dr_answer) does to this node.
 *
 * The shipping table (vms_dlm_rm_fsm.c, compiled into this fuzz with the
 * sanitizers) is opened at release 4 against fake engine doors that hold a
 * small set of resources this node masters, so up to VMS_DLM_DR_WINDOW records
 * are on the "wire". Then every iteration hands vms_dlm_rm_dr_answer a body
 * from a random sender: an exact echo of one of OUR records stamped 0xf9 (the
 * real recipe, 1487 of 1487 real answers), a byte-mutated or truncated copy of
 * one, or random bytes; and now and then the window is re-opened, expired or
 * closed.
 *
 * THE BAR, fuzz_common.h's plus this item's properties:
 *   - every body lives in an EXACT-sized heap allocation (the codec twin and
 *     this TU's copy of the table are instrumented): no over-read, no UB, no
 *     crash, whatever the bytes;
 *   - the engine is told a directory holds an entry ONLY for a record really
 *     in flight to THAT sender, for exactly that identity -- a forged, mutated
 *     or misaddressed answer never makes a directory "told";
 *   - in-flight records never exceed the window, and an answer frees at most
 *     one;
 *   - non-vacuity: answers really match, and forged ones are really refused.
 */
#include "fuzz_common.h"
#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_dlm_rm_fsm.h"

#include <stdio.h>

#define CSID_SELF 0x00010004u
#define CSID_DIR1 0x00010002u
#define CSID_DIR2 0x00010003u

#define N_RES 12u

struct fake_res {
	struct vms_dlm_res_ident id;
	uint32_t dir;
	uint32_t told;      /* the directory the engine was told, 0 = none */
};

static struct fake_res g_res[N_RES];

/* What is on the "wire" right now: the last record sent to each identity. */
static struct { uint8_t body[VMS_CM_BODY_LEN]; uint32_t dst; int live; }
	g_wire[N_RES];
static uint32_t g_sent, g_told_bad, g_told_ok;

static int res_of(const struct vms_dlm_res_ident *id)
{
	uint32_t i;

	for (i = 0u; i < N_RES; i++) {
		if (g_res[i].id.name_len == id->name_len &&
		    g_res[i].id.group == id->group &&
		    g_res[i].id.mode == id->mode &&
		    memcmp(g_res[i].id.name, id->name, id->name_len) == 0)
			return (int)i;
	}
	return -1;
}

static int fk_send(void *ctx, uint32_t dst, const uint8_t *b, uint32_t len)
{
	struct vms_dlm_dir_reg r;
	int i;

	(void)ctx;
	if (len != VMS_CM_BODY_LEN)
		return -1;
	if (vms_dlm_dir_reg_parse_body(b, len, &r) != VMS_CODEC_OK) {
		/* The builder leaves body[12:14] to the connection manager. */
		uint8_t t[VMS_CM_BODY_LEN];

		memcpy(t, b, len);
		t[12] = 0x01;
		if (vms_dlm_dir_reg_parse_body(t, len, &r) != VMS_CODEC_OK)
			return -1;
	}
	i = res_of(&r.id);
	if (i < 0)
		return -1;
	memcpy(g_wire[i].body, b, len);
	g_wire[i].body[12] = 0x01;     /* what the CM stamps */
	g_wire[i].body[14] = 0x03;
	g_wire[i].dst = dst;
	g_wire[i].live = 1;
	g_sent++;
	return 0;
}

static int fk_next(void *ctx, const struct vms_dlm_res_ident *skip,
		   uint32_t n_skip, struct vms_dlm_res_ident *out,
		   uint32_t *dir)
{
	uint32_t i, k;

	(void)ctx;
	for (i = 0u; i < N_RES; i++) {
		int skipped = 0;

		if (g_res[i].told != 0u)
			continue;
		for (k = 0u; k < n_skip; k++)
			if (res_of(&skip[k]) == (int)i)
				skipped = 1;
		if (skipped)
			continue;
		*out = g_res[i].id;
		*dir = g_res[i].dir;
		return 1;
	}
	return 0;
}

static int fk_told(void *ctx, const struct vms_dlm_res_ident *id, uint32_t dir)
{
	int i = res_of(id);

	(void)ctx;
	/* Told is legitimate only for a record live on the wire to `dir`. */
	if (i < 0 || !g_wire[i].live || g_wire[i].dst != dir ||
	    g_res[i].dir != dir) {
		g_told_bad++;
		return 0;
	}
	g_wire[i].live = 0;
	g_res[i].told = dir;
	g_told_ok++;
	return 1;
}

static void fk_log(void *ctx, const char *msg)
{
	(void)ctx;
	(void)msg;
}

static struct vms_dlm_rm_ops g_ops;
static struct vms_dlm_rm g_rm;

static void world_reset(void)
{
	uint32_t i;

	memset(g_res, 0, sizeof(g_res));
	memset(g_wire, 0, sizeof(g_wire));
	for (i = 0u; i < N_RES; i++) {
		struct fake_res *r = &g_res[i];

		r->id.name_len = (uint8_t)(1u + (uint32_t)rand() % 31u);
		fz_fill(r->id.name, r->id.name_len);
		r->id.group = (uint16_t)(rand() % 3 == 0 ? rand() : 0);
		r->id.mode = (uint8_t)(rand() % 4);
		r->id.hash = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
		r->dir = rand() % 2 ? CSID_DIR1 : CSID_DIR2;
	}
	vms_dlm_rm_dr_close(&g_rm);
	vms_dlm_rm_dr_barrier_released(&g_rm, VMS_DLM_DR_REL_STEP, 0u);
}

static uint32_t pick_csid(void)
{
	switch (rand() % 6) {
	case 0: return CSID_DIR1;
	case 1: return CSID_DIR2;
	case 2: return CSID_SELF;
	case 3: return 0u;
	default: return 0x00010000u | (uint32_t)(rand() % 8);
	}
}

int main(void)
{
	uint8_t scratch[VMS_CM_BODY_LEN + 16];
	uint32_t matched = 0, refused = 0, window_bad = 0, multi_free = 0;
	int i;

	printf("=== fuzz_dlm_dirreg_answer (rd vms-fdeb) ===\n");
	srand(0xfdeb);
	memset(&g_ops, 0, sizeof(g_ops));
	g_ops.send = fk_send;
	g_ops.log = fk_log;
	g_ops.dr_next = fk_next;
	g_ops.dr_told = fk_told;
	vms_dlm_rm_init(&g_rm, &g_ops);
	world_reset();

	for (i = 0; i < FZ_ITERS; i++) {
		uint32_t n, from, before, after;
		int k, src, consumed;
		uint8_t *b;

		switch (rand() % 64) {
		case 0:
			world_reset();
			break;
		case 1:
			(void)vms_dlm_rm_dr_expire(&g_rm, 30000u, 20000u);
			break;
		case 2:
			vms_dlm_rm_dr_barrier_released(&g_rm,
						       (uint32_t)(rand() % 13), 0u);
			break;
		default:
			break;
		}

		/* The body: an echo of one of our records (mostly), mutated or
		 * not, or random bytes. */
		src = (int)((uint32_t)rand() % N_RES);
		if (rand() % 4 != 0 && g_wire[src].live) {
			n = VMS_CM_BODY_LEN;
			memcpy(scratch, g_wire[src].body, n);
			scratch[8] = 0x82;
			scratch[34] = rand() % 16 ? 0xf9 : (uint8_t)rand();
			if (rand() % 3 == 0) {
				for (k = 1 + rand() % 3; k > 0; k--)
					scratch[rand() % n] ^=
						(uint8_t)(1 + rand() % 255);
			}
			if (rand() % 8 == 0)
				n = fz_len(VMS_CM_BODY_LEN);
			from = rand() % 4 ? g_wire[src].dst : pick_csid();
		} else {
			n = fz_len(VMS_CM_BODY_LEN + 8u);
			if (n > sizeof(scratch))
				n = sizeof(scratch);
			fz_fill(scratch, n);
			from = pick_csid();
		}
		b = fz_exact(scratch, n);
		if (b == NULL)
			continue;
		before = vms_dlm_rm_dr_outstanding(&g_rm);
		consumed = vms_dlm_rm_dr_answer(&g_rm, from,
						n > 8u ? b[8] : 0u,
						n > 9u ? b[9] : 0u, b, n);
		after = vms_dlm_rm_dr_outstanding(&g_rm);
		free(b);
		if (consumed)
			matched++;
		else
			refused++;
		if (after > VMS_DLM_DR_WINDOW)
			window_bad++;
		/* One answer frees one slot; the pump may refill it at once. */
		if (before > after + 1u)
			multi_free++;
	}

	ct_check(g_sent > 1000u, "fuzz-fdeb: records are really on the wire");
	ct_check(matched > 1000u, "fuzz-fdeb: real-recipe answers really match");
	ct_check(refused > 1000u, "fuzz-fdeb: forged / mutated answers really occur");
	ct_check(g_told_ok > 1000u, "fuzz-fdeb: matched answers tell the engine");
	ct_check_eq_u32(g_told_bad, 0u,
			"fuzz-fdeb: no answer ever makes a directory 'told' that "
			"no record of ours went to");
	ct_check_eq_u32(window_bad, 0u, "fuzz-fdeb: in flight never exceeds the window");
	ct_check_eq_u32(multi_free, 0u, "fuzz-fdeb: an answer frees at most one record");
	return ct_summary("fuzz_dlm_dirreg_answer");
}
