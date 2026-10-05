/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_dir.h - the LOCK DIRECTORY's entries: which system masters a
 * resource, for the resources whose directory node is THIS one (rd vms-8219;
 * plan row FC-P5.4).
 *
 * On VMS every resource tree is mastered on one node and found through its
 * DIRECTORY node, entry[(hash >> 16) mod n] of the Lock Directory Weight
 * Vector (vms_dlm_ldwv.h). A directory node holds, for each root resource
 * that maps to it, the CSID of the system that masters it (Davis pp. 6-31,
 * 6-50). With every member at the V7.3 default LOCKDIRWT 0, p. 6-32 makes
 * EVERY member a directory node -- OVMX included -- so a real VAX addresses
 * lookups, removals and rebuild registrations to an OVMX member.
 *
 * WHAT THIS OBJECT IS. A pure, fixed-capacity table keyed by the resource's
 * WIRE identity (group, access mode, name; vms_cluster_codec_dlm.h struct
 * vms_dlm_res_ident) with the master's CSID as the value -- and the one
 * decision a directory makes on a lookup (vms_dlm_dir_lookup). Storage is
 * the caller's; nothing here allocates, sleeps, logs or reads a clock, so
 * it is host-unit-testable as-is.
 *
 * INV-6. Every entry is a fact the cluster put on the wire: a registration
 * from the master itself (op-0x0d), or a lookup this directory answered
 * "you master it" (and therefore MADE true). A full table is refused, never
 * answered -- answering "you master it" without recording it would let the
 * next asker become a second master.
 *
 * INCLUDES: kernel-core headers only.
 */
#ifndef OVMX_VMS_DLM_DIR_H
#define OVMX_VMS_DLM_DIR_H

#include "vms_cluster.h"            /* vms_csid_t */
#include "vms_cluster_codec_dlm.h"  /* struct vms_dlm_res_ident */

#ifdef __cplusplus
extern "C" {
#endif

/* Capacity of the production table: a power of two. An idle three-node V7.3
 * cluster put 939 root resources on the wire in minutes; with n = 3 a node
 * directs about a third of them. */
#define VMS_DLM_DIR_CAP 4096u

enum vms_dlm_dir_slot_state {
	VMS_DLM_DIR_EMPTY = 0,
	VMS_DLM_DIR_USED  = 1
};

struct vms_dlm_dir_entry {
	struct vms_dlm_res_ident id;
	vms_csid_t master;
	uint8_t    state;
	uint8_t    pad[3];
};

struct vms_dlm_dir {
	struct vms_dlm_dir_entry *slot;
	uint32_t cap;              /* power of two; 0 = no table */
	uint32_t used;

	/* Counted facts. */
	uint32_t registered;       /* entries set from a master's registration */
	uint32_t answered_you;     /* lookups answered "you master it"         */
	uint32_t answered_redirect;/* lookups answered "the master is X"       */
	uint32_t removed;          /* entries removed by their master          */
	uint32_t remove_unknown;   /* a removal for no entry, or another master*/
	uint32_t full_refusals;    /* a new entry that did not fit             */
	uint32_t dropped;          /* entries dropped by a transition          */
};

enum vms_dlm_dir_outcome {
	VMS_DLM_DIR_ANSWER_YOU      = 0,   /* body[34] 0xf9: requester masters */
	VMS_DLM_DIR_ANSWER_REDIRECT = 1,   /* body[34] 0xf8: master is *out    */
	VMS_DLM_DIR_ANSWER_NONE     = 2    /* table full / no table: refuse     */
};

/* `storage` holds `cap` entries; cap must be a power of two. 0 on success. */
int vms_dlm_dir_init(struct vms_dlm_dir *d, struct vms_dlm_dir_entry *storage,
		     uint32_t cap);

/* The entry for `id`, or NULL. */
const struct vms_dlm_dir_entry *vms_dlm_dir_find(const struct vms_dlm_dir *d,
						 const struct vms_dlm_res_ident *id);

/* A master registers a resource (op-0x0d): set or overwrite. 0 on success,
 * -1 when the table is full. */
int vms_dlm_dir_register(struct vms_dlm_dir *d,
			 const struct vms_dlm_res_ident *id, vms_csid_t master);

/*
 * THE LOOKUP (p. 6-31 outcomes 2 and 3). An entry naming another system ->
 * REDIRECT with *out_master. No entry -> the requester becomes the master:
 * recorded, then YOU. An entry naming the requester itself -> YOU. A full
 * table -> NONE (nothing recorded, nothing to answer).
 */
enum vms_dlm_dir_outcome vms_dlm_dir_lookup(struct vms_dlm_dir *d,
					    const struct vms_dlm_res_ident *id,
					    vms_csid_t requester,
					    vms_csid_t *out_master);

/* The master removes its entry (op-0x04). Only the master named by the entry
 * may remove it. 0 when removed, -1 otherwise (counted). */
int vms_dlm_dir_remove(struct vms_dlm_dir *d,
		       const struct vms_dlm_res_ident *id, vms_csid_t master);

/* A member left: every entry it mastered is gone with it. Returns how many. */
uint32_t vms_dlm_dir_drop_master(struct vms_dlm_dir *d, vms_csid_t master);

/* The vector changed: drop every entry `keep(ctx, hash)` says this node no
 * longer directs. Returns how many. */
uint32_t vms_dlm_dir_drop_unless(struct vms_dlm_dir *d,
				 int (*keep)(void *ctx, uint32_t hash),
				 void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* OVMX_VMS_DLM_DIR_H */
