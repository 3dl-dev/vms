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
 * OVMX-UNWIRED: vms_kif_setident (vms-2b8) -- LOGINOUT still does not call
 * it; src/vmsdcl/dcl_main.c reads VMS_USERNAME, VMS_UIC_GROUP,
 * VMS_UIC_MEMBER and VMS_PRIVILEGES from the environment, so a process
 * still names its own identity. */
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
 * THE WHOLE FAMILY IS UNWIRED (vms-as1): src/vmsprocess delivers ASTs
 * per-process, so an AST declared in one process is unknown to every other.
 * ================================================================ */

/* Declare AST at specified access mode
 * OVMX-UNWIRED: vms_kif_dclast (vms-as1) */
uint32_t vms_kif_dclast(uint64_t astadr, uint64_t astprm, uint8_t acmode);

/* Enable/disable AST delivery. Returns SS$_WASSET or SS$_WASCLR
 * OVMX-UNWIRED: vms_kif_setast (vms-as1) */
uint32_t vms_kif_setast(int enable);

/* Deliver next pending AST. Returns 0 if AST delivered, -1 if none
 * OVMX-UNWIRED: vms_kif_deliverast (vms-as1) */
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
 * THE WHOLE FAMILY IS UNWIRED. The kernel device table, these wrappers and
 * their QEMU suite were merged with no product reader: SHOW DEVICE still
 * prints the host Linux mount table and src/vmsdcl/dcl_cmd_show.c carries a
 * COMMENT saying the conversion is future work. A comment is not a caller --
 * that is exactly what this census exists to say out loud.
 * ================================================================ */

/* $ASSIGN a channel to a device by name. SS$_NOSUCHDEV if the
 * executive has no such device; SS$_IVDEVNAM if the name is not a
 * device name at all.
 * OVMX-UNWIRED: vms_kif_assign (vms-dv1) */
uint32_t vms_kif_assign(const char *devnam, uint32_t *chan);

/* $DASSGN the channel. SS$_IVCHAN if it is not one of ours.
 * OVMX-UNWIRED: vms_kif_dassgn (vms-dv1) */
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
 * OVMX-UNWIRED: vms_kif_getdvi_chan (vms-fb9) */
uint32_t vms_kif_getdvi_chan(uint32_t chan, struct vms_devinfo *info);

/* Enumerate the device table. Pass *index = 0 for the first row; each
 * call fills info and advances *index. Returns SS$_NOMOREDEV when the
 * scan is exhausted.
 * Wired: the census gate is what proves it has a product caller. */
uint32_t vms_kif_devscan(uint32_t *index, struct vms_devinfo *info);

/* Set terminal characteristics through an assigned channel (the
 * $QIO IO$_SETMODE path). flags is a mask of VMS_TTSET_*; SS$_IVCHAN
 * if the caller holds no such channel.
 * OVMX-UNWIRED: vms_kif_ttsetmode (vms-fb9) -- SET TERMINAL is the writer */
uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,
                           uint64_t setchar, uint64_t clrchar,
                           uint32_t width, uint32_t page);

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

#endif /* _VMS_KIF_H */
