/*
 * vms_kif.h - Kernel Interface for VMS userspace
 *
 * Provides userspace wrappers around the /dev/vms ioctl interface.
 * These functions abstract the ioctl calls behind VMS-style APIs.
 *
 * Usage:
 *   1. Call vms_kif_open() at process startup
 *   2. Call vms_kif_register() to register with the kernel module
 *   3. Use vms_kif_* functions for VMS operations
 *   4. Call vms_kif_close() at process exit
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

/* Close the /dev/vms fd */
void vms_kif_close(void);

/* Register this process with the kernel module */
uint32_t vms_kif_register(uint32_t vms_pid, uint64_t init_privs);

/* ================================================================
 * Access Mode (3a)
 * ================================================================ */

/* Set access mode. Returns SS$_ status */
uint32_t vms_kif_setmode(uint8_t mode);

/* Get current mode and privileges */
uint32_t vms_kif_getmode(uint8_t *mode, uint64_t *cur_privs, uint64_t *perm_privs);

/* Set/clear privileges. Returns previous privilege mask in *prev */
uint32_t vms_kif_setprv(uint64_t mask, int enable, int permanent, uint64_t *prev);

/* Check if privileges are held. Returns SS$_NORMAL or SS$_NOPRIV */
uint32_t vms_kif_chkpriv(uint64_t mask);

/* ================================================================
 * AST Delivery (3b)
 * ================================================================ */

/* Declare AST at specified access mode */
uint32_t vms_kif_dclast(uint64_t astadr, uint64_t astprm, uint8_t acmode);

/* Enable/disable AST delivery. Returns SS$_WASSET or SS$_WASCLR */
uint32_t vms_kif_setast(int enable);

/* Deliver next pending AST. Returns 0 if AST delivered, -1 if none */
int vms_kif_deliverast(uint64_t *astadr, uint64_t *astprm, uint8_t *acmode);

/* ================================================================
 * Event Flags (3c)
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

/* Get lock information */
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
 * ================================================================ */

/* $ASSIGN a channel to a device by name. SS$_NOSUCHDEV if the
 * executive has no such device; SS$_IVDEVNAM if the name is not a
 * device name at all. */
uint32_t vms_kif_assign(const char *devnam, uint32_t *chan);

/* $DASSGN the channel. SS$_IVCHAN if it is not one of ours. */
uint32_t vms_kif_dassgn(uint32_t chan);

/* Read a device row by name. SS$_NOSUCHDEV if there is no such device. */
uint32_t vms_kif_getdvi_devnam(const char *devnam, struct vms_devinfo *info);

/* Read the device row behind an assigned channel. SS$_IVCHAN if the
 * channel is not ours. */
uint32_t vms_kif_getdvi_chan(uint32_t chan, struct vms_devinfo *info);

/* Enumerate the device table. Pass *index = 0 for the first row; each
 * call fills info and advances *index. Returns SS$_NOMOREDEV when the
 * scan is exhausted. */
uint32_t vms_kif_devscan(uint32_t *index, struct vms_devinfo *info);

/* Set terminal characteristics through an assigned channel (the
 * $QIO IO$_SETMODE path). flags is a mask of VMS_TTSET_*; SS$_IVCHAN
 * if the caller holds no such channel. */
uint32_t vms_kif_ttsetmode(uint32_t chan, uint32_t flags,
                           uint64_t setchar, uint64_t clrchar,
                           uint32_t width, uint32_t page);

#endif /* _VMS_KIF_H */
