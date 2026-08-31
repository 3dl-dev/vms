/*
 * vms_kif.h - Kernel Interface for VMS userspace
 *
 * Provides userspace wrappers around the /dev/vms ioctl interface.
 * These functions abstract the ioctl calls behind VMS-style APIs.
 *
 * Usage:
 *   Just call the vms_kif_* function you want. Every one of them completes
 *   the open -> register sequence for the calling task first, once, and the
 *   registration is re-established automatically after fork(), in a new
 *   thread, and in a freshly activated image after execve().
 *
 * vms_kif_open() and vms_kif_register() remain available for callers that
 * want to drive the sequence explicitly (and to observe REGISTER's status),
 * but NO caller is required to remember them. Requiring it was the bug:
 * this header documented the two-step protocol from the day it was written,
 * vms_kif_register() had zero callers product-wide, and so every /dev/vms
 * ioctl issued by OVMX was rejected with -ESRCH by the executive (vms-9fc).
 *
 * UNWIRED DECLARATIONS (vms-7fb). An entry point below that has NO product
 * caller carries a machine-checked declaration line naming itself and the
 * item tracking it -- see any of the declarations below for the form, and
 * tests/integration/test_kif_caller_census.sh for the reader that parses
 * them. The census enforces the pair in both directions: an entry point
 * with no product path and no declaration fails the build, and so does a
 * declaration left behind on an entry point that has since been wired.
 *
 * The census exists because a kernel facility, a wrapper and a test suite
 * were merged more than once WITH NO PRODUCT PATH -- most of this interface
 * is in that state today, and every declaration below is a statement that
 * OVMX does not yet use the facility, not a permission slip to leave it
 * that way.
 */

#ifndef _VMS_KIF_H
#define _VMS_KIF_H

#include <stdint.h>
#include "../kernel/vms_ioctl.h"

/* ================================================================
 * Connection management
 * ================================================================ */

/* Open /dev/vms, returns fd or -1 on error */
int vms_kif_open(void);

/* Close the /dev/vms fd.
 *
 * OVMX-UNWIRED: vms_kif_close (vms-a86) -- nothing in OVMX closes the
 * executive channel; PID 1 pins it for the life of the system (Rule 9),
 * and kif_bind() drops a fork-inherited descriptor with vms_sys_close()
 * directly. Open question in vms-a86: should this exist at all? */
void vms_kif_close(void);

/* Register this process with the kernel module.
 *
 * The unit is the PROCESS, not the thread: the executive holds one PCB per
 * process (keyed by the thread-group id) and every thread of an image
 * shares it, exactly as VMS kernel threads share one PCB. So a second
 * thread registering adopts the process's existing entry and sees the same
 * process name, the same event flag clusters and the same lock ids.
 *
 * Idempotent: registering a task the executive already knows ADOPTS the
 * existing entry and returns SS$_NORMAL, leaving its identity and its
 * privilege mask untouched. That is what VMS does -- activating an image
 * inside a process does not recreate the process (oracle pin: on the
 * reference lab VAX V7.3, SHOW PROCESS/ACCOUNTING across two image
 * activations reports the same Process ID 2020021D and the same process
 * name "SYSTEM" with "Images activated" going 19 -> 21).
 *
 * TAKES NO PRIVILEGE MASK AND NO PROCESS ID (vms-2b8). Registration
 * proves only that a task exists; the executive derives the authorized
 * privilege mask and the UIC from the task's real credentials, and
 * ASSIGNS the VMS process ID. A process that could name its own
 * privileges here would be enforcing them against itself, and a process
 * that could name its own VMS process ID could collide with a privileged
 * process's row and be resolved in its place.
 *
 * vms_pid is OUT, and may be NULL: on success it receives the ID the
 * executive assigned. */
uint32_t vms_kif_register(uint32_t *vms_pid);

/* Register the CALLING task as a CONTINUATION of its VMS parent's process
 * rather than a new one (vms-4d7, Option B): the executive shares the
 * parent's VMS PID, UIC, user name and privileges onto this task. DCL
 * calls this in the forked child of an image activation, before execve, so
 * the activated image runs with DCL's identity -- SYSTEM's RUN AUTHORIZE
 * holds SYSPRV. Not for SPAWN / RUN/DETACHED / $CREPRC, which are genuinely
 * new VMS processes and use vms_kif_register(). */
uint32_t vms_kif_register_continue(void);

/* Register the CALLING task as a genuinely NEW VMS process (a fresh, distinct
 * VMS PID) that INHERITS its creator's executive identity -- UIC, user name and
 * privileges -- from its real_parent (vms-19e9). $CREPRC's forked child calls
 * this before it execs, so a SPAWNed subprocess of a NON-ROOT named session
 * (the interactive DCL after LOGINOUT's credential drop) becomes that session's
 * identity by CONTINUATION -- which vms_kif_setident() cannot do, since the
 * executive refuses a non-root self-declared privileged name. This is $CREPRC /
 * SPAWN; image activation (same VMS process, shared PID) uses
 * vms_kif_register_continue() instead. */
uint32_t vms_kif_register_subprocess(void);

/* Stamp an AUTHENTICATED identity onto this process ($GETJPI reads it
 * back, from any process). The caller must already hold SETPRV to
 * establish an identity that is not a weakening of its own -- so this
 * is LOGINOUT's call, made after SYSUAF authentication, and it is a
 * one-way drop for anyone else. SS$_NOPRIV if the caller may not.
 *
 * uic is (group << 16) | member. authorized_privs is the SYSUAF
 * uaf$q_priv quadword; the executive sets current privileges equal to
 * it (an OVMX design choice -- see vms_ioctl.h).
 *
 * WIRED (vms-2b8): tools/vms_login.c (LOGINOUT) calls this after SYSUAF
 * authentication -- an arbitrary authenticated user, which is exactly what
 * this call is for. src/vmsdcl/dcl_main.c and dcl_cmd_show.c read the row
 * back through vms_kif_getjpi_self() instead of the environment.
 *
 * src/ovmx_provision/ovmx_provision.c (PROVISION.EXE) used to call this
 * too, for the fixed SYSTEM identity system startup runs under -- moved to
 * vms_kif_establish_system() below (vms-a17e), because SYSTEM is not an
 * arbitrary authenticated user: it is a constant the executive itself now
 * constructs. */
uint32_t vms_kif_setident(const char *username, uint32_t uic,
                          uint64_t authorized_privs);

/* As vms_kif_setident(), but also establishes the authorized JIB quota set the
 * account carries (vms-14a). LOGINOUT uses this so identity and quota arrive in
 * one SETIDENT, from the same SYSUAF record it authenticated. quota == NULL is
 * identical to plain vms_kif_setident() (no sourced quota, executive omits
 * VMS_PI_V_QUOTA -- honest omission, INV-6). vms_kif_setident() is this with
 * quota == NULL, so the ~30 identity-only callers are unaffected. */
uint32_t vms_kif_setident_quota(const char *username, uint32_t uic,
                                uint64_t authorized_privs,
                                const struct vms_jib_quota *quota);

/* Construct the SYSTEM identity onto this process (vms-a17e). The
 * OPA0:-style counterpart to vms_kif_setident() above: no username, uic,
 * or privilege mask to supply -- SYSTEM/[1,4]/ALL are constants the
 * executive already owns (VMS_SYSTEM_UIC / VMS_PRV_M_SYSTEM_ALL,
 * src/kernel/vms_internal.h), the same way OPA0: is a constant
 * vms_devtab_init() creates at module load rather than a value a process
 * registers.
 *
 * WIRED: src/ovmx_provision/ovmx_provision.c (PROVISION.EXE) calls this in
 * place of a SYSUAF read + vms_kif_setident() -- so PROVISION.EXE never
 * opens SYS$SYSTEM:SYSUAF.DAT to become SYSTEM. Its
 * vms_kif_getjpi_self() readback afterward is unchanged: it still reports
 * the executive's verdict, never what it asked for -- there is simply
 * nothing left for it to have asked for.
 *
 * SS$_NOPRIV if the caller lacks CAP_SYS_ADMIN. */
uint32_t vms_kif_establish_system(void);

/* Translate a failed ioctl's negative errno into a VMS status.
 *
 * Exposed because it is a boundary translation with a testable contract,
 * the same shape as sys_lock.c's kstat_to_ss: the errno set vms.ko can
 * produce is closed, and each status it maps to is oracle-pinned. See the
 * definition in vms_kif.c for the pins. */
uint32_t vms_kif_kerr_to_ss(int err);

/* ================================================================
 * Access Mode (3a)
 *
 * STALE AS A BLANKET CLAIM (corrected here, not re-derived): setmode/getmode
 * remain unwired -- privileges and access modes are still whatever a process
 * says they are (src/vmsdcl/dcl_main.c reads VMS_PRIVILEGES from the
 * environment) -- but setprv and, as of vms-651, chkpriv are NOT: sys$setprv
 * routes real $SETPRV mutations through the executive, and cmd_mount/
 * cmd_dismount ask it whether PRV$M_MOUNT is held before touching a volume.
 * vms-pv1 is the item that makes the executive the enforcer for the REST of
 * this family.
 * ================================================================ */

/* Set access mode. Returns SS$_ status
 * OVMX-UNWIRED: vms_kif_setmode (vms-pv1) */
uint32_t vms_kif_setmode(uint8_t mode);

/* Get current mode and privileges
 * OVMX-UNWIRED: vms_kif_getmode (vms-pv1) */
uint32_t vms_kif_getmode(uint8_t *mode, uint64_t *cur_privs, uint64_t *perm_privs);

/* Set/clear privileges. Returns previous privilege mask in *prev.
 * Wired (vms-pv1): sys$setprv (src/libvms/syssvc/sys_misc.c) routes the $SETPRV
 * mutation here so the executive OWNS and AUTHORIZES the privilege state -- the
 * census gate is what proves it has a product caller. */
uint32_t vms_kif_setprv(uint64_t mask, int enable, int permanent, uint64_t *prev);

/* Check if privileges are held. Returns SS$_NORMAL or SS$_NOPRIV
 * Wired (vms-651): cmd_mount/cmd_dismount (src/vmsdcl/dcl_cmd_misc.c) check
 * PRV$M_MOUNT this way before mount(2)/umount(2)ing a volume -- the first
 * product path that asks the executive whether a privilege is held, rather
 * than trusting a self-declared mask. */
uint32_t vms_kif_chkpriv(uint64_t mask);

/* ================================================================
 * AST Delivery (3b)
 *
 * THE WHOLE FAMILY IS WIRED (vms-as1): src/libvms/syssvc/sys_ast.c is now a
 * translation layer over the executive -- sys$dclast calls vms_kif_dclast,
 * sys$setast calls vms_kif_setast and drains the queue through
 * vms_kif_deliverast. The AST queue, the per-mode enable flag and the quota
 * are the executive's (src/kernel/vms_ast.c), not this image's. Proof:
 * tests/qemu/test_syssvc_ast.c.
 * ================================================================ */

/* Declare AST at specified access mode */
uint32_t vms_kif_dclast(uint64_t astadr, uint64_t astprm, uint8_t acmode);

/* Enable/disable AST delivery. Returns SS$_WASSET or SS$_WASCLR */
uint32_t vms_kif_setast(int enable);

/* Deliver next pending AST. Returns 0 if AST delivered, -1 if none */
int vms_kif_deliverast(uint64_t *astadr, uint64_t *astprm, uint8_t *acmode);

/*
 * $HIBER / $WAKE, executive-resident + AST-interruptible (vms-feb).
 *
 * vms_kif_hiber blocks in the executive until a $WAKE is pending for this
 * process OR an AST becomes deliverable to it. It returns the executive's VMS
 * status (SS$_NORMAL on a normal release, or an error status when the ioctl
 * could not be delivered -- e.g. no /dev/vms -- so sys$hiber fails honestly
 * instead of busy-looping); *woken is set to 1 iff a $WAKE released the wait
 * (consuming the sticky wake bit) and 0 iff an AST did. A bare signal does NOT
 * end it (it re-enters past EINTR, like the blocking event-flag waits) -- only a
 * $WAKE or a deliverable AST does. sys$hiber loops it, draining and running the
 * deliverable ASTs after each successful return.
 *
 * vms_kif_wake sets the sticky wake bit on the target (vms_pid == 0 = the
 * caller) and wakes it if hibernating. Returns the executive's VMS status.
 */
uint32_t vms_kif_hiber(int *woken);
uint32_t vms_kif_wake(uint32_t vms_pid);

/* ================================================================
 * Event Flags (3c)
 *
 * THE WHOLE FAMILY IS WIRED (vms-2a8, vms-ef1): src/libvms/syssvc/sys_event.c
 * calls vms_kif_setef, clref, waitfr, wflor, wfland, readef, ascefc, dacefc,
 * and dlcefc -- every event-flag entry point in this header has a product
 * caller. See PR #22 (vms-2a8, e5cf411).
 * ================================================================ */

/* Set event flag. Returns SS$_WASSET or SS$_WASCLR */
uint32_t vms_kif_setef(uint32_t efn);

/* Clear event flag. Returns SS$_WASSET or SS$_WASCLR */
uint32_t vms_kif_clref(uint32_t efn);

/* Wait for single event flag */
uint32_t vms_kif_waitfr(uint32_t efn);

/* Wait for any flag in mask (OR wait) */
uint32_t vms_kif_wflor(uint32_t efn, uint32_t mask);

/* Wait for all flags in mask (AND wait) */
uint32_t vms_kif_wfland(uint32_t efn, uint32_t mask);

/* Read event flag cluster state */
uint32_t vms_kif_readef(uint32_t efn, uint32_t *state);

/* Associate with common event flag cluster */
uint32_t vms_kif_ascefc(uint32_t efn, const char *name, uint32_t prot, uint32_t perm);

/* Disassociate from common event flag cluster */
uint32_t vms_kif_dacefc(uint32_t efn);

/* Mark a PERMANENT common event flag cluster for deletion. Named, not
 * numbered: the caller need never have associated with it. SS$_UNASEFC if
 * the executive has no cluster of that name. */
uint32_t vms_kif_dlcefc(const char *name);

/* ================================================================
 * Lock Manager (3d)
 * ================================================================ */

/* Enqueue lock request. Returns lock ID in *lkid */
uint32_t vms_kif_enq(uint32_t efn, uint32_t lkmode, uint32_t flags,
                      const char *resnam, uint32_t parid,
                      uint64_t astadr, uint64_t astprm,
                      uint64_t blkastadr,
                      uint32_t *lkid, uint8_t *valblk);

/* Dequeue (release) lock */
uint32_t vms_kif_deq(uint32_t lkid, uint8_t *valblk, uint32_t flags);

/* Convert lock to new mode */
uint32_t vms_kif_convert(uint32_t lkid, uint32_t lkmode, uint32_t flags,
                          uint64_t blkastadr, uint8_t *valblk);

/* Get lock information
 * OVMX-UNWIRED: vms_kif_getlki (vms-a86) -- there is no sys$getlki in
 * src/libvms at all: lckdef.h and lkidef.h name the service and define its
 * LKI$_ item codes, but nothing implements it, so the kernel handler, the
 * ioctl and this wrapper serve a system service that does not exist. */
uint32_t vms_kif_getlki(uint32_t lkid, uint32_t *granted_mode,
                          uint32_t *requested_mode, char *resnam,
                          uint8_t *valblk);

/* vms_kif_getlki_parent -- vms_kif_getlki plus the lock's PARENT lkid (0 for
 * a root lock), the #GETLKI field ca673c87 wired the kernel to fill but the
 * original wrapper never surfaced. Same ioctl, no new op.
 * OVMX-UNWIRED: vms_kif_getlki_parent (vms-0dd) -- sys$getlki does not exist
 * in src/libvms (see vms_kif_getlki's own OVMX-UNWIRED note above); this
 * sibling wrapper has the same non-existent product caller, exercised only
 * by tests/qemu/test_syssvc_rms_reclock.c. */
uint32_t vms_kif_getlki_parent(uint32_t lkid, uint32_t *granted_mode,
                                uint32_t *requested_mode, char *resnam,
                                uint8_t *valblk, uint32_t *parent_id);

/* Read a resource's DLM directory + mastering state (vms-ci.5 DB).
 * OVMX-UNWIRED: vms_kif_get_resmaster (vms-ci.5) -- a READ-ONLY DLM
 * resource-master diagnostic with no product path: no sys$ service issues it.
 * It exists so the kernel's LOCAL resource-directory + mastering scaffolding
 * (directory hashes to self, resource mastered on first $ENQ) is observable
 * against a real /dev/vms -- exercised only by tests/qemu/test_kmod_resdir.c,
 * the same footing as vms_kif_getlki above. When 0.4 gives the DLM a product
 * reader (SHOW CLUSTER / a lock-master query), wire it and delete this line. */
uint32_t vms_kif_get_resmaster(const char *resnam, uint32_t *found,
                               uint32_t *local_csid, uint32_t *dir_csid,
                               uint32_t *master_csid, uint32_t *is_local_master,
                               uint32_t *n_granted,
                               uint32_t *remote_holder_csid);

/* Dispatch a decoded cross-node DLM request to the kernel lock manager's
 * cross-node handler (vms-94c, DLM epic vms-7fa rung 1).
 * OVMX-UNWIRED: vms_kif_dlm_xnode (vms-94c) -- the DLM message TRANSPORT reaches
 * the executive here, but the daemon that carries a peer's DLM message (scsd) is
 * a glibc process and reaches /dev/vms with a direct POSIX ioctl, not this
 * freestanding client; and no sys$ service issues it. This wrapper exists so the
 * cross-node RECEIVE handler (vms_lock_dlm_xnode_dispatch, which returns
 * SS$_UNSUPPORTED in rung 1) is observable against a real /dev/vms -- exercised
 * only by tests/qemu/test_syssvc_dlm_xnode.c, the same footing as
 * vms_kif_get_resmaster above. Rung 2 (vms-7fa) gives it a product path (or
 * moves the dispatch behind a shared client); wire it and delete this line then. */
uint32_t vms_kif_dlm_xnode(uint32_t op, uint32_t lkmode, uint32_t flags,
                           uint32_t req_lkid, uint32_t master_lkid,
                           uint32_t req_csid, uint32_t master_csid,
                           const char *resnam, const uint8_t *valblk,
                           uint32_t *out_master_lkid, uint32_t *out_queued,
                           uint32_t *out_blocking_csid,
                           uint32_t *out_blocking_master_lkid,
                           uint32_t *out_req_lkid, uint32_t *out_lkmode);

/* Report a graceful cluster departure to the executive: shrink the LIVE DLM
 * directory membership + re-resolve the directory over the survivors (vms-2bf,
 * DLM rung H10a).
 * OVMX-UNWIRED: vms_kif_dlm_member_depart (vms-2bf) -- the departure INGRESS
 * reaches the executive here, but the daemon that observes a peer's graceful
 * class-0x04 self-departure (scsd) is a glibc process and issues
 * VMS_IOCTL_DLM_MEMBER_DEPART with a DIRECT POSIX ioctl, not this freestanding
 * client; and no sys$ service issues it. Same footing as vms_kif_dlm_xnode
 * above: this wrapper exists so the ioctl is observable against a real /dev/vms
 * (exercised by the 3-node H10 harness via scsd's direct ioctl). Wire it and
 * delete this line if a product client ever issues it. */
uint32_t vms_kif_dlm_member_depart(uint32_t departed_csid,
                                   uint32_t *out_members_live,
                                   uint32_t *out_found);

/* Read the first REMOTE-held granted lock on a resource -- holder CSID, its own
 * handle, and granted mode -- to value-verify a rebuilt cross-node lock (vms-dca9,
 * DLM rung H10b).
 * OVMX-UNWIRED: vms_kif_dlm_get_granted (vms-dca9) -- like vms_kif_dlm_member_depart
 * above, scsd issues VMS_IOCTL_DLM_GET_GRANTED with a DIRECT POSIX ioctl (it is a
 * glibc process), not this freestanding client; no sys$ service issues it. This
 * wrapper exists so the readback is observable against a real /dev/vms (exercised
 * by the 3-node H10b harness via scsd's direct ioctl). Wire it and delete this
 * line if a product client ever issues it. */
uint32_t vms_kif_dlm_get_granted(const char *resnam, uint32_t *out_found,
                                 uint32_t *out_n_granted, uint32_t *out_holder_csid,
                                 uint32_t *out_holder_req_lkid,
                                 uint32_t *out_granted_mode);

/*
 * vms_kif_dlm_enum_waits - enumerate this node's pending (NL) cross-node waits, the
 * HOME authority for distributed deadlock search (vms-ec75, DLM rung H11).
 * OVMX-UNWIRED: vms_kif_dlm_enum_waits (vms-ec75) -- like vms_kif_dlm_get_granted
 * above, scsd issues VMS_IOCTL_DLM_ENUM_WAITS with a DIRECT POSIX ioctl in its chase
 * orchestration, not this freestanding client; no sys$ service issues it. This
 * wrapper exists so the readback is observable against a real /dev/vms (exercised by
 * the DLM H11 harness via scsd's direct ioctl). Wire it and delete this line if a
 * product client ever issues it. */
uint32_t vms_kif_dlm_enum_waits(uint32_t *out_count, uint32_t *out_total);

/* Cluster membership crosses into the executive (rd vms-551,
 * docs/design-cluster-membership-executive.md). SET/CLEAR insert-or-update /
 * remove one member by csid; GET copies out the live view.
 * OVMX-UNWIRED: vms_kif_cluster_member_set (vms-551) -- like
 * vms_kif_dlm_member_depart above, scsd (a glibc process) issues
 * VMS_IOCTL_CLUSTER_MEMBER_SET with a DIRECT POSIX ioctl from
 * scsd_publish_membership, not this freestanding client; no sys$ service
 * issues it. This wrapper exists so SET is observable against a real
 * /dev/vms from the freestanding syssvc test. Wire it and delete this line
 * if a product client ever issues it. */
uint32_t vms_kif_cluster_member_set(uint32_t csid, uint32_t sysid,
                                    const char *scsnode, const char *state);

/* OVMX-UNWIRED: vms_kif_cluster_member_clear (vms-551) -- same footing as
 * vms_kif_cluster_member_set above: scsd issues VMS_IOCTL_CLUSTER_MEMBER_CLEAR
 * with a direct POSIX ioctl on departure, not this freestanding client. This
 * wrapper exists so CLEAR is observable against a real /dev/vms from the
 * freestanding syssvc test. Wire it and delete this line if a product
 * client ever issues it. */
uint32_t vms_kif_cluster_member_clear(uint32_t csid);

/*
 * vms_kif_cluster_get_members - SHOW CLUSTER's read of the executive
 * membership block (vms-551). WIRED: DCL's cmd_show_cluster
 * (src/vmsdcl/dcl_cmd_show.c) calls this directly, so it satisfies the kif
 * caller census as a real product path. Fails honestly with SS$_NOSUCHDEV
 * when /dev/vms is unreachable (never conflated with a genuine NOTMEMBER
 * view, which is SS$_NORMAL with *out_n_members==0).
 */
uint32_t vms_kif_cluster_get_members(struct vms_cluster_member *out_members,
                                     uint32_t max_members,
                                     uint32_t *out_n_members);

/*
 * vms_kif_dlm_xnode_blkast - the BLKAST-WIRE half of the cross-node DLM receive
 * (DLM epic vms-7fa rung H6, vms-76d). A focused wrapper that carries the two
 * fields the generic vms_kif_dlm_xnode above does not: `blkastadr`/`blkastprm`
 * (in -- a GRANT receive that establishes a HOLDER origin record registers the
 * holder's blocking-AST routine) and `out_blkast_delivered` (a BLKAST receive
 * reports 1 iff a genuine user-mode blocking AST was queued to the holder proc).
 * Also returns blocking_req_lkid for an ENQ. Exercised by
 * tests/qemu/test_syssvc_dlm_xnode.c against a real /dev/vms; the product carrier
 * (scsd) builds the ioctl struct directly, as it does for the other DLM ops.
 *
 * OVMX-UNWIRED: vms_kif_dlm_xnode_blkast (vms-76d) -- like vms_kif_dlm_xnode, the
 * BLKAST-wire carrier is scsd, a glibc process that reaches /dev/vms with direct
 * POSIX ioctls (scsd_dlm_holder_establish / scsd_dlm_blkast_fire), not this
 * freestanding client; no sys$ service issues it. This wrapper exists only so the
 * holder-side BLKAST receive is observable against a real /dev/vms from the
 * freestanding syssvc test. */
uint32_t vms_kif_dlm_xnode_blkast(uint32_t op, uint32_t lkmode,
                                  uint32_t req_lkid, uint32_t master_lkid,
                                  uint32_t req_csid, uint32_t master_csid,
                                  const char *resnam,
                                  uint64_t blkastadr, uint64_t blkastprm,
                                  uint32_t *out_blocking_req_lkid,
                                  uint32_t *out_blkast_delivered);

/* ================================================================
 * Device table (executive-resident I/O database)
 *
 * The executive owns the devices; a process owns only its channels to
 * them. A device attribute read here is the attribute every process
 * on the node sees, and a characteristic set here is seen by every
 * process on the node -- which is the whole difference between a VMS
 * device and a private notion of one.
 *
 * THE FAMILY WAS ONCE WHOLLY UNWIRED, and the paragraph that said so is
 * replaced rather than deleted, because what remains unwired is the point.
 * The kernel device table, these wrappers and their QEMU suite were merged
 * with no product reader at all: SHOW DEVICE printed the host Linux mount
 * table, and src/vmsdcl/dcl_cmd_show.c carried a COMMENT saying the
 * conversion was future work. A comment is not a caller -- that is exactly
 * what this census exists to say out loud. vms-fb9 wired the two readers
 * ($GETDVI by name, $DEVICE_SCAN) and vms-d0b wires $ASSIGN, so what is
 * still declared below is what OVMX still does not do.
 * ================================================================ */

/* $ASSIGN a channel to a device by name. SS$_NOSUCHDEV if the
 * executive has no such device; SS$_IVDEVNAM if the name is not a
 * device name at all.
 * Wired: the census gate is what proves it has a product caller.
 * (src/ovmx_init/ovmx_init.c takes the login session's channel to the
 * console with it, which is what the terminal binding is built on.) */
uint32_t vms_kif_assign(const char *devnam, uint32_t *chan);

/* $DASSGN the channel. SS$_IVCHAN if it is not one of ours.
 * Wired: the census gate is what proves it has a product caller. */
uint32_t vms_kif_dassgn(uint32_t chan);

/* $ALLOC the device to this process -- this, and not $ASSIGN, is what
 * makes a process the device's owner. SS$_DEVALLOC when it is already
 * allocated to another process or another process holds channels to
 * it; SS$_NOSUCHDEV when there is no such device.
 * Wired (vms-651): cmd_mount (src/vmsdcl/dcl_cmd_misc.c) claims the unit in
 * the executive's device table before mount(2)ing its backing block device,
 * so a second process cannot MOUNT (or ALLOCATE, once vms-dv1 lands) the
 * same unit out from under it. ALLOCATE/DEALLOCATE as their own DCL verbs
 * remain vms-dv1's to wire; this is a second, independent caller. */
uint32_t vms_kif_alloc(const char *devnam);

/* $DALLOC the device. SS$_DEVNOTALLOC if this process does not have it
 * allocated.
 * Wired (vms-651): cmd_dismount releases the claim vms_kif_alloc() took,
 * after umount(2) succeeds. */
uint32_t vms_kif_dalloc(const char *devnam);

/* vms_kif_alloc_op() is the static body those two share inside vms_kif.c.
 * It has no prototype here and is not part of the interface, but the census
 * universe is the union of what this header prototypes and what vms_kif.c
 * defines -- static definitions included, so that marking a definition static
 * cannot drop it out of the census. It is reached from $ALLOC/$DALLOC, both
 * wired (vms-651), so it is wired too. */

/* Read a device row by name. SS$_NOSUCHDEV if there is no such device.
 * Wired: the census gate is what proves it has a product caller. */
uint32_t vms_kif_getdvi_devnam(const char *devnam, struct vms_devinfo *info);

/* Read the device row behind an assigned channel. SS$_IVCHAN if the
 * channel is not ours.
 * Wired: the census gate is what proves it has a product caller. */
uint32_t vms_kif_getdvi_chan(uint32_t chan, struct vms_devinfo *info);

/* Enumerate the device table. Pass *index = 0 for the first row; each
 * call fills info and advances *index. Returns SS$_NOMOREDEV when the
 * scan is exhausted.
 * Wired: the census gate is what proves it has a product caller. */
uint32_t vms_kif_devscan(uint32_t *index, struct vms_devinfo *info);

/* Resolve a DISK unit name (DKA0:, DKA100:, ...) to the Linux block device
 * the executive enumerated it from at module init. On success (odd status)
 * `backing` receives the vda/vdb/... name (up to backing_size-1 chars, always
 * NUL-terminated) and the major/minor pair -- both optional -- the backing dev_t.
 * SS$_NOSUCHDEV if there is no such unit; SS$_IVDEVNAM if the name is
 * malformed or names a device that is not a disk. Enumeration of the units
 * themselves is the existing vms_kif_devscan() (disk units appear in the
 * table as DC$_DISK rows); this is the companion that hands back the backing
 * device the process must open. The executive owns the fact -- the process
 * never scans /sys/block itself (Rule 11).
 * Wired (vms-651): cmd_mount resolves the unit to its backing block device
 * this way, then mount(2)s "/dev/<backing>" as vmsfs at the unit's mount
 * point. */
uint32_t vms_kif_disk_resolve(const char *devnam, char *backing,
                              uint32_t backing_size,
                              uint32_t *major, uint32_t *minor);

/* $GETDVI for the VOLUME items of a disk unit (vms-e6f): mount state, ODS-2
 * volume label, size, cluster factor and free-block count of a mounted volume,
 * read from the executive-global mounted-volume table the ACP populates at
 * $MOUNT. On return `out` (optional) holds the vms_getvol_args: out->mounted is
 * 1 for a mounted ODS-2 volume (then volnam/volsize/cluster are set, and
 * freeblocks iff out->free_valid), 0 for a device that is not a mounted volume.
 * SHOW DEVICE reads this to report a disk as Mounted with its label + free/total
 * blocks; the values are the executive's, never a per-process fake (Rule 11). */
uint32_t vms_kif_getvol(const char *devnam, struct vms_getvol_args *out);

/* Set terminal characteristics through an assigned channel (the
 * $QIO IO$_SETMODE path). flags is a mask of VMS_TTSET_*; SS$_IVCHAN
 * if the caller holds no such channel.
 * OVMX-UNWIRED: vms_kif_ttsetmode (vms-a36) -- SET TERMINAL is the writer, and
 * what it writes is ctx->terminal, a per-process DCL-local model no other
 * process (and no later image in this one) can observe. Re-pointed from
 * vms-fb9, which is CLOSED: an item that is done tracks nothing, and the whole
 * price of this declaration is that a LIVE item owns the gap. */
uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,
                           uint64_t setchar, uint64_t clrchar,
                           uint32_t width, uint32_t page);

/* Record this process's TERMINAL in the executive's process table, from
 * a channel it already holds to a terminal device (vms-d0b). The
 * executive copies the name off that channel's device; the caller
 * supplies no name. SS$_IVCHAN if the channel is not ours, SS$_IVDEVNAM
 * if it is not to a terminal.
 *
 * This is the writer behind $GETJPI's terminal and SHOW TERMINAL's
 * "Terminal: _OPA0:" header. It is made where the session's channel to
 * the console is -- src/ovmx_init/ovmx_init.c's login child, before the
 * image is activated -- so the terminal a job is on is a fact other
 * processes can read, not a claim the job makes about itself.
 * Wired: the census gate is what proves it has a product caller. */
uint32_t vms_kif_setterm(uint32_t chan);

/* ================================================================
 * Process table (executive-resident PCB directory)
 *
 * The executive owns the process name, so these are readers and a
 * single writer over shared state -- not accessors for anything this
 * process keeps to itself. A name set here is visible to every other
 * process and survives execve().
 * ================================================================ */

/* Set this process's name ($SETPRN). SS$_DUPLNAM if the name is
 * already in use within this process's UIC group. */
uint32_t vms_kif_setprn(const char *prcnam);

/* Read this process's row from the executive process table. */
uint32_t vms_kif_getjpi_self(struct vms_procinfo *info);

/* Read the executive's system-wide physical memory figures (SHOW MEMORY's
 * "Physical Memory Usage" section). info->fields_valid & VMS_SYIMEM_V_PHYS is
 * set only when total_bytes/free_bytes carry real executive data.
 *
 * OVMX-UNWIRED: vms_kif_getsyi_memory (vms-a3cd) -- wired ONLY on the
 * NetBSD/VAX SHOW MEMORY path (cmd_show_memory's #if defined(__NetBSD__)
 * branch, src/vmsdcl/dcl_cmd_show.c), where /proc/meminfo is absent. On the
 * census arch (x86_64) that branch is preprocessed out -- SHOW MEMORY there
 * reads /proc/meminfo directly -- so the product emits no call to this entry
 * point on x86_64. It is genuinely wired on VAX (proven by the VAX rail
 * acceptance battery), just not on the arch the census compiles. Same footing
 * as vms_kif_dlm_xnode. */
uint32_t vms_kif_getsyi_memory(struct vms_syi_meminfo *info);

/* Resolve a process by VMS PID. SS$_NONEXPR if no such process. */
uint32_t vms_kif_getjpi_pid(uint32_t vms_pid, struct vms_procinfo *info);

/* Resolve a process by name within this process's UIC group.
 * SS$_NONEXPR if no process in the group holds that name. */
uint32_t vms_kif_getjpi_prcnam(const char *prcnam, struct vms_procinfo *info);

/* Enumerate the process table. Pass *index = 0 for the first row; each
 * call fills info and advances *index. Returns SS$_NONEXPR when the
 * scan is exhausted. */
uint32_t vms_kif_procscan(uint32_t *index, struct vms_procinfo *info);

/* ================================================================
 * $EXIT / $STATUS + CLI invocation context (vms-f60d)
 *
 * The executive owns image-completion status and the invoking CLI's command
 * line. The exiting image records its status (vms_kif_setexit) and the invoking
 * CLI reads it (vms_kif_getexit); the CLI records its command line
 * (vms_kif_setcli) and the activated image reads it (vms_kif_getcli).
 * INV-6: no /dev/vms -> SS$_NOSUCHDEV, no per-process fake.
 * ================================================================ */

/* Record `condition` as this process's image-completion $STATUS. *exit_code
 * (optional) receives the OVMX POSIX exit code mapped from it. */
uint32_t vms_kif_setexit(uint32_t condition, uint32_t *exit_code);

/* Read this process's recorded image-completion $STATUS. *has_exited (optional)
 * is nonzero iff an image has actually recorded one (do not infer it from a
 * zero *condition, which is a legal value). */
uint32_t vms_kif_getexit(uint32_t *condition, int *has_exited);

/* Read the image-completion $STATUS of a process named by its backing Linux pid
 * (vms-707). DCL's RUN uses this to recover the true condition value of an image
 * it activated through the fork()+execve() fallback -- that child shares DCL's
 * VMS PID, so only the child's Linux pid names its PCB unambiguously. Must be
 * called BEFORE the child is reaped. *has_exited (optional) is nonzero iff an
 * image actually recorded a status (a foreign tool that never $EXITs leaves it
 * 0). */
uint32_t vms_kif_getexit_linux(uint32_t linux_pid, uint32_t *condition,
                               int *has_exited);

/* Record this CLI process's invoking command line + cliflag, for an activated
 * image to inherit and read back. cliflag == 0 means "no CLI". */
uint32_t vms_kif_setcli(uint32_t cliflag, const char *command);

/* Read this process's own invoking CLI context. *cliflag (optional) is nonzero
 * iff a CLI launched it; command/command_size receive the NUL-terminated line;
 * *length (optional) its true byte length. */
uint32_t vms_kif_getcli(uint32_t *cliflag, char *command,
                        uint32_t command_size, uint32_t *length);

/* Arm a /NOWAIT subprocess-exit completion (vms-e9a B1, LIB$SPAWN efn/astadr).
 * The caller is the parent of a subprocess it created /NOWAIT; the executive
 * sets event flag `efn` (VMS_EF_NONE = none) and/or queues completion AST
 * `astadr`/`astprm` (astadr 0 = none) in this process when the child (named by
 * VMS PID) records its exit. *completed (optional) is nonzero iff the child had
 * already exited and the notification fired immediately. INV-6: no /dev/vms ->
 * SS$_NOSUCHDEV, nothing armed. */
uint32_t vms_kif_spawn_notify(uint32_t child_vms_pid, uint32_t efn,
                              uint64_t astadr, uint64_t astprm, int *completed);

/* ================================================================
 * P0 program region (vms-68f.i, in-process image activation foundation)
 *
 * Records/clears this process's P0 program-region extent with the
 * executive, so it is reflected in $GETJPI (struct vms_procinfo's
 * p0_base/p0_limit above) -- docs/design-in-process-activation.md Part II
 * §A.2.1. Neither call maps or unmaps any memory itself: the P0 window
 * reservation and the per-image mmaps into it are DCL's and
 * imgact$activate's job in userspace: this pair only RECORDS the extent
 * for executive bookkeeping and cross-process/$GETJPI visibility.
 * INV-6: no /dev/vms -> SS$_NOSUCHDEV, no per-process fake.
 * ================================================================ */

/* Register [base, limit) as this process's current P0 extent. SS$_BADPARAM
 * for a degenerate or out-of-address-space range; overwrites any
 * previously-registered extent (see vms_p0.c for why that is not an error).
 * Wired (vms-6f1, increment iv): imgact_activate() (src/libvms/syssvc/
 * sys_imgact.c) calls this once it has mapped an in-process image's PT_LOAD
 * segments into the P0 window, and dcl_activate_image() dispatches RUN through
 * it -- the census gate is what proves it has a product caller. */
uint32_t vms_kif_p0_map(uint64_t base, uint64_t limit);

/* Clear this process's P0 extent (image rundown). Always succeeds
 * (clearing an already-clear extent is a no-op). *old_base and *old_limit --
 * both optional -- receive the extent that WAS registered, zero if none
 * was, so a caller can observe what was just freed without a separate
 * $GETJPI round trip.
 * Wired (vms-6f1, increment iv): imgact_activate()'s rundown path
 * (src/libvms/syssvc/sys_imgact.c) calls this to mark the process image-less
 * after the in-process image returns and its P0 window is collapsed -- the
 * census gate is what proves it has a product caller. (Full SYS$RUNDWN
 * resource release -- channels, P0 locks, image ASTs -- is increment v.) */
uint32_t vms_kif_p0_unmap(uint64_t *old_base, uint64_t *old_limit);

/* ================================================================
 * P1 control region (vms-68f.ii, in-process image activation foundation,
 * increment (ii))
 *
 * Records this process's P1 (process-permanent control-region) extent
 * with the executive, so it is reflected in $GETJPI (struct
 * vms_procinfo's p1_base/p1_limit above) -- docs/design-in-process-
 * activation.md Part II §A.1.1, §A.2.1. Like vms_kif_p0_map, this does
 * not map or unmap any memory itself: laying DCL's actual process-
 * permanent state into a P1 window is DCL/imgact$activate's job in
 * userspace. Unlike P0, there is no unmap counterpart -- P1 lasts for
 * the process's lifetime; see vms_p1.c for why.
 * INV-6: no /dev/vms -> SS$_NOSUCHDEV, no per-process fake.
 * ================================================================ */

/* Register [base, limit) as this process's P1 extent. SS$_BADPARAM for a
 * degenerate or out-of-address-space range; overwrites any previously-
 * registered extent (see vms_p1.c for why that is not an error).
 * Wired (vms-68f.v): dcl_p1_init() (src/vmsdcl/dcl_cmd_process.c) lays a real
 * P1 control block at DCL startup and registers its extent here, so $GETJPI
 * reports this process's P1 region and dcl_activate_image() protects its
 * critical page while an in-process image runs -- the census gate is what
 * proves it has a product caller. */
uint32_t vms_kif_p1_map(uint64_t base, uint64_t limit);

/* ================================================================
 * Access-mode transition primitive (vms-68f.iii, in-process image
 * activation foundation, increment (iii))
 *
 * The CHMx/REI-equivalent pair: a controlled descent into an activated
 * image (Supervisor -> User) and the paired, executive-verified return at
 * image rundown (User -> Supervisor). docs/design-in-process-
 * activation.md Part II §A.1.3, §A.2.3. See vms_ioctl.h's
 * VMS_IOCTL_ENTER_IMAGE/VMS_IOCTL_IMAGE_RUNDOWN comment for why this is a
 * distinct pair from vms_kif_setmode() rather than two calls to it.
 * INV-6: no /dev/vms -> SS$_NOSUCHDEV, no per-process fake.
 * ================================================================ */

/* Enter an image: refused SS$_NOPRIV unless the caller is currently in
 * PSL_C_SUPER. On success, *prev_mode / *new_mode (both optional) report the
 * transition (PSL_C_SUPER -> PSL_C_USER) and the executive records the
 * mode to restore.
 * Wired (vms-6f1, increment iv): imgact_activate() (src/libvms/syssvc/
 * sys_imgact.c) calls this to descend into an in-process image's code before
 * entering its entry point, and dcl_activate_image() dispatches RUN through
 * it -- the census gate is what proves it has a product caller. */
uint32_t vms_kif_enter_image(uint8_t *prev_mode, uint8_t *new_mode);

/* Run an image down: refused SS$_NOPRIV unless a matching vms_kif_
 * enter_image() is still open for this process. On success, *prev_mode/
 * *new_mode (both optional) report the transition (PSL_C_USER -> whatever
 * the matching ENTER_IMAGE recorded) and the open descent is closed.
 * Wired (vms-6f1, increment iv): imgact_activate() (src/libvms/syssvc/
 * sys_imgact.c) calls this when the in-process image returns, restoring
 * Supervisor before control returns to DCL -- the census gate is what proves
 * it has a product caller. (Full SYS$RUNDWN resource release is increment
 * v.) */
uint32_t vms_kif_image_rundown(uint8_t *prev_mode, uint8_t *new_mode);

/*
 * vms_kif_p1_protect - the CRITICAL-P1 MPROTECT MECHANISM (vms-68f.iii,
 * docs/design-in-process-activation.md Part II §A.2.3(b)).
 *
 * NOT an ioctl -- there is no VMS_IOCTL_* behind this call. [base, limit)
 * is a real page-aligned range in THIS PROCESS's own address space (the
 * design's "DCL's crown-jewel P1 structures", typically a sub-range
 * registered with vms_kif_p1_map()); this wraps a real mprotect(2)
 * (vms_sys_mprotect(), src/libvmssys/vms_syscall.h) to PROT_READ (writable
 * == 0) while an image runs in User mode, or back to PROT_READ|PROT_WRITE
 * (writable != 0) at rundown. This IS the enforced half of §A.2.3(b): a
 * wild write from an in-process image to a protected P1 page faults at
 * the MMU, genuinely, regardless of the software current-mode tracking
 * above -- it does not depend on /dev/vms being reachable at all, so
 * there is no INV-6 fallback question for this call the way there is for
 * an ioctl-backed one.
 * Returns SS$_BADPARAM for a degenerate range, SS$_ACCVIO if the
 * underlying mprotect(2) itself fails (e.g. the range is not currently
 * mapped), SS$_NORMAL on success.
 * Wired (vms-6f1, increment iv): imgact_activate() (src/libvms/syssvc/
 * sys_imgact.c) calls this to mprotect a caller-supplied critical-P1 range
 * read-only before entering an in-process image in User mode, and read/write
 * again at rundown -- the census gate is what proves it has a product caller. */
uint32_t vms_kif_p1_protect(uint64_t base, uint64_t limit, int writable);

/* ================================================================
 * Logical name tables (executive-resident LNM$SYSTEM, vms-d37)
 *
 * The executive owns the LNM$SYSTEM storage, so a name defined here by
 * one process is visible to every other process on the node. DEFINE and
 * DELETE are mutations and go through an ioctl; TRANSLATE reads a
 * read-only mmap of the arena and issues NO syscall on the hot path
 * (design "C-corrected", docs/design-logical-name-placement.md).
 *
 * NO PER-PROCESS FALLBACK (CLAUDE.md Rule 9 / INV-6). If /dev/vms is
 * absent the mapping cannot be obtained and the arena is NULL: define and
 * delete return SS$_NOSUCHDEV, and translate reports "unavailable" (its
 * caller then returns SS$_NOSUCHDEV). There is no non-executive table to
 * fall back to -- that absence is the whole point.
 * ================================================================ */

/* Create or supersede a name in an executive-resident table.
 * `table` is VMS_LNM_TBL_SYSTEM, _GROUP or _JOB (vms-aba). `values` carries
 * `num_values` equivalence strings (1..VMS_LNM_MAX_EQUIV). Returns the
 * executive's SS$_ status, or SS$_NOSUCHDEV if the executive is absent.
 * Wired: sys$crelnm (src/libvms/syssvc/sys_logical.c). */
uint32_t vms_kif_lnm_define(uint32_t table, const char *name,
                            const char *const *values, uint8_t num_values,
                            uint32_t attributes, uint8_t acmode);

/* Delete a name from an executive-resident table. SS$_NOLOGNAM if the
 * name is not present, SS$_NOSUCHDEV if the executive is absent.
 * Wired: sys$dellnm (src/libvms/syssvc/sys_logical.c). */
uint32_t vms_kif_lnm_delete(uint32_t table, const char *name, uint8_t acmode);

/* Translate a name by reading the mmap'd arena (no syscall on a warm
 * mapping). `index` selects WHICH equivalence string of a (possibly
 * multi-valued, i.e. search-list) logical name to return -- 0 is the
 * first, mirroring VMS $TRNLNM's index argument (a caller enumerates a
 * search list by calling again with index+1 until this returns 0).
 * Returns 1 and fills value/vallen/attrs when `index` names a value that
 * exists, 0 when the name is absent OR `index` is beyond the entry's last
 * equivalence string (vms-420: previously this always read index 0 only,
 * which is why DEFINE FOO BAR,BAZ + SHOW LOGICAL FOO silently dropped
 * BAZ), or -1 when the executive/mapping is unavailable (the caller
 * renders that as SS$_NOSUCHDEV). If `num_equiv` is non-NULL and the name
 * IS present in the table (regardless of whether `index` itself is in
 * range), it receives the entry's total equivalence count, so a caller
 * enumerating a search list knows when it has reached the end; it is set
 * to 0 when the name is not found at all.
 * Wired: sys$trnlnm (src/libvms/syssvc/sys_logical.c). */
int vms_kif_lnm_translate(uint32_t table, const char *name, uint8_t index,
                          char *value, uint32_t valsz,
                          uint16_t *vallen, uint32_t *attrs,
                          uint8_t *num_equiv);

/* One record produced by vms_kif_lnm_enumerate: a logical name and ALL of
 * its equivalence strings (vms-420 -- previously only index 0 was carried,
 * which is why an enumerated search-list logical showed just its first
 * value), NUL-terminated. Sizes track the arena's VMS_LNM_MAX_NAME/VALUE/
 * EQUIV (asserted in vms_kif.c). */
struct vms_kif_lnm_enum_rec {
    char     name[VMS_LNM_MAX_NAME + 1];
    uint8_t  num_values;                          /* 1..VMS_LNM_MAX_EQUIV */
    char     values[VMS_LNM_MAX_EQUIV][VMS_LNM_MAX_VALUE + 1];
    uint32_t attributes;
};

/* Enumerate every name in an executive-resident table (VMS_LNM_TBL_SYSTEM,
 * _GROUP or _JOB), filtered to THIS caller's own scope exactly as
 * vms_kif_lnm_translate() filters -- SYSTEM is node-wide (scope 0),
 * GROUP/JOB are the caller's executive-derived scope keys (never recomputed
 * locally). Fills up to `max_out` records into `out` and returns the number
 * filled (>=0), or -1 when the executive/mapping is unavailable (the caller
 * renders that as SS$_NOSUCHDEV). The arena holds at most VMS_LNM_MAX_ENTRIES
 * names, so a caller sizing `out` to that never truncates; a smaller buffer
 * caps the return at `max_out`. Reads the read-only mmap arena under the SAME
 * seqlock as vms_kif_lnm_translate -- no syscall on a warm mapping, and a
 * torn snapshot is retried, never returned.
 * Wired: lnm_enumerate (src/vmslnm/lnm_client.c), the path DCL SHOW LOGICAL
 * walks so a DEFINE/SYSTEM by one process is listed by another. */
int vms_kif_lnm_enumerate(uint32_t table,
                          struct vms_kif_lnm_enum_rec *out, uint32_t max_out);

/* ================================================================
 * Mailboxes (executive-resident MBAn:, vms-d44)
 *
 * The executive owns the mailbox table (src/kernel/vms_mbx.c): the unit
 * number, the message queue and the buffer-quota accounting. A mailbox
 * created by one process is reachable by ANY other process that knows its
 * device name (vms_kif_mbx_assign) or, for a named mailbox, the LNM$SYSTEM
 * logical name sys$crembx defines for it (src/libvms/syssvc/sys_mailbox.c)
 * -- unlike the AF_UNIX-socketpair implementation this replaces, where a
 * second process could never reach the first's mailbox at all.
 *
 * NO PER-PROCESS FALLBACK (CLAUDE.md Rule 9 / INV-6): every entry point
 * below returns SS$_NOSUCHDEV, never a private substitute, when /dev/vms
 * is unreachable -- checked explicitly in vms_kif.c (mbx_bind_ok()) rather
 * than inferred from the ioctl's own -EBADF, exactly as
 * vms_kif_lnm_define()/_delete() probe the LNM arena mapping before
 * mutating.
 * ================================================================ */

/* $CREMBX: create a mailbox and hand back a channel to it (an EXECUTIVE
 * channel number -- see src/libvms/syssvc/sys_mailbox.c for how it is
 * stored in the caller's PCB). `permanent` gates on PRMMBX privilege, a
 * temporary mailbox on TMPMBX (both enforced by the executive, not here).
 * devnam receives "MBAn:", NUL-terminated within devnam_sz. */
uint32_t vms_kif_mbx_create(int permanent, uint32_t maxmsg, uint32_t bufquo,
                            uint32_t *exec_chan, uint32_t *unit,
                            char *devnam, uint32_t devnam_sz);

/* $ASSIGN to an EXISTING mailbox by device name ("MBAn:"), the path an
 * unrelated process uses to reach a mailbox it did not create. SS$_NOSUCHDEV
 * if no such mailbox exists. */
uint32_t vms_kif_mbx_assign(const char *devnam, uint32_t *exec_chan);

/* $DELMBX: mark the mailbox behind `exec_chan` for deletion. Does not
 * deassign `exec_chan` itself -- see vms_mbx.c's header. */
uint32_t vms_kif_mbx_delmbx(uint32_t exec_chan);

/* $QIO IO$_WRITEVBLK-equivalent: one message, moved whole. SS$_EXQUOTA if
 * `len` exceeds the mailbox's MAXMSG or its remaining BUFQUO. */
uint32_t vms_kif_mbx_write(uint32_t exec_chan, const void *buf, uint32_t len);

/* $QIO IO$_READVBLK-equivalent. Copies up to `bufsz` bytes of the next
 * message into `buf` and reports the message's true length in *actlen (which
 * may exceed bufsz if the caller's buffer was smaller than the message -- the
 * excess is discarded, as $QIO truncates an oversized mailbox message into an
 * undersized buffer).
 *
 * nowait == 0: blocks until a message is queued (the default mailbox read).
 * nowait != 0: $QIO IO$M_NOW -- completes immediately; if the mailbox is empty
 *   returns SS$_ENDOFFILE without blocking (VSI OpenVMS I/O User's Reference
 *   Manual, Mailbox Driver). */
uint32_t vms_kif_mbx_read(uint32_t exec_chan, void *buf, uint32_t bufsz,
                          uint32_t *actlen, int nowait);

/* $QIO IO$_SETMODE|IO$M_WRTATTN-equivalent: arm a WRITE-ATTENTION AST on the
 * mailbox behind `exec_chan`. When ANOTHER process writes to the mailbox, the
 * executive queues astadr(astprm) into this process's AST queue at `acmode`
 * (drained through $SETAST/DELIVERAST, like a $DCLAST-declared AST). One-shot,
 * re-armed by another call. SS$_NOSUCHDEV if the executive is unreachable. */
uint32_t vms_kif_mbx_set_wrtattn(uint32_t exec_chan, uint8_t acmode,
                                 uint64_t astadr, uint64_t astprm);

/* ================================================================
 * INET pseudo-device BGn: (vms-527)
 *
 * Marshalling wrappers over the VMS_IOCTL_BG_* driver in vms.ko
 * (src/kernel/vms_bg.c). Every one binds to /dev/vms and returns
 * SS$_NOSUCHDEV if the executive is absent -- the honest state for an
 * executive device whose driver is unreachable (INV-6), never a userspace
 * socket fallback. The socket is executive-resident; these move only the
 * request/response bytes.
 * ================================================================ */

/* $ASSIGN TCPIP$DEVICE: -- allocate a fresh BGn: unit + a channel to it. */
uint32_t vms_kif_bg_create(uint32_t *exec_chan, uint32_t *unit,
                           char *devnam, uint32_t devnam_sz);
/* IO$_SETMODE -- create the channel's host socket (AF_INET/SOCK_STREAM). */
uint32_t vms_kif_bg_setmode(uint32_t exec_chan);
/* IO$_SETMODE (raw-ICMP subfunction) -- create the channel's host socket as a
 * raw ICMP socket for PING (vms-80b), then drive it with the ordinary
 * connect/send/recv. Fails SS$_NOSUCHDEV with no executive. */
uint32_t vms_kif_bg_setmode_icmp(uint32_t exec_chan);
/* IO$_ACCESS -- connect to a peer (sockaddr_in fields, network byte order). */
uint32_t vms_kif_bg_connect(uint32_t exec_chan, uint16_t family,
                            uint16_t port, uint32_t addr);
/* IO$_WRITEVBLK -- send one buffer; *actlen gets the bytes sent. */
uint32_t vms_kif_bg_send(uint32_t exec_chan, const void *buf, uint32_t len,
                         uint32_t *actlen);
/* IO$_READVBLK -- receive into buf (<= bufsz); *actlen gets the bytes read. */
uint32_t vms_kif_bg_recv(uint32_t exec_chan, void *buf, uint32_t bufsz,
                         uint32_t *actlen);
/* IO$_DEACCESS -- shut the connection down both ways. */
uint32_t vms_kif_bg_deaccess(uint32_t exec_chan);
/* $DASSGN -- release the channel and its host socket. */
uint32_t vms_kif_bg_dassgn(uint32_t exec_chan);
/* Readiness poll fd -- hand back a real Linux readiness-only pollable fd for the
 * channel's socket (poll()/select()-able; data still moves via send/recv). The
 * fd is installed in the caller's fd table; *out_fd gets it on success. */
uint32_t vms_kif_bg_pollfd(uint32_t exec_chan, int *out_fd);
/* Materialize the channel's executive socket as a REAL, data-carrying fd (RUNG-3b):
 * real read/write route to the executive socket, and NO O_CLOEXEC so it survives
 * execve. The primitive the --wrap dup2/dup uses to hand a BGn: connection to a
 * ported daemon's exec'd child. *out_fd gets the real fd on success; fail-honest
 * (no /dev/vms -> SS$_NOSUCHDEV, bad channel -> SS$_IVCHAN), never a fabricated fd. */
uint32_t vms_kif_bg_materialize_fd(uint32_t exec_chan, int *out_fd);
/* IO$_SENSEMODE -- report the channel socket's local (which==0) or peer
 * (which==1) address, straight from the host kernel socket. The de-veneer path
 * (vms-4bf): getpeername gets the REAL remote IP:port. Out params (network byte
 * order) may be NULL if not wanted. */
uint32_t vms_kif_bg_getname(uint32_t exec_chan, uint32_t which,
                            uint16_t *family, uint16_t *port, uint32_t *addr);
/* IO$_SETMODE socket-option subfunction -- apply an integer option (e.g.
 * TCP_NODELAY, SO_KEEPALIVE) to the REAL host kernel socket. */
uint32_t vms_kif_bg_setsockopt(uint32_t exec_chan, int level, int optname,
                               int optval);
/* IO$_SENSEMODE socket-option subfunction -- read an integer option back off
 * the live host kernel socket; *out_optval gets it on success. */
uint32_t vms_kif_bg_getsockopt(uint32_t exec_chan, int level, int optname,
                               int *out_optval);
/* Server path (vms-698, OpenSSH server port). bind binds a local AF_INET address
 * (network byte order; port 0 = ephemeral) and returns the EFFECTIVE port/addr in
 * *out_port/*out_addr; listen marks the socket passive; accept blocks for one
 * inbound connection and installs it onto accept_exec_chan (a second BG channel
 * the caller $ASSIGNed empty), returning the peer address. NULL out params OK. */
uint32_t vms_kif_bg_bind(uint32_t exec_chan, uint16_t family, uint16_t port,
                         uint32_t addr, uint16_t *out_port, uint32_t *out_addr);
uint32_t vms_kif_bg_listen(uint32_t exec_chan, int backlog);
uint32_t vms_kif_bg_accept(uint32_t listen_exec_chan, uint32_t accept_exec_chan,
                           uint16_t *family, uint16_t *port, uint32_t *addr);

/* ================================================================
 * Files-11 (ODS-2) ACP -- channel + mount front-end (vms-149, epic vms-208)
 *
 * The executive owns the mounted-volume table (src/kernel-core/vmsfs_acp.c): a
 * volume mounted by one process is reachable by ANY other process that $ASSIGNs
 * the unit -- the shared executive state that replaces the userspace adapter's
 * per-process passthrough. This rung carries the CHANNEL half of the ACP; the
 * ACP-QIO file operations (IO$_ACCESS/READVBLK/...) are later rungs.
 *
 * NO PER-PROCESS FALLBACK (CLAUDE.md Rule 9 / INV-6): every entry point below
 * returns SS$_NOSUCHDEV, never a private substitute, when /dev/vms is
 * unreachable -- checked explicitly (acp_bind_ok()) exactly as the mailbox
 * wrappers do.
 * ================================================================ */

/* $MOUNT an ODS-2 volume into the executive-global mounted table by unit name
 * (e.g. "DKA0:"). Idempotent. SS$_NOSUCHDEV if /dev/vms is absent.
 * WIRED (vms-481, epic vms-208): the DCL MOUNT command mounts a volume through
 * the ACP -- src/vmsdcl/dcl_cmd_misc.c cmd_mount emits vms_kif_acp_mount instead
 * of the retired setuid mount(2) helper (the /vms passthrough is gone). Also
 * exercised by tests/qemu/test_syssvc_acp_channel.c and test_syssvc_dcl_acp.c. */
uint32_t vms_kif_acp_mount(const char *devnam);

/* $DISMOUNT: remove a volume from the executive-global mounted table.
 * SS$_NOSUCHDEV if it is not mounted, SS$_DEVALLOC while channels remain
 * assigned to it, SS$_NOSUCHDEV if /dev/vms is absent.
 * WIRED (vms-149, epic vms-208): the DCL DISMOUNT command removes a volume
 * through the ACP -- src/vmsdcl/dcl_cmd_misc.c cmd_dismount emits
 * vms_kif_acp_dmount, mirroring cmd_mount's vms-481 switch to vms_kif_acp_mount
 * (the /proc/mounts scan is gone). Also exercised by
 * tests/qemu/test_syssvc_acp_channel.c. */
uint32_t vms_kif_acp_dmount(const char *devnam);

/* $ASSIGN a FILE-CLASS channel to a mounted ODS-2 volume by unit name; the
 * returned exec_chan is an EXECUTIVE channel bound to the volume (see
 * src/libvms/syssvc/sys_assign.c for how it is stored in the caller's PCB).
 * SS$_NOSUCHDEV if the unit is not a mounted volume (fail-honest -- never a
 * fabricated channel) or if /dev/vms is absent. Released by vms_kif_dassgn().
 * Wired (vms-149): src/libvms/syssvc/sys_assign.c routes $ASSIGN of the boot
 * unit (DKA0:/SYS$SYSDEVICE) here -- the census gate is what proves it has a
 * product caller. */
uint32_t vms_kif_acp_assign(const char *devnam, uint32_t *exec_chan);

/*
 * IO$_ACCESS / IO$_DEACCESS (vms-204, epic vms-208): open a file on a
 * file-class channel and release it. The caller fills the IN fields of *args
 * (chan, acctl, either fidmode+fid_* for open-by-FID or did_*+name+version for
 * open-by-name, and optionally probe_vbn); on return *args carries the resolved
 * FID, attributes, window summary and probe_lbn, and the function returns the
 * SS$_ status (SS$_NOSUCHFILE / SS$_NOPRIV / SS$_IVCHAN fail-honest, or
 * SS$_NOSUCHDEV if /dev/vms is absent). See src/kernel/vms_acp.h for the FIB/ATR
 * interface.
 *
 * WIRED (vms-bc7, epic vms-208): RMS $OPEN/$CREATE emit vms_kif_acp_access to
 * open a file on the SYS$DISK channel (src/vmsrms/rms_core.c rms_acp_open_file,
 * and rms_acp_resolve_did walking directory components), and $CLOSE emits
 * vms_kif_acp_deaccess (rms_impl_close / rms_acp_close_handle). Also exercised
 * by tests/qemu/test_syssvc_acp_access.c and test_syssvc_rms_acp.c. */
uint32_t vms_kif_acp_access(struct vms_acp_access_args *args);
uint32_t vms_kif_acp_deaccess(uint32_t chan);
/*
 * Virtual-block transfer on an accessed file channel ({vbn,offset,length}
 * through the window); a write past EOF implicitly extends (BITMAP.SYS
 * allocation + FH2 grow). args carries the transfer control fields + a user data
 * `buffer` pointer; see src/kernel/vms_acp.h. Fail-honest (SS$_NOSUCHDEV if
 * /dev/vms is absent). Exercised only by tests/qemu/test_syssvc_acp_rw.c against
 * a real /dev/vms.
 * WIRED (vms-bc7, epic vms-208): RMS record I/O rides these -- $GET is
 * vms_kif_acp_readvb and $PUT is vms_kif_acp_writevb, through the block-I/O
 * substrate src/vmsrms/rms_io.c (rms_io_read / rms_io_write) under the seq/rel
 * record engines; also exercised by test_syssvc_acp_rw.c / test_syssvc_rms_acp.c.
 */
uint32_t vms_kif_acp_readvb(struct vms_acp_rw_args *args);
uint32_t vms_kif_acp_writevb(struct vms_acp_rw_args *args);

/* IO$_ACPCONTROL wildcard directory search -- the $SEARCH primitive (vms-a0b).
 * (Re)open (args->wcc_reset) or continue a wildcard directory context on a
 * file-class channel and return the NEXT matching {name, version, FID}, or
 * SS$_NOMOREFILES when the context is exhausted. See src/kernel/vms_acp.h for
 * the FIB/wildcard interface. Returns SS$_NOSUCHDEV if /dev/vms is absent.
 * WIRED (vms-481, epic vms-208): RMS $SEARCH emits vms_kif_acp_acpcontrol --
 * src/vmsrms/rms_search.c drives the executive wildcard directory context, and
 * the DCL DIRECTORY command + the F$SEARCH lexical reach it through sys$search
 * (src/vmsdcl/dcl_filespec.c dcl_rms_dir_*). Also exercised by
 * tests/qemu/test_syssvc_acp_search.c and test_syssvc_dcl_acp.c. */
uint32_t vms_kif_acp_acpcontrol(struct vms_acp_acpcontrol_args *args);

/*
 * IO$_CREATE / IO$_DELETE / IO$_MODIFY -- the ACP file-operation umbrella
 * (vms-5303): create a file (real FID from INDEXF.SYS, versioned directory
 * entry, optional access), delete a file (remove the entry + deallocate header
 * and blocks), modify a file (extend/truncate/write attributes). func-dispatched
 * on args->func (VMS_ACP_FOP_*). See src/kernel/vms_acp.h for the FIB/ATR
 * interface and the ioctl-mapping decision. Returns SS$_NOSUCHDEV if /dev/vms
 * is absent. WIRED (vms-bc7, epic vms-208): RMS $CREATE emits IO$_CREATE,
 * $ERASE emits IO$_DELETE, and $EXTEND / truncate emit IO$_MODIFY through this
 * umbrella (src/vmsrms/rms_core.c rms_impl_create/_erase/_extend/_close and
 * rms_io.c rms_io_ftruncate); also exercised by tests/qemu/test_syssvc_acp_create.c
 * and test_syssvc_rms_acp.c against real /dev/vms. */
uint32_t vms_kif_acp_fileop(struct vms_acp_fileop_args *args);

#endif /* _VMS_KIF_H */
