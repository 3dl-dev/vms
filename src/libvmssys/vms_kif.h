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
 * authentication, and src/ovmx_provision/ovmx_provision.c (PROVISION.EXE --
 * the startup process PID 1 execs, which then execs DCL.EXE on STARTUP.COM in
 * the same process, vms-9b7) calls it for the SYSTEM identity that system
 * startup runs under. src/vmsdcl/dcl_main.c and
 * dcl_cmd_show.c read the row back through vms_kif_getjpi_self() instead of
 * the environment. */
uint32_t vms_kif_setident(const char *username, uint32_t uic,
                          uint64_t authorized_privs);

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
 * THE WHOLE FAMILY IS UNWIRED: no product code calls any of it, so
 * privileges and access modes are still whatever a process says they are
 * (src/vmsdcl/dcl_main.c reads VMS_PRIVILEGES from the environment).
 * vms-pv1 is the item that makes the executive the enforcer.
 * ================================================================ */

/* Set access mode. Returns SS$_ status
 * OVMX-UNWIRED: vms_kif_setmode (vms-pv1) */
uint32_t vms_kif_setmode(uint8_t mode);

/* Get current mode and privileges
 * OVMX-UNWIRED: vms_kif_getmode (vms-pv1) */
uint32_t vms_kif_getmode(uint8_t *mode, uint64_t *cur_privs, uint64_t *perm_privs);

/* Set/clear privileges. Returns previous privilege mask in *prev
 * OVMX-UNWIRED: vms_kif_setprv (vms-pv1) -- $SETPRV does not reach here yet */
uint32_t vms_kif_setprv(uint64_t mask, int enable, int permanent, uint64_t *prev);

/* Check if privileges are held. Returns SS$_NORMAL or SS$_NOPRIV
 * OVMX-UNWIRED: vms_kif_chkpriv (vms-pv1) -- no privilege check in OVMX asks
 * the executive, which is why any process can still claim any privilege */
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
 * OVMX-UNWIRED: vms_kif_alloc (vms-dv1) */
uint32_t vms_kif_alloc(const char *devnam);

/* $DALLOC the device. SS$_DEVNOTALLOC if this process does not have it
 * allocated.
 * OVMX-UNWIRED: vms_kif_dalloc (vms-dv1) */
uint32_t vms_kif_dalloc(const char *devnam);

/* vms_kif_alloc_op() is the static body those two share inside vms_kif.c.
 * It has no prototype here and is not part of the interface, but the census
 * universe is the union of what this header prototypes and what vms_kif.c
 * defines -- static definitions included, so that marking a definition static
 * cannot drop it out of the census. It is reached only from $ALLOC/$DALLOC,
 * so while they are unwired it is unwired too, and it says so:
 * OVMX-UNWIRED: vms_kif_alloc_op (vms-dv1) -- shared body of the two above */

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
 * OVMX-UNWIRED: vms_kif_p1_map (vms-6f1) -- DCL laying a real P1 control-region
 * window at process startup and registering its extent here is increment v of
 * vms-68f. Increment iv (in-process activation, imgact_activate(),
 * src/libvms/syssvc/sys_imgact.c) protects critical-P1 pages with
 * vms_kif_p1_protect() directly but registers no P1 extent, so nothing in the
 * product calls this one yet. */
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
 * mapping). Returns 1 and fills value/vallen/attrs when found, 0 when the
 * name is absent from the table, or -1 when the executive/mapping is
 * unavailable (the caller renders that as SS$_NOSUCHDEV). `value` receives
 * the first (index 0) equivalence string, NUL-terminated.
 * Wired: sys$trnlnm (src/libvms/syssvc/sys_logical.c). */
int vms_kif_lnm_translate(uint32_t table, const char *name,
                          char *value, uint32_t valsz,
                          uint16_t *vallen, uint32_t *attrs);

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

/* $QIO IO$_READVBLK-equivalent: blocks until a message is queued, then
 * copies up to `bufsz` bytes of it into `buf` and reports the message's
 * true length in *actlen (which may exceed bufsz if the caller's buffer
 * was smaller than the message -- the excess is discarded, as $QIO
 * truncates an oversized mailbox message into an undersized buffer). */
uint32_t vms_kif_mbx_read(uint32_t exec_chan, void *buf, uint32_t bufsz,
                          uint32_t *actlen);

#endif /* _VMS_KIF_H */
