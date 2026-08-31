/*
 * STARLET.H - VMS Master System Include File
 *
 * OpenVMX compatibility layer - On VMS, #include <starlet.h> provides
 * all system service prototypes and the fundamental data type definitions.
 * This header replicates that behavior for the OVMX project.
 *
 * This file includes all foundational headers (descriptors, status codes,
 * I/O definitions) and declares the full set of SYS$ system service
 * entry points.
 *
 * Reference: OpenVMS System Services Reference Manual
 *            OpenVMS Programming Concepts Manual
 */

#ifndef __STARLET_H
#define __STARLET_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * Foundational data type headers
 * ================================================================ */

#include "descrip.h"
#include "ssdef.h"
#include "stsdef.h"
#include "iodef.h"
#include "lnmdef.h"
#include "prcdef.h"
#include "rmsdef.h"
#include "prvdef.h"
#include "chfdef.h"
#include "msgdef.h"
#include "libclidef.h"
#include "opcdef.h"
#include "gen64def.h"
#include "iledef.h"
#include "fscndef.h"

/* ================================================================
 * Run-time library routine headers
 * ================================================================ */

#include "lib$routines.h"
#include "str$routines.h"
#include "mth$routines.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * I/O Channel Assignment Services
 *
 * SYS$ASSIGN assigns a channel to a device for subsequent I/O.
 * SYS$DASSGN deassigns the channel when done.
 * ================================================================ */

/**
 * sys$assign - Assign I/O channel
 *
 * @param devnam   Pointer to descriptor of device name
 * @param chan      Pointer to receive assigned channel number
 * @param acmode   Access mode for channel (0=kernel .. 3=user)
 * @param mbxnam   Optional pointer to descriptor of mailbox name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$assign(
    const struct dsc$descriptor_s *devnam,
    uint16_t *chan,
    uint32_t acmode,
    const struct dsc$descriptor_s *mbxnam
);

/**
 * sys$dassgn - Deassign I/O channel
 *
 * @param chan  Channel number to deassign
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$dassgn(uint16_t chan);

/* ================================================================
 * Mailbox Services
 *
 * SYS$CREMBX creates a virtual mailbox for interprocess communication.
 * SYS$DELMBX marks a mailbox for deletion when all channels are deassigned.
 * ================================================================ */

/**
 * sys$crembx - Create mailbox and assign channel
 *
 * @param prmflg  Permanent flag (1 = permanent, 0 = temporary)
 * @param chan     Pointer to receive assigned channel number
 * @param maxmsg  Maximum message size in bytes (0 = default)
 * @param bufquo  Buffer quota in bytes (0 = default)
 * @param promsk  Protection mask (0 = default)
 * @param acmode  Access mode for the channel (0 = kernel)
 * @param lognam  Optional pointer to descriptor of logical name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$crembx(
    int prmflg,
    uint16_t *chan,
    uint32_t maxmsg,
    uint32_t bufquo,
    uint32_t promsk,
    uint32_t acmode,
    const struct dsc$descriptor_s *lognam
);

/**
 * sys$delmbx - Delete mailbox
 *
 * @param chan  Channel number of mailbox to delete
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$delmbx(uint16_t chan);

/* ================================================================
 * Queued I/O Services
 *
 * SYS$QIO queues an I/O request and returns immediately.
 * SYS$QIOW queues and waits for completion.
 * ================================================================ */

/**
 * sys$qio - Queue I/O request
 *
 * @param efn     Event flag number to set on completion
 * @param chan    I/O channel number
 * @param func   I/O function code (IO$_ value with optional IO$M_ modifiers)
 * @param iosb   Pointer to I/O status block
 * @param astadr  Optional AST completion routine address
 * @param astprm  AST parameter
 * @param p1-p6  Function-dependent parameters
 *
 * @return  SS$_NORMAL if queued successfully
 */
uint32_t sys$qio(
    uint32_t efn,
    uint16_t chan,
    uint32_t func,
    void *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm,
    void *p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6
);

/**
 * sys$qiow - Queue I/O request and wait
 *
 * Same parameters as sys$qio.  Blocks until the I/O completes.
 */
uint32_t sys$qiow(
    uint32_t efn,
    uint16_t chan,
    uint32_t func,
    void *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm,
    void *p1, uint32_t p2, uint32_t p3,
    uint32_t p4, uint32_t p5, uint32_t p6
);

/* ================================================================
 * Event Flag Services
 *
 * Event flags are the primary VMS synchronization mechanism.
 * Flags 0-63 are local; 64-127 are in common clusters.
 * ================================================================ */

/**
 * sys$setef - Set event flag
 *
 * @param efn  Event flag number to set
 * @return     SS$_WASCLR if previously clear, SS$_WASSET if already set
 */
uint32_t sys$setef(uint32_t efn);

/**
 * sys$clref - Clear event flag
 *
 * @param efn  Event flag number to clear
 * @return     SS$_WASCLR or SS$_WASSET
 */
uint32_t sys$clref(uint32_t efn);

/**
 * sys$waitfr - Wait for single event flag
 *
 * @param efn  Event flag number to wait for
 * @return     SS$_NORMAL when flag is set
 */
uint32_t sys$waitfr(uint32_t efn);

/**
 * sys$wflor - Wait for logical OR of event flags
 *
 * @param efn   Event flag cluster number (0 or 1 for local)
 * @param mask  Bitmask of flags to wait for (any one)
 *
 * @return  SS$_NORMAL when any specified flag is set
 */
uint32_t sys$wflor(uint32_t efn, uint32_t mask);

/**
 * sys$wfland - Wait for logical AND of event flags
 *
 * @param efn   Event flag cluster number
 * @param mask  Bitmask of flags to wait for (all)
 *
 * @return  SS$_NORMAL when all specified flags are set
 */
uint32_t sys$wfland(uint32_t efn, uint32_t mask);

/**
 * sys$synch - Synchronize with async system service
 *
 * @param efn   Event flag number to wait on
 * @param iosb  Pointer to I/O status block (NULL = just wait for EF)
 *
 * @return  IOSB status if iosb provided, SS$_NORMAL otherwise
 */
uint32_t sys$synch(uint32_t efn, void *iosb);

/**
 * sys$readef - Read event flag state
 *
 * @param efn    Event flag number
 * @param state  Pointer to receive cluster state
 *
 * @return  SS$_WASCLR or SS$_WASSET
 */
uint32_t sys$readef(uint32_t efn, uint32_t *state);

/**
 * sys$ascefc - Associate common event flag cluster
 *
 * @param efn    Starting event flag number (64 or 96)
 * @param name   Pointer to descriptor of cluster name
 * @param prot   Protection mask
 * @param perm   Permanent flag
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$ascefc(
    uint32_t efn,
    const struct dsc$descriptor_s *name,
    uint32_t prot,
    uint32_t perm
);

/**
 * sys$dacefc - Disassociate from common event flag cluster
 */
uint32_t sys$dacefc(uint32_t efn);

/**
 * sys$dlcefc - Delete common event flag cluster
 */
uint32_t sys$dlcefc(const struct dsc$descriptor_s *name);

/* ================================================================
 * Time Services
 * ================================================================ */

/**
 * sys$gettim - Get current system time
 *
 * @param timadr  Pointer to quadword to receive 64-bit VMS time
 *
 * @return  SS$_NORMAL on success
 *
 * VMS time is a 64-bit value counting 100-nanosecond intervals
 * since November 17, 1858 00:00:00.00 (the Smithsonian base date).
 */
uint32_t sys$gettim(uint64_t *timadr);

/**
 * sys$getutc - Get current system time in UTC.
 *
 * @param timadr  Pointer to quadword to receive 64-bit VMS time
 *
 * @return  SS$_NORMAL on success
 *
 * Like sys$gettim, but always returns Coordinated Universal Time
 * regardless of the process/system time zone differential factor.
 * OVMX's internal clock (CLOCK_REALTIME) is already UTC-based with no
 * local offset applied, so sys$getutc and sys$gettim currently return
 * identical values; the separate entry point is provided for source
 * compatibility with programs that call SYS$GETUTC explicitly.
 */
uint32_t sys$getutc(uint64_t *timadr);

/**
 * sys$numtim - Convert binary time to numeric components
 *
 * @param timbuf  Array of 7 words: year, month, day, hour, minute, second, hundredths
 * @param timadr  Optional pointer to time (NULL = current time)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$numtim(uint16_t timbuf[7], const uint64_t *timadr);

/* Forward declaration of the generic 64-bit union (full def in gen64def.h).
 * sys$bintim's timadr is "the address of the quadword system time, which
 * receives the converted time" — VSI callers pass a GENERIC_64 (struct
 * _generic_64), not a bare uint64_t*. */
struct _generic_64;

/**
 * sys$bintim - Convert ASCII time string to binary time
 *
 * @param timbuf  Address of a character-string descriptor pointing to the
 *                ASCII absolute/delta time to convert (by descriptor).
 * @param timadr  Address of a quadword that receives the converted 64-bit
 *                system-format time.
 *
 * @return  SS$_NORMAL on success, SS$_IVTIME on invalid format
 *
 * Signature per VSI OpenVMS System Services Reference Manual, $BINTIM
 * ("timadr — the ... address of the quadword system time, which receives
 * the converted time") and the VSI-supplied starlet.h C prototype
 *   int sys$bintim(void *timbuf, struct _generic_64 *timadr);
 * (VSI OpenVMS Wiki, $GETJPI/$BINTIM pages). OVMX keeps the concrete
 * descriptor type for timbuf (a compatible refinement of VSI's void*).
 */
uint32_t sys$bintim(
    const struct dsc$descriptor_s *timbuf,
    struct _generic_64 *timadr
);

/**
 * sys$asctim - Convert binary time to ASCII string
 *
 * @param timlen  Optional pointer to receive string length
 * @param timbuf  Pointer to descriptor of output buffer
 * @param timadr  Optional pointer to time (NULL = current time)
 * @param cvtflg  Conversion flags (0 = full, 1 = date only)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$asctim(
    uint16_t *timlen,
    struct dsc$descriptor_s *timbuf,
    const uint64_t *timadr,
    uint32_t cvtflg
);

/**
 * sys$setimr - Set timer
 *
 * @param efn     Event flag to set on expiration
 * @param daytim  Pointer to quadword time (delta or absolute)
 * @param astadr  Optional AST routine for timer expiration
 * @param reqidt  Request identification for cancellation
 * @param flags   Flags (reserved, pass 0)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$setimr(
    uint32_t efn,
    const uint64_t *daytim,
    void (*astadr)(uint32_t),
    uint32_t reqidt,
    uint32_t flags
);

/**
 * sys$cantim - Cancel timer
 *
 * @param reqidt  Request identification (0 = cancel all)
 * @param acmode  Access mode
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$cantim(uint32_t reqidt, uint32_t acmode);

/* ================================================================
 * Logical Name Services
 * ================================================================ */

/**
 * sys$crelnm - Create logical name
 *
 * @param attr    Optional pointer to logical name attributes
 * @param tabnam  Pointer to descriptor of table name
 * @param lognam  Pointer to descriptor of logical name
 * @param acmode  Optional pointer to access mode
 * @param itmlst  Pointer to item list (equivalence strings, etc.)
 *
 * @return  SS$_NORMAL on success, SS$_SUPERSEDE if name replaced
 */
uint32_t sys$crelnm(
    const uint32_t *attr,
    const struct dsc$descriptor_s *tabnam,
    const struct dsc$descriptor_s *lognam,
    const uint8_t *acmode,
    const struct item_list_3 *itmlst
);

/**
 * sys$dellnm - Delete logical name
 *
 * @param tabnam  Pointer to descriptor of table name
 * @param lognam  Pointer to descriptor of logical name
 * @param acmode  Optional pointer to access mode
 *
 * @return  SS$_NORMAL on success, SS$_NOLOGNAM if not found
 */
uint32_t sys$dellnm(
    const struct dsc$descriptor_s *tabnam,
    const struct dsc$descriptor_s *lognam,
    const uint8_t *acmode
);

/**
 * sys$trnlnm - Translate logical name
 *
 * @param attr    Optional pointer to attributes to match
 * @param tabnam  Pointer to descriptor of table name
 * @param lognam  Pointer to descriptor of logical name
 * @param acmode  Optional pointer to access mode
 * @param itmlst  Pointer to item list for results
 *
 * @return  SS$_NORMAL on success, SS$_NOLOGNAM if not found
 */
uint32_t sys$trnlnm(
    const uint32_t *attr,
    const struct dsc$descriptor_s *tabnam,
    const struct dsc$descriptor_s *lognam,
    const uint8_t *acmode,
    const struct item_list_3 *itmlst
);

/* ================================================================
 * Process Management Services
 * ================================================================ */

/**
 * sys$creprc - Create process
 *
 * @param pidadr   Optional pointer to receive new process ID
 * @param image    Pointer to descriptor of image to run
 * @param input    Pointer to descriptor of SYS$INPUT equivalent
 * @param output   Pointer to descriptor of SYS$OUTPUT equivalent
 * @param error    Pointer to descriptor of SYS$ERROR equivalent
 * @param prvadr   Optional pointer to privilege mask
 * @param quota    Optional pointer to quota list
 * @param prcnam   Optional pointer to descriptor of process name
 * @param baspri   Base priority
 * @param uic      UIC (User Identification Code)
 * @param mbxunt   Termination mailbox unit number
 * @param stsflg   Status flags (PRC$M_ values)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$creprc(
    uint32_t *pidadr,
    const struct dsc$descriptor_s *image,
    const struct dsc$descriptor_s *input,
    const struct dsc$descriptor_s *output,
    const struct dsc$descriptor_s *error,
    const void *prvadr,
    const void *quota,
    const struct dsc$descriptor_s *prcnam,
    uint32_t baspri,
    uint32_t uic,
    uint32_t mbxunt,
    uint32_t stsflg
);

/**
 * sys$delprc - Delete process
 *
 * @param pidadr  Optional pointer to process ID (NULL = current process)
 * @param prcnam  Optional pointer to descriptor of process name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$delprc(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam
);

/**
 * sys$hiber - Hibernate (suspend current process)
 *
 * @return  SS$_NORMAL when awakened
 */
uint32_t sys$hiber(void);

/**
 * sys$wake - Wake a hibernating process
 *
 * @param pidadr  Optional pointer to process ID (NULL = current)
 * @param prcnam  Optional pointer to descriptor of process name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$wake(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam
);

/**
 * sys$exit - Exit image
 *
 * @param code  Exit status code
 *
 * @return  Does not return
 */
uint32_t sys$exit(uint32_t code);

/* ================================================================
 * Process and System Information Services
 * ================================================================ */

/**
 * sys$getjpi - Get job/process information
 *
 * @param efn     Event flag for completion
 * @param pidadr  Optional pointer to process ID
 * @param prcnam  Optional pointer to descriptor of process name
 * @param itmlst  Pointer to item list
 * @param iosb    Pointer to I/O status block
 * @param astadr  Optional AST completion routine
 * @param astprm  AST parameter
 *
 * @return  SS$_NORMAL on success (or SS$_SYNCH if completed synchronously)
 */
/*
 * prcnam and itmlst are passed as void* to match the VSI-supplied starlet.h
 * C prototype (VSI OpenVMS Wiki, $GETJPI):
 *   int sys$getjpi(unsigned int efn, unsigned int *pidadr, void *prcnam,
 *                  void *itmlst, struct _iosb *iosb,
 *                  void (*astadr)(__unknown_params), unsigned __int64 astprm);
 * Real VMS uses void* here precisely so a caller may pass any flavour of
 * process-name descriptor or item-list entry (ILE3, item_list_3, ...); the
 * over-specified typed pointers OVMX previously declared rejected the
 * standard ILE3 item lists the corpus (and real VMS code) build.
 */
uint32_t sys$getjpi(
    uint32_t efn,
    const uint32_t *pidadr,
    void *prcnam,
    void *itmlst,
    void *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/**
 * sys$getjpiw - Get job/process information (wait for completion)
 * Same argument list as sys$getjpi (VSI OpenVMS System Services Reference,
 * $GETJPIW: "identical to the $GETJPI service ... completes synchronously").
 */
uint32_t sys$getjpiw(
    uint32_t efn,
    const uint32_t *pidadr,
    void *prcnam,
    void *itmlst,
    void *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/**
 * sys$getsyi - Get system information
 *
 * @param efn       Event flag for completion
 * @param csidadr   Optional pointer to cluster system ID
 * @param nodename  Optional pointer to descriptor of node name
 * @param itmlst    Pointer to item list
 * @param iosb      Pointer to I/O status block
 * @param astadr    Optional AST completion routine
 * @param astprm    AST parameter
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$getsyi(
    uint32_t efn,
    const uint32_t *csidadr,
    const struct dsc$descriptor_s *nodename,
    const struct item_list_3 *itmlst,
    void *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/**
 * sys$getsyiw - Get system information (wait for completion)
 */
uint32_t sys$getsyiw(
    uint32_t efn,
    const uint32_t *csidadr,
    const struct dsc$descriptor_s *nodename,
    const struct item_list_3 *itmlst,
    void *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/* ================================================================
 * Memory Management Services
 * ================================================================ */

/**
 * sys$cretva - Create virtual address space
 *
 * @param inadr   Address range (two-longword array: start, end)
 * @param retadr  Returned address range
 * @param acmode  Access mode for the pages
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$cretva(
    const void *inadr,
    void *retadr,
    uint32_t acmode
);

/**
 * sys$deltva - Delete virtual address space
 */
uint32_t sys$deltva(
    const void *inadr,
    void *retadr,
    uint32_t acmode
);

/**
 * sys$expreg - Expand program region
 *
 * @param pagcnt   Number of pages to add
 * @param retadr   Returned address range
 * @param acmode   Access mode
 * @param region   Region indicator (0=P0, 1=P1)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$expreg(
    uint32_t pagcnt,
    void *retadr,
    uint32_t acmode,
    uint32_t region
);

/**
 * sys$purgws - Purge working set
 *
 * @param inadr  Address range to purge (two-pointer array: start, end;
 *               see VA_RANGE in va_rangedef.h)
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if inadr is NULL
 *
 * On real VMS, removes all but a minimal number of resident pages in
 * the given range from the process's working set. OVMX processes are
 * demand-paged by the Linux VMM rather than a VMS-style adjustable
 * working set, so there is no equivalent resident-page list to trim;
 * OVMX validates the address range and returns success without
 * further action (consistent with how sys$cretva/sys$deltva above
 * already treat page-residency-adjacent parameters as no-ops).
 */
uint32_t sys$purgws(
    const void *inadr
);

/**
 * sys$crmpsc - Create and map section
 *
 * @param inadr    Address range
 * @param retadr   Returned address range
 * @param acmode   Access mode
 * @param flags    Section flags
 * @param gsdnam   Optional global section name
 * @param ident    Optional section version identification
 * @param relpag   Relative page offset
 * @param chan     Channel number (for file-backed sections)
 * @param pagcnt   Page count
 * @param vbn     Virtual block number
 * @param prot    Protection mask
 * @param pfc     Page fault cluster size
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$crmpsc(
    const void *inadr,
    void *retadr,
    uint32_t acmode,
    uint32_t flags,
    const struct dsc$descriptor_s *gsdnam,
    uint64_t *ident,
    uint32_t relpag,
    uint16_t chan,
    uint32_t pagcnt,
    uint32_t vbn,
    uint32_t prot,
    uint32_t pfc
);

/**
 * sys$deltva_64 - Delete virtual address space (64-bit region API)
 *
 * The 64-bit-addressing counterpart to sys$deltva, used with the P0/P1/P2
 * region-based VA management family (sys$create_region_64, sys$cretva_64,
 * sys$expreg_64, ...). OVMX implements only this one entry point of that
 * family for now (see docs/conformance-gap-report.md, vms-fb3); the
 * region_id_64 argument is accepted for call-site compatibility but not
 * validated against a region registry (no sys$create_region_64 exists
 * yet to have created one).
 *
 * Signature matches the corpus call sites (tests/corpus/tier1-examples/
 * sys_cretva_64.c, sys_gs64.c, sys_create_gpfn.c): unlike 32-bit
 * sys$deltva (a single two-pointer VA_RANGE), the 64-bit form takes an
 * explicit byte length and separate output address/length pointers.
 *
 * @param region_id_64  Pointer to the 64-bit region identifier (GENERIC_64;
 *                       accepted, not yet validated - see above)
 * @param inadr_64       Start address of the range to delete
 * @param bytlen_64      Length in bytes of the range to delete
 * @param acmode         Access mode (ignored)
 * @param retadr_64      Optional pointer to receive the deleted start address
 * @param retlen_64      Optional pointer to receive the deleted byte length
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if inadr_64 is NULL
 */
uint32_t sys$deltva_64(
    const GENERIC_64 *region_id_64,
    void *inadr_64,
    uint64_t bytlen_64,
    uint32_t acmode,
    void **retadr_64,
    uint64_t *retlen_64
);

/**
 * sys$dgblsc - Delete global section
 *
 * Marks a global section for deletion. Per documented VMS behavior, the
 * section is not actually removed until every process that has mapped
 * it also deletes its virtual-address range onto it (via sys$deltva /
 * sys$deltva_64) - this call only marks intent. OVMX does not maintain
 * a cross-process named-global-section registry (sys$mgblsc/
 * sys$mgblsc_64/sys$mgblsc_gpfn_64 are not implemented - see
 * docs/conformance-gap-report.md), so this validates its arguments and
 * returns success without a registry lookup, consistent with how
 * sys$purgws above documents an equivalent honest simplification.
 *
 * @param flags   Section flags (SEC$M_SYSGBL etc. - ignored)
 * @param gsdnam  Descriptor of the global section name (required)
 * @param ident   Optional section version identification
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if gsdnam is missing
 */
uint32_t sys$dgblsc(
    uint32_t flags,
    const struct dsc$descriptor_s *gsdnam,
    void *ident
);

/**
 * sys$lkwset - Lock pages into working set
 *
 * @param inadr   Address range to lock (VA_RANGE - see va_rangedef.h)
 * @param retadr  Returned address range actually locked
 * @param acmode  Access mode (ignored)
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if inadr is NULL
 *
 * OVMX processes are demand-paged by the Linux VMM rather than a
 * VMS-style adjustable working set with a lockable resident-page list
 * (see sys$purgws above for the same tradeoff), so there is no
 * WSQUOTA-bounded lock to perform; OVMX validates the range, echoes it
 * to retadr, and returns success without further action. Because OVMX
 * has no working-set quota, this never returns the real VMS
 * SS$_LKWSETFUL condition (exceeded lockable-page quota) - flagged in
 * vms-fb3 findings since that status value was not independently
 * cross-verified against a second source this session.
 */
uint32_t sys$lkwset(
    const void *inadr,
    void *retadr,
    uint32_t acmode
);

/**
 * sys$ulwset - Unlock pages from working set
 *
 * @param inadr   Address range to unlock (VA_RANGE - see va_rangedef.h)
 * @param retadr  Optional returned address range actually unlocked
 * @param acmode  Access mode (ignored)
 *
 * @return  SS$_NORMAL on success, SS$_BADPARAM if inadr is NULL
 *
 * Companion no-op to sys$lkwset above - see that comment for the
 * demand-paging rationale.
 */
uint32_t sys$ulwset(
    const void *inadr,
    void *retadr,
    uint32_t acmode
);

/* ================================================================
 * AST (Asynchronous System Trap) Services
 * ================================================================ */

/**
 * sys$dclast - Declare AST
 *
 * @param astadr  AST routine address
 * @param astprm  AST parameter
 * @param acmode  Access mode for the AST
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$dclast(
    void (*astadr)(uint32_t),
    uint32_t astprm,
    uint32_t acmode
);

/**
 * sys$setast - Enable or disable AST delivery
 *
 * @param enable  1 to enable, 0 to disable
 *
 * @return  SS$_WASSET if ASTs were enabled, SS$_WASCLR if disabled
 */
uint32_t sys$setast(uint32_t enable);

/**
 * sys$dclexh - Declare exit handler
 *
 * @param desblk  Pointer to exit handler control block
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$dclexh(void *desblk);

/* ================================================================
 * Lock Manager Services
 * ================================================================ */

/* Lock modes (LCK$K_xxx) — passed as the lkmode parameter. */
#define LCK$K_NLMODE  0  /* Null */
#define LCK$K_CRMODE  1  /* Concurrent Read */
#define LCK$K_CWMODE  2  /* Concurrent Write */
#define LCK$K_PRMODE  3  /* Protected Read */
#define LCK$K_PWMODE  4  /* Protected Write */
#define LCK$K_EXMODE  5  /* Exclusive */

/* Lock flags (LCK$M_xxx) — passed as the flags parameter to $ENQ/$ENQW/$DEQ.
 *
 * These are the real OpenVMS $LCKDEF bit values (source-of-truth: VMS
 * compatibility). They do NOT match our kernel lock manager's internal
 * LCK_M_xxx bitmask (src/kernel/vms_ioctl.h) — sys_lock.c translates the
 * public flags to the kernel layout at the /dev/vms boundary so kernel
 * numbering never leaks into this compat surface.
 *
 * Provenance: pinned against the OpenVMS VAX V7.3 reference oracle (lab-2
 * vaxlab-7) on 2026-08-10 by TWO independent documented-tool methods —
 * (A) LIBRARY/EXTRACT=$LCKDEF from SYS$LIBRARY:STARLET.MLB, and (B) a MACRO-32
 * `$LCKDEF GLOBAL` assembly whose symbol table was read with SEARCH. Both
 * agreed exactly on all 14 $ENQ flags. $LCKDEF is architecture-independent, so
 * a VAX oracle is authoritative. (These match the earlier FreeVMS lckdef.h
 * reproduction, github.com/ztmr/FreeVMS, now superseded as the citation.)
 * The full extract is quoted in <lckdef.h>, which carries the same layout.
 * See vms-982. */
#define LCK$M_VALBLK    0x0001  /* Lock has a 16-byte value block */
#define LCK$M_CONVERT   0x0002  /* Convert the existing lock named by lkid */
#define LCK$M_NOQUEUE   0x0004  /* Fail immediately (SS$_NOTQUEUED) if not grantable */
#define LCK$M_SYNCSTS   0x0008  /* Synchronous completion (return SS$_SYNCH) */
#define LCK$M_SYSTEM    0x0010  /* System-wide resource */
#define LCK$M_NOQUOTA   0x0020  /* Do not charge against enqueue quota */
#define LCK$M_CVTSYS    0x0040  /* Convert to a system lock */
#define LCK$M_RECOVER   0x0080  /* Recovery lock */
#define LCK$M_PROTECT   0x0100  /* Protected against forced dequeue */
#define LCK$M_NODLCKWT  0x0200  /* Exclude from deadlock-wait detection */
#define LCK$M_NODLCKBLK 0x0400  /* Exclude from deadlock-block detection */
#define LCK$M_EXPEDITE  0x0800  /* Expedite grant */
#define LCK$M_QUECVT    0x1000  /* Queued conversion */
#define LCK$M_BYPASS    0x2000  /* Bypass fast-path */

/**
 * sys$enq - Enqueue lock request
 *
 * @param efn        Event flag for completion
 * @param lkmode     Lock mode (NL, CR, CW, PR, PW, EX)
 * @param lksb       Pointer to lock status block
 * @param flags      Lock flags
 * @param resnam     Pointer to descriptor of resource name
 * @param parid      Parent lock ID (0 for root)
 * @param astadr     Optional AST completion routine
 * @param astprm     AST parameter
 * @param blkastadr  Optional blocking AST routine
 * @param acmode     Access mode
 * @param rsdm_id    Resource domain ID
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$enq(
    uint32_t efn,
    uint32_t lkmode,
    void *lksb,
    uint32_t flags,
    const struct dsc$descriptor_s *resnam,
    uint32_t parid,
    void (*astadr)(uint32_t),
    uint32_t astprm,
    void (*blkastadr)(uint32_t),
    uint32_t acmode,
    uint32_t rsdm_id
);

/**
 * sys$enqw - Enqueue lock request and wait
 */
uint32_t sys$enqw(
    uint32_t efn,
    uint32_t lkmode,
    void *lksb,
    uint32_t flags,
    const struct dsc$descriptor_s *resnam,
    uint32_t parid,
    void (*astadr)(uint32_t),
    uint32_t astprm,
    void (*blkastadr)(uint32_t),
    uint32_t acmode,
    uint32_t rsdm_id
);

/**
 * sys$deq - Dequeue lock
 *
 * @param lkid    Lock ID
 * @param valblk  Optional pointer to value block
 * @param acmode  Access mode
 * @param flags   Dequeue flags
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$deq(
    uint32_t lkid,
    void *valblk,
    uint32_t acmode,
    uint32_t flags
);

/* ================================================================
 * Security and Privilege Services
 * ================================================================ */

/**
 * sys$setprv - Set or clear process privileges
 *
 * @param enbflg  1 to enable, 0 to disable
 * @param prvadr  Pointer to quadword privilege mask
 * @param prmflg  1 for permanent change, 0 for temporary
 * @param prvprv  Optional pointer to receive previous privileges
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$setprv(
    uint32_t enbflg,
    const uint64_t *prvadr,
    uint32_t prmflg,
    uint64_t *prvprv
);

/**
 * sys$chkpro - Check protection
 *
 * @param objpro  Pointer to object protection block
 *
 * @return  SS$_NORMAL if access allowed
 */
uint32_t sys$chkpro(void *objpro);

/* ================================================================
 * RMS (Record Management Services) System Service Interface
 *
 * These are the SYS$ entry points for RMS operations.
 * The FAB and RAB structures are defined in fabdef.h and rabdef.h
 * respectively (passed as void* here since those headers may not
 * be included yet).
 *
 * Every RMS file/record service takes the VMS three-argument form
 *   SYS$xxx  cb ,[err] ,[suc]
 * (VSI OpenVMS Record Management Services Reference Manual, Part III):
 *   cb   — address of the control block (FAB for file ops, RAB for record
 *          ops), by reference.
 *   err  — address of the entry mask of an AST-level error completion
 *          routine (invoked with the control-block address on failure).
 *   suc  — address of the entry mask of an AST-level success completion
 *          routine (invoked with the control-block address on success).
 * Both err and suc are optional (pass 0/NULL for the common synchronous
 * call). OVMX RMS completes synchronously; when a completion routine is
 * supplied it is invoked before the service returns.
 * ================================================================ */

/** sys$open - Open existing file (cb=FAB) */
uint32_t sys$open(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$close - Close file (cb=FAB) */
uint32_t sys$close(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$create - Create new file (cb=FAB) */
uint32_t sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$erase - Erase (delete) file (cb=FAB) */
uint32_t sys$erase(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$parse - Parse file specification (cb=FAB with NAM/NAML) */
uint32_t sys$parse(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$search - Search for file, wildcard (cb=FAB after sys$parse) */
uint32_t sys$search(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$display - Display file attributes (cb=FAB) */
uint32_t sys$display(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$extend - Extend file allocation (cb=FAB) */
uint32_t sys$extend(void *fab, void (*err)(void *), void (*suc)(void *));

/** sys$connect - Connect record stream (cb=RAB) */
uint32_t sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$disconnect - Disconnect record stream (cb=RAB) */
uint32_t sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$get - Get (read) record (cb=RAB) */
uint32_t sys$get(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$put - Put (write) record (cb=RAB) */
uint32_t sys$put(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$update - Update record in place (cb=RAB) */
uint32_t sys$update(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$delete - Delete current record (cb=RAB) */
uint32_t sys$delete(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$find - Find record, position without reading (cb=RAB) */
uint32_t sys$find(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$rewind - Rewind record stream to beginning (cb=RAB) */
uint32_t sys$rewind(void *rab, void (*err)(void *), void (*suc)(void *));

/** sys$flush - Flush buffers to disk (cb=RAB) */
uint32_t sys$flush(void *rab, void (*err)(void *), void (*suc)(void *));

/* ================================================================
 * Formatted ASCII Output (FAO) Services
 * ================================================================ */

/**
 * sys$fao - Formatted ASCII output
 *
 * @param ctrstr  Pointer to descriptor of control string
 * @param outlen  Optional pointer to receive output length
 * @param outbuf  Pointer to descriptor of output buffer
 * @param ...     FAO directive arguments
 *
 * @return  SS$_NORMAL on success, SS$_BUFFEROVF if truncated
 */
uint32_t sys$fao(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    ...
);

/**
 * sys$faol - Formatted ASCII output with argument list
 *
 * @param ctrstr  Pointer to descriptor of control string
 * @param outlen  Optional pointer to receive output length
 * @param outbuf  Pointer to descriptor of output buffer
 * @param prmlst  Pointer to argument list (array of uint64_t)
 *
 * @return  SS$_NORMAL on success, SS$_BUFFEROVF if truncated
 */
uint32_t sys$faol(
    const struct dsc$descriptor_s *ctrstr,
    uint16_t *outlen,
    struct dsc$descriptor_s *outbuf,
    const uint64_t *prmlst
);

/* ================================================================
 * File Specification Scan (FILESCAN) Service
 * ================================================================ */

/**
 * sys$filescan - Scan a file specification string into its components
 *
 * Lightweight, purely lexical parse of a VMS file specification
 *
 *     node"access"::device:[root.][directory]name.type;version
 *
 * into the caller-requested component fields. Unlike sys$parse, $FILESCAN
 * performs NO RMS I/O, directory lookup, defaulting, or logical-name
 * translation — it only locates each syntactic field within srcstr and
 * returns its position and length.
 *
 * @param srcstr    Descriptor of the file specification string to scan.
 * @param valuelst  Item list (array of ILE2) naming the FSCN$_ components
 *                  of interest. For each requested item, $FILESCAN writes
 *                  the component length into ile2$w_length and a pointer to
 *                  the component (into srcstr's own storage) into
 *                  ile2$ps_bufaddr. Absent components return length 0 and a
 *                  NULL pointer. The list is terminated by an entry whose
 *                  ile2$w_code is 0.
 * @param fldflags  Optional (may be NULL). Receives a longword bit mask
 *                  (FSCN$M_*) with one bit set per component present in
 *                  srcstr.
 *
 * @return  SS$_NORMAL on a successful scan; SS$_BADPARAM if srcstr or
 *          valuelst is invalid or an unrecognized FSCN$_ item code appears
 *          in the list.
 */
uint32_t sys$filescan(
    const struct dsc$descriptor_s *srcstr,
    ILE2                          *valuelst,
    uint32_t                      *fldflags
);

/* ================================================================
 * Message Services
 * ================================================================ */

/**
 * sys$getmsg - Get message text for condition value
 *
 * @param msgid   Condition value (message ID)
 * @param msglen  Pointer to receive message length
 * @param bufadr  Pointer to descriptor of output buffer
 * @param flags   Message component flags (MSG$M_ bits)
 * @param outadr  Optional pointer to receive 4-byte result vector
 *
 * @return  SS$_NORMAL on success, SS$_MSGNOTFND if not found
 */
uint32_t sys$getmsg(
    uint32_t msgid,
    uint16_t *msglen,
    struct dsc$descriptor_s *bufadr,
    uint32_t flags,
    uint32_t *outadr
);

/**
 * sys$putmsg - Output message for condition value
 *
 * @param msgvec   Pointer to message vector
 * @param actrtn   Optional action routine (called for each line)
 * @param facnam   Optional facility name override descriptor
 * @param actprm   Optional action routine parameter
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$putmsg(
    const uint32_t *msgvec,
    uint32_t (*actrtn)(struct dsc$descriptor_s *, uint32_t),
    const struct dsc$descriptor_s *facnam,
    uint32_t actprm
);

/* ================================================================
 * Additional Process Management Services
 * ================================================================ */

/**
 * sys$forcex - Force image exit on another process
 *
 * @param pidadr  Optional pointer to target process ID
 * @param prcnam  Optional pointer to descriptor of process name
 * @param code    Exit status code to force
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$forcex(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam,
    uint32_t code
);

/**
 * sys$suspnd - Suspend a process (canonical VMS name)
 *
 * @param pidadr  Optional pointer to process ID
 * @param prcnam  Optional pointer to descriptor of process name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$suspnd(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam
);

/**
 * sys$suspend - Suspend a process (backwards-compatible alias for sys$suspnd)
 *
 * @param pidadr  Optional pointer to process ID
 * @param prcnam  Optional pointer to descriptor of process name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$suspend(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam
);

/**
 * sys$resume - Resume a suspended process
 *
 * @param pidadr  Optional pointer to process ID
 * @param prcnam  Optional pointer to descriptor of process name
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$resume(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam
);

/**
 * sys$setpri - Set process priority
 *
 * @param pidadr  Optional pointer to process ID
 * @param prcnam  Optional pointer to descriptor of process name
 * @param pri     New base priority
 * @param prvpri  Optional pointer to receive previous priority
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$setpri(
    const uint32_t *pidadr,
    const struct dsc$descriptor_s *prcnam,
    uint32_t pri,
    uint32_t *prvpri
);

/**
 * sys$cancel - Cancel I/O on channel
 *
 * @param chan  Channel number
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$cancel(uint16_t chan);

/* ================================================================
 * Operator Communication Services
 * ================================================================ */

/**
 * sys$sndopr - Send message to operator
 *
 * Sends a message to the OPCOM operator communication manager.
 * The message is written to the operator log file.
 *
 * @param msgbuf  Pointer to descriptor of OPCDEF message buffer
 * @param chan    Optional reply mailbox channel (0 = no reply)
 *
 * @return  SS$_NORMAL on success
 */
uint32_t sys$sndopr(
    const struct dsc$descriptor_s *msgbuf,
    uint16_t chan
);

/* ================================================================
 * Floating-Point Services
 * ================================================================ */

/**
 * sys$check_fen - Check Floating-point ENable status
 *
 * @param flags  Optional pointer to longword receiving FP bank flags
 *               (IA-64 only; set to 0 on other platforms)
 *
 * @return  1 if floating-point is enabled, 0 if disabled
 *          (Note: boolean return, not a VMS condition code)
 */
uint32_t sys$check_fen(uint32_t *flags);

/* ================================================================
 * User Authorization File Services
 * ================================================================ */

/**
 * sys$getuai - Get User Authorization Information
 *
 * Retrieves user account information from /etc/ovmx/sysuaf.dat
 * via an item list of UAI$_ codes.
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param context  Context pointer for iterating (pass NULL)
 * @param usrnam   Pointer to descriptor of username to look up
 * @param itmlst   Pointer to item list of UAI$_ codes
 * @param iosb     Pointer to I/O status block (may be NULL)
 * @param astadr   Optional AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 *
 * @return  SS$_NORMAL on success, SS$_NOSUCHID if user not found
 */
uint32_t sys$getuai(
    uint32_t efn,
    uint32_t *context,
    struct dsc$descriptor_s *usrnam,
    void *itmlst,
    struct _iosb *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/**
 * sys$setuai - Set User Authorization Information
 *
 * Updates user account fields in /etc/ovmx/sysuaf.dat.
 * Requires SYSPRV privilege.
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param context  Context pointer (pass NULL)
 * @param usrnam   Pointer to descriptor of username
 * @param itmlst   Pointer to item list of UAI$_ codes to update
 * @param iosb     Pointer to I/O status block (may be NULL)
 * @param astadr   Optional AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 *
 * @return  SS$_NORMAL on success, SS$_NOPRIV if no SYSPRV privilege
 */
uint32_t sys$setuai(
    uint32_t efn,
    uint32_t *context,
    struct dsc$descriptor_s *usrnam,
    void *itmlst,
    struct _iosb *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/* ================================================================
 * Device Information Services
 * ================================================================ */

/**
 * sys$getdvi - Get Device/Volume Information
 *
 * Retrieves device and volume attributes via an item list of DVI$_ codes.
 * Identifies the device either by channel number (chan) or name (devnam);
 * devnam takes priority.
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param chan     I/O channel number (0 if using devnam)
 * @param devnam   Pointer to descriptor of device name (NULL if using chan)
 * @param itmlst   Pointer to item list of DVI$_ codes
 * @param iosb     Pointer to I/O status block (may be NULL)
 * @param astadr   Optional AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 * @param nullarg  Reserved, pass 0
 *
 * @return  SS$_NORMAL on success, SS$_NOSUCHDEV if device not found
 */
uint32_t sys$getdvi(
    uint32_t efn,
    uint16_t chan,
    struct dsc$descriptor_s *devnam,
    void *itmlst,
    struct _iosb *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm,
    uint32_t nullarg
);

/**
 * sys$getdviw - Get Device/Volume Information (synchronous wait)
 *
 * Identical to sys$getdvi — our implementation is always synchronous.
 */
uint32_t sys$getdviw(
    uint32_t efn,
    uint16_t chan,
    struct dsc$descriptor_s *devnam,
    void *itmlst,
    struct _iosb *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm,
    uint32_t nullarg
);

/**
 * sys$device_scan - Scan for devices matching a wildcard name/item filter
 *
 * Returns device names one at a time on successive calls, each call
 * continuing from where the last left off via ctx. Matching is against
 * the (VMS-wildcard, "*"/"%") devnam pattern and, if itmlst is supplied,
 * further filtered by item codes such as DVS$_DEVCLASS (see dvsdef.h).
 *
 * @param devnam    Descriptor to receive the next matching device name
 * @param devnamlen Pointer to receive the length written to devnam
 * @param wildnam   Optional descriptor of a wildcard device-name pattern
 *                  (NULL/empty matches all devices)
 * @param itmlst    Optional item list of DVS$_ filter codes. Declared as
 *                  void* (rather than a specific item-list struct type)
 *                  because corpus call sites pass either ILE3 (iledef.h)
 *                  or struct item_list_3 (lnmdef.h) arrays interchangeably
 *                  — the two are field-for-field layout compatible, same
 *                  convention already used by sys$getdvi/sys$getuai above.
 * @param ctx       Context for continuing the scan across calls; the
 *                  caller zero-initializes it before the first call
 *
 * @return  SS$_NORMAL with a device name in devnam,
 *          SS$_NOSUCHDEV if no device ever matched the filter,
 *          SS$_NOMOREDEV if the scan reached the end of the device list
 *          (see the file-header PROVENANCE flag on SS$_NOMOREDEV's
 *          numeric value in ssdef.h — sourced this session but not
 *          independently cross-verified; flagged for operator sign-off
 *          per vms-fb3 findings)
 */
uint32_t sys$device_scan(
    struct dsc$descriptor_s *devnam,
    uint16_t *devnamlen,
    const struct dsc$descriptor_s *wildnam,
    void *itmlst,
    GENERIC_64 *ctx
);

/* ================================================================
 * Operator Communication Services
 * ================================================================ */

/**
 * sys$brkthruw - Broadcast message to terminal(s)
 *
 * Sends a broadcast message to the specified terminal device.
 * If sendto is NULL or empty, broadcasts to the current terminal (TT:).
 *
 * @param efn      Event flag (ignored — synchronous)
 * @param msgbuf   Pointer to descriptor of message to broadcast
 * @param sendto   Pointer to descriptor of target terminal device name
 * @param sndtyp   Send type flags (ignored)
 * @param iosb     Pointer to I/O status block (may be NULL)
 * @param astadr   Optional AST completion routine (ignored)
 * @param astprm   AST parameter (ignored)
 *
 * @return  SS$_NORMAL on success, SS$_NOSUCHDEV if terminal not found
 */
uint32_t sys$brkthruw(
    uint32_t efn,
    struct dsc$descriptor_s *msgbuf,
    struct dsc$descriptor_s *sendto,
    uint32_t sndtyp,
    struct _iosb *iosb,
    void (*astadr)(uint32_t),
    uint32_t astprm
);

/* ================================================================
 * Condition Handling Services
 * ================================================================ */

/**
 * sys$setexv - Set (or clear) a software exception vector (vms-2e72).
 *
 * Establishes one of the three per-access-mode software exception
 * vectors the OpenVMS condition dispatcher consults around the
 * call-frame handler search (see chfdef.h for the CHF$K_*_VECTOR
 * selectors and docs/design-chf-condition-handling.md for the search
 * order). A vectored handler has the same prototype as a frame handler
 * (uint32_t h(struct chf$signal_array *, struct chf$mech_array *)) and
 * returns SS$_CONTINUE / SS$_RESIGNAL identically.
 *
 * @param vector  CHF$K_PRIMARY_VECTOR / _SECONDARY_ / _LAST_CHANCE_.
 * @param addres  New handler address, or NULL to clear the vector.
 * @param acmode  Access mode (validated; single-mode userspace model).
 * @param prvhnd  Optional out; receives the previously established
 *                handler for this vector (NULL if none).
 *
 * @return SS$_NORMAL, or SS$_BADPARAM for an out-of-range selector.
 */
uint32_t sys$setexv(
    uint32_t vector,
    void *addres,
    uint32_t acmode,
    void **prvhnd
);

/**
 * sys$unwind - Unwind the call stack, invoking intervening handlers
 *
 * Called from within a condition handler (see lib$establish/lib$signal
 * in lib_signal.c) to abandon signal delivery and transfer control back
 * to an earlier call frame instead of returning normally to the point
 * where the condition was signaled.
 *
 * rung-2 (vms-8802): a real machine-frame-transfer unwind. As on VMS the
 * transfer is DEFERRED - sys$unwind records the request and returns to the
 * handler; the CHF dispatcher (lib_signal.c) performs it when the handler
 * returns, calling each intervening handler once with CHF$V_UNWINDING set
 * and transferring control (setjmp/longjmp) to the target frame's armed
 * resume anchor (VMS$UNWIND_ANCHOR, chfdef.h), abandoning the intervening
 * machine frames and honouring newpc.
 *
 * Compatibility: a NULL depadr, a target frame that armed no anchor, or a
 * call made outside an active dispatch keeps the historical pop-only
 * handler-chain contract (rung-1 / test_lib_fb3). Resuming into an
 * un-anchored ancestor frame (the real Alpha invocation-context walk) is
 * rung-3 (vms-1fa).
 *
 * @param depadr  Optional pointer to the target call depth (as obtained
 *                from a chf$mech_array's chf$is_mch_depth field - see
 *                chfdef.h). NULL means "unwind one level" (pop the
 *                currently executing handler and return to the frame
 *                that established it); this form never transfers.
 * @param newpc   Resume PC selector honoured on a frame transfer; readable
 *                at the resume site via vms$$unwind_newpc(). 0 resumes at
 *                the target frame's armed anchor with no selector.
 *
 * @return  SS$_NORMAL
 */
uint32_t sys$unwind(
    const uint32_t *depadr,
    void *newpc
);

#ifdef __cplusplus
}
#endif

#endif /* __STARLET_H */
