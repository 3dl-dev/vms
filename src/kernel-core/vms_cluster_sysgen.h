/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_sysgen.h - load the cluster SYSGEN parameters into the
 * executive's ONE struct vms_cluster (FC-P0.10).
 *
 * Design: docs/design-faithful-cluster-executive.md, plan row FC-P0.10.
 * struct vms_cluster_params (vms_cluster.h section 2) is already the typed
 * SYSGEN store the cluster stack reads; this pair (.h/.c) is the ONE place
 * that VALIDATES a caller-supplied set of those fields and, only if valid,
 * commits it into a real struct vms_cluster -- so VMS_IOCTL_SYSGEN_LOAD's
 * per-substrate dispatcher (vms_devtab.c on Linux, vms_netbsd.c on NetBSD)
 * has no validation logic of its own to keep in sync between the two.
 *
 * PURE, like vms_cnxman_csb.c and vms_cluster_api.c beside it: no kernel
 * header, no substrate idiom, no SS$_ status vocabulary (that translation is
 * the ioctl dispatcher's job, exactly as vms_cluster_api.c's $GETSYI
 * projection returns nothing but the view it filled). Builds at R1 with a
 * plain host compiler (-DOVMX_CLUSTER_HOST) and is exercised in milliseconds
 * there, including the negctl this item's plan row names.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CLUSTER_SYSGEN_H
#define OVMX_VMS_CLUSTER_SYSGEN_H

#include "vms_cluster.h"

/*
 * cluster_sysgen_load - validate *in, then, only if valid, copy it into
 * cl->params.
 *
 * The one rule vms_cluster.h's own struct vms_cluster_params comment states
 * ("identity ... fatal if absent with vaxcluster >= 1, as on VMS"): a node
 * configured to ever join or always run in a cluster (VAXCLUSTER 1 or 2)
 * needs a configured SCSNODE. VAXCLUSTER 0 never attempts to join, so an
 * unconfigured node name in that mode is not an error -- SYSGEN was simply
 * never asked to load a cluster identity.
 *
 * Returns 1 (valid; cl->params now holds *in) or 0 (invalid; cl->params is
 * left at whatever it already honestly held -- INV-6: never a half-written
 * or fabricated identity). The caller maps 0 to SS$_BADPARAM.
 */
int cluster_sysgen_load(struct vms_cluster *cl,
                        const struct vms_cluster_params *in);

/*
 * cluster_sysgen_sw_version - the 8-byte software-version identity this node
 * may BROADCAST (SCS START body abs 72), read out of the loaded parameters.
 *
 * Writes `out' and returns 1 only when a boot actually committed a token
 * (cl->params_valid AND a nonzero sw_version_len): the value is the loaded
 * token, left-justified and BLANK-padded to VMS_CLUSTER_SWVER_LEN, which is
 * the field's own convention (the VAX renders it verbatim).
 *
 * Returns 0 -- and leaves `out' untouched -- when this node has nothing to
 * assert. There is no default: kernel-core holds no version literal (INV-1),
 * and a peer's own "VMS V7.3" is that peer's identity, never ours (INV-6).
 * The caller then advertises zeros and COUNTS the omission.
 */
int cluster_sysgen_sw_version(const struct vms_cluster *cl,
                              uint8_t out[VMS_CLUSTER_SWVER_LEN]);

/*
 * cluster_sysgen_credits - SYSGEN CLUSTER_CREDITS: the number of receive
 * buffers this node ASKS its cluster port to commit to each virtual circuit
 * (p. 2-43 -- the credit a node extends is the count of buffers it allocated
 * to receive that peer's messages), read out of the loaded parameters.
 *
 * IT IS A REQUEST, NOT THE WIRE VALUE (E60). What a START body advertises at
 * abs 95 is what the port's credit ledger actually GRANTED out of the receive
 * buffers it really allocated (vms_pe_fsm.h SS4b): equal to this when the pool
 * can back it, smaller when it cannot. Nothing may put this number on the wire
 * directly -- a configured request is not an allocation, and advertising one
 * as though it were is the promise-without-buffers this split exists to stop.
 *
 * Writes `*out' and returns 1 only when a boot actually committed the
 * parameters: a configured 0 is a real request for nothing and IS reported,
 * which is precisely why cl->params_valid, not the value, decides.
 *
 * Returns 0 when the parameters were never loaded, and ALSO when the
 * configured value cannot be expressed in the field (> 255): truncating 256 to
 * 0, or to 255, would put a credit window on the wire that this node did not
 * configure -- a promise it never made. Honest omission instead (INV-6).
 */
int cluster_sysgen_credits(const struct vms_cluster *cl, uint8_t *out);

/*
 * cluster_sysgen_clean_depart - OVMX_CLEAN_DEPART (rd vms-abd): may this node
 * ANNOUNCE its departure at the SCS layer when it leaves the cluster?
 *
 * Nonzero = yes, a symmetric DISCONNECT_REQUEST goes out on every open
 * connection at VMS_IOCTL_CLUSTER_STOP, which is what a real VMS node leaving
 * through SHUTDOWN.COM does. Zero = no, and the survivors fall back on the PE
 * last gasp and their own RECNXINTERVAL timers.
 *
 * THIS IS THE WHOLE KILL SWITCH, and it lives here -- in the pure, host-linkable
 * parameter TU -- rather than as an `if' inside the connection manager, for one
 * reason: vms_cnxman.c is not host-linkable, so a gate written there could only
 * ever be proved by grepping the source. Here the R1 rung drives the REAL
 * decision function, and the departure asks it exactly once.
 *
 * A NODE THAT NEVER LOADED ITS PARAMETERS ANSWERS 0. That is not the switch
 * being off: it is the honest reading that this executive holds no parameter
 * record at all, and the same test cluster_sysgen_credits() applies for the same
 * reason (INV-6 -- an unloaded 0 is not a configured 0). It costs nothing here,
 * because a node with no committed SYSGEN parameters never joined a cluster and
 * so has no connection to announce a departure on.
 */
int cluster_sysgen_clean_depart(const struct vms_cluster *cl);

/*
 * cluster_sysgen_depart_from_wire - turn VMS_IOCTL_SYSGEN_LOAD's NEGATED wire
 * byte (`clean_depart_off') into the POSITIVE sense the executive stores.
 *
 * ONE LINE, AND IT IS A NAMED FUNCTION ANYWAY, for two reasons.
 *
 * FIRST, THE POLARITY IS THE WHOLE ANTI-HOLLOW ARGUMENT AND HAS TO BE TESTABLE.
 * sysgen_read_param() cannot distinguish "the operator set it to 0" from "this
 * .PAR predates the parameter": an absent read leaves the caller's memset zero
 * standing. Every existing OVMXVMSSYS.PAR is in the second case. So the wire
 * field carries the NEGATION -- 0 means "nothing was turned off", which is the
 * faithful default -- and an operator's explicit 0 is carried down as a 1 here.
 * Get that inversion backwards and the clean departure is silently OFF on every
 * install that already exists, which is a switch that reads as implemented and
 * behaves as absent. The R1 rung drives THIS function with the zero-filled byte
 * an absent parameter really produces.
 *
 * SECOND, it lives in this PURE TU rather than inside vms_devtab.c's static
 * copier because that copier is not host-linkable: a polarity written there
 * could only ever be proved by grepping the source.
 *
 * Returns what belongs in struct vms_cluster_params.clean_depart.
 */
uint8_t cluster_sysgen_depart_from_wire(uint8_t clean_depart_off);

#endif /* OVMX_VMS_CLUSTER_SYSGEN_H */
