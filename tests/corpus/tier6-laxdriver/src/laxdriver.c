#pragma module LAXDRIVER "X-1"
/*
 * System load average extended driver (LAX0:), returning fixed-point
 * values as 32-bit unsigned ints, with a 14-bit scaling factor.
 * This supports a result range from 0.000 to over 262143.999.
 *
 * Copyright 2022, Jake Hamby.
 * MIT License.
 *
 * Some code comments are from the example device driver in sys$examples:
 * that this driver used as a template.
 */

/* Define system data structure types and constants */

#define __NEW_STARLET 1

#include <bufiodef.h>		/* Define the packet header for a system */
				/*   buffer for buffered I/O data */
#include <ccbdef.h>             /* Channel control block */
#include <cpudef.h>             /* Per-CPU data definition */
#include <crbdef.h>             /* Controller request block */
#include <dcdef.h>              /* Device codes */
#include <ddbdef.h>             /* Device data block */
#include <ddtdef.h>             /* Driver dispatch table */
#include <devdef.h>             /* Device characteristics */
#include <dptdef.h>             /* Driver prologue table */
#include <dyndef.h>             /* Data structure type definitions */
#include <fdtdef.h>             /* Function decision table */
#include <idbdef.h>             /* Interrupt data block */
#include <iocdef.h>             /* IOC constants */
#include <iodef.h>              /* I/O function codes */
#include <irpdef.h>             /* I/O request packet */
#include <orbdef.h>             /* Object rights block */
#include <pcbdef.h>             /* Process control block */
#include <prvdef.h>             /* Privilege bits */
#include <ssdef.h>              /* System service status codes */
#include <stsdef.h>             /* Status value fields */
#include <tqedef.h>             /* Timer queue entry fields */
#include <ucbdef.h>             /* Unit control block */
#include <aaa_system_cells.h>	/* Global data references */

/* Define function prototypes for system routines */

#include <exe_routines.h>       /* Prototypes for exe$ and exe_std$ routines */
#include <ioc_routines.h>       /* Prototypes for ioc$ and ioc_std$ routines */
#include <sch_routines.h>       /* Prototypes for sch$ and sch_std$ routines */

/* Define various device driver macros */

#include <vms_drivers.h>        /* Device driver support macros, including */
                                /* table initialization macros and prototypes */
#include <vms_macros.h>         /* Additional macros */

/* Define the DEC C functions used by this driver */

#include <string.h>             /* String routines provided by "kernel CRTL" */
#include <stdint.h>		/* C99 typedefs */
#include <stdbool.h>		/* C99 bool type */

/* Define the fixed-point scaling factors. Update the constants if you change them. */

#define FX_SCALE	14		/* scaling factor for stored results */
#define FX_LSHIFT   (FX_SCALE - 8)	/* coefficients will shift 8 extra bits */
#define FX_RSHIFT   (FX_SCALE + 10)	/* right-shift after multiply and add */

/* Number of realtime priority levels to test and eliminate (as an optimization).
 * OpenVMS V7.0 tests bits 0-46 with a mask, skipping to bit 47, corresponding to
 * external priority 16 (swapper), which is the highest priority typically used,
 * if none of the bits in the mask are set. Do the same thing in this program.
 * If you use priorities 17 or higher, reduce this accordingly (subtract from 63).
 */

#define FIRST_LIKELY_PRIO	47
#define RT_PRIO_MASK		((1ULL << FIRST_LIKELY_PRIO) - 1)

/* Define Device-Dependent Unit Control Block with extensions for LAX device */

typedef struct {
    UCB		ucb$r_ucb;		/* Generic UCB */
    uint32_t	ucb$fx_avgs[9];		/* fixed-point data to return on reads */
    bool	ucb$b_is_stopping;	/* user request to stop pending */
    bool	ucb$b_is_stopped;	/* stats update is currently stopped */
    TQE		ucb$l_tqe;		/* timer tick (1 Hz) */
} LAX_UCB;

/* Define const references to the global data we need to reference */

/* System time */
extern uint64_t	exe$gq_systime;

/* Bit vectors of which priority queues are active (bit 0 = priority 63) */
extern uint64_t	sch$gq_comqs;	/* computable queue */
extern uint64_t	sch$gq_comoqs;	/* computable (outswapped) queue */

/* Array of queue heads and tails for each priority, from 63 to 0 */
extern KTB* sch$aq_comh[];	/* COM queue heads and tails */
extern KTB* sch$aq_comoh[];	/* COMO queue heads and tails */

/* Queue heads for the three wait states we care about */
extern KTB* sch$gq_colpgwq;	/* collided page wait queue */
extern KTB* sch$gq_pfwq;	/* page fault wait queue */
extern KTB* sch$gq_fpgwq;	/* free page wait queue */

/* TODO: Use CBB functions instead of bitmasks to support > 64 CPUs */

extern uint32_t	smp$gl_max_cpuid;   /* max CPU ID for bitmasks */
extern uint64_t	smp$gq_active_set;  /* active CPU set bitmask */
extern uint64_t	sch$gq_idle_cpus;   /* idle CPU bitmask */

extern CPU* smp$gl_cpu_data[];  /* CPU data pointer array */

extern MUTEX ioc$gq_mutex;	/* mutex for IOC database */

/* Driver table initialization routine */

int  driver$init_tables (void);

/* Device I/O database structure initialization routine */

static void lax_struc_init (CRB *crb, DDB *ddb, IDB *idb, ORB *orb, LAX_UCB *ucb);

/* Device I/O database structure re-initialization routine */

static void lax_struc_reinit (CRB *crb, DDB *ddb, IDB *idb, ORB *orb, LAX_UCB *ucb);

/* Unit initialization routine */

static int  lax_unit_init (IDB *idb, LAX_UCB *ucb);

/* FDT routine for read functions */

static int  lax_read (IRP *irp, PCB *pcb, LAX_UCB *ucb, CCB *ccb);

/* FDT routine for write functions */

static int  lax_write (IRP *irp, PCB *pcb, LAX_UCB *ucb, CCB *ccb);

/* Periodic load averages update via timer queue entry */

static void lax_stats_update_int (void *fr3, LAX_UCB *ucb, TQE *tqe);

/*
 * DRIVER$INIT_TABLES - Initialize Driver Tables
 *
 * Functional description:
 *
 *   This routine completes the initialization of the DPT, DDT, and FDT
 *   structures.  If a driver image contains a routine named DRIVER$INIT_TABLES
 *   then this routine is called once by the $LOAD_DRIVER service immediately
 *   after the driver image is loaded or reloaded and before any validity checks
 *   are performed on the DPT, DDT, and FDT.  A prototype version of these
 *   structures is built into this image at link time from the
 *   VMS$VOLATILE_PRIVATE_INTERFACES.OLB library.  Note that the device related
 *   data structures (e.g. DDB, UCB, etc.) have not yet been created when this
 *   routine is called.  Thus the actions of this routine must be confined to
 *   the initialization of the DPT, DDT, and FDT structures which are contained
 *   in the driver image.
 *
 *   TODO: switch to VAX MACRO-based table initialization to reduce code size.
 *
 * Calling convention:
 *
 *   status = driver$init_tables ();
 *
 * Input parameters:
 *
 *   None.
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   status     If the status is not successful, then the driver image will
 *              be unloaded.  Note that the ini_* macros used below will
 *              result in a return from this routine with an error status if
 *              an initialization error is detected.
 *
 * Implicit inputs:
 *
 *  driver$dpt, driver$ddt, driver$fdt
 *              These are the externally defined names for the prototype
 *              DPT, DDT, and FDT structures that are linked into this driver.
 *
 * Environment:
 * 
 *   Kernel mode, system context.
 */
 

int driver$init_tables (void)  {

    /* Prototype driver DPT, DDT, and FDT will be pulled in from the
     * VMS$VOLATILE_PRIVATE_INTERFACES.OLB library at link time.
     */
    extern DPT driver$dpt;
    extern DDT driver$ddt;
    extern FDT driver$fdt;

    /* Finish initialization of the Driver Prologue Table (DPT) */

    ini_dpt_name        (&driver$dpt, "LAXDRIVER");
    ini_dpt_adapt       (&driver$dpt, AT$_NULL); /* software adapter */
    ini_dpt_defunits    (&driver$dpt, 1);
    ini_dpt_maxunits    (&driver$dpt, 1);
    ini_dpt_ucbsize     (&driver$dpt, sizeof(LAX_UCB));
    ini_dpt_struc_init  (&driver$dpt, lax_struc_init );
    ini_dpt_struc_reinit(&driver$dpt, lax_struc_reinit );
    ini_dpt_end         (&driver$dpt);

    /* Finish initialization of the Driver Dispatch Table (DDT) */

    ini_ddt_unitinit    (&driver$ddt, lax_unit_init);
    ini_ddt_cancel      (&driver$ddt, ioc_std$cancelio);
    ini_ddt_end         (&driver$ddt);

    /* Finish initialization of the Function Decision Table (FDT)   */
    /*								    */
    /* The BUFFERED_64 indicates that this driver supports a 64-bit */
    /* virtual address in the QIO P1 parameter for that function.   */
    /* This driver, therefore, supports 64-bit user buffers in all  */
    /* of its I/O functions.					    */

    ini_fdt_act (&driver$fdt, IO$_READLBLK, lax_read, BUFFERED_64);
    ini_fdt_act (&driver$fdt, IO$_READPBLK, lax_read, BUFFERED_64);
    ini_fdt_act (&driver$fdt, IO$_READVBLK, lax_read, BUFFERED_64);

    ini_fdt_act (&driver$fdt, IO$_WRITELBLK, lax_write, BUFFERED_64);
    ini_fdt_act (&driver$fdt, IO$_WRITEPBLK, lax_write, BUFFERED_64);
    ini_fdt_act (&driver$fdt, IO$_WRITEVBLK, lax_write, BUFFERED_64);

    ini_fdt_end (&driver$fdt);

    /* If we got this far then everything worked, so return success. */

    return SS$_NORMAL;
}


/*
 * LAX_STRUC_INIT - Device Data Structure Initialization Routine
 *
 * Functional description:
 *
 *   This routine is called once for each unit by the $LOAD_DRIVER service
 *   after that UCB is created.  At the point of this call the UCB has not
 *   yet been fully linked into the I/O database.  This routine is responsible
 *   for filling in driver specific fields that in the I/O database structures
 *   that are passed as parameters to this routine.
 *
 *   This routine is responsible for filling in the fields that are not
 *   affected by a RELOAD of the driver image.  In contrast, the structure
 *   reinitialization routine is responsible for filling in the fields that
 *   need to be corrected when (and if) this driver image is reloaded.
 *
 *   After this routine is called for a new unit, then the reinitialization
 *   routine is called as well.  Then the $LOAD_DRIVER service completes the
 *   integration of these device specific structures into the I/O database.
 *
 *   Note that this routine must confine its actions to filling in these I/O
 *   database structures and may not attempt to initialize the hardware device.
 *   Initialization of the hardware device is the responsibility of the 
 *   controller and unit initialization routines which are called some time
 *   later.
 *
 * Calling convention:
 *
 *   lax_struc_init (crb, ddb, idb, orb, ucb)
 *
 * Input parameters:
 *
 *   crb        Pointer to associated controller request block.
 *   ddb        Pointer to associated device data block.
 *   idb        Pointer to associated interrupt dispatch block.
 *   orb        Pointer to associated object rights block.
 *   ucb        Pointer to the unit control block that is to be initialized.
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   None.
 *
 * Environment:
 * 
 *   Kernel mode, system context, IPL may be as high as 31 and may not be
 *   altered.
 *
 */ 

static void lax_struc_init (CRB *crb, DDB *ddb, IDB *idb, ORB *orb, LAX_UCB *ucb) {

    /* Initialize the fork lock and device IPL fields */

    ucb->ucb$r_ucb.ucb$b_flck = SPL$C_IOLOCK8;
    ucb->ucb$r_ucb.ucb$b_dipl = IOC$K_DEVICE_IPL;

    /* Device characteristics: Record oriented (REC), Available (AVL), 
     *	Input device (IDV), Shared (SHR)
     */
    ucb->ucb$r_ucb.ucb$l_devchar = DEV$M_REC | DEV$M_AVL | DEV$M_IDV | DEV$M_SHR;

    /* Set to prefix device name with "node$", set device class, device type,
     *  and default buffer size.
     */
    ucb->ucb$r_ucb.ucb$l_devchar2 = DEV$M_NNM;
    ucb->ucb$r_ucb.ucb$b_devclass = DC$_MISC;
    /* ucb->ucb$r_ucb.ucb$b_devtype = LP$_LP11; */  /* we have no device type */
    ucb->ucb$r_ucb.ucb$w_devbufsiz = sizeof(ucb->ucb$fx_avgs);   /* 36 byte buffer */

    /* set up our 1 Hz TQE */
    ucb->ucb$l_tqe.tqe$w_size = TQE$C_LENGTH;
    ucb->ucb$l_tqe.tqe$b_type = DYN$C_TQE;
    ucb->ucb$l_tqe.tqe$b_rqtype = TQE$C_SSREPT;
    ucb->ucb$l_tqe.tqe$q_fr3 = 0;
    ucb->ucb$l_tqe.tqe$q_fr4 = (__int64) ucb;
    ucb->ucb$l_tqe.tqe$l_fpc = (int) lax_stats_update_int;
    ucb->ucb$l_tqe.tqe$q_delta = 10000000;  /* VMS time uses 100 ns units */
}


/*
 * LAX_STRUC_REINIT - Device Data Structure Re-Initialization Routine
 *
 * Functional description:
 *
 *   This routine is called once for each unit by the $LOAD_DRIVER service
 *   immediately after the structure initialization routine is called.
 *
 *   Additionally, this routine is called once for each unit by the $LOAD_DRIVER
 *   service when a driver image is RELOADED.  Thus, this routine is
 *   responsible for filling in the fields in the I/O database structures
 *   that point into this driver image.
 *
 *   Note that this routine must confine its actions to filling in these I/O
 *   database structures.
 *
 * Calling convention:
 *
 *   lax_struc_reinit (crb, ddb, idb, orb, ucb)
 *
 * Input parameters:
 *
 *   crb        Pointer to associated controller request block.
 *   ddb        Pointer to associated device data block.
 *   idb        Pointer to associated interrupt dispatch block.
 *   orb        Pointer to associated object rights block.
 *   ucb        Pointer to the unit control block that is to be initialized.
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   None.
 *
 * Environment:
 * 
 *   Kernel mode, system context, IPL may be as high as 31 and may not be
 *   altered.
 *
 */ 

static void lax_struc_reinit (CRB *crb, DDB *ddb, IDB *idb, ORB *orb, LAX_UCB *ucb) {
    
    extern DDT driver$ddt;

    /* Setup the pointer from our DDB in the I/O database to the driver
     * dispatch table that's within this driver image.
     */
    ddb->ddb$ps_ddt = &driver$ddt;

    /* Setup the procedure descriptor and code entry addresses in the VEC
     * portion of the CRB in the I/O database to point to the interrupt
     * service routine that's within this driver image.
     */
    /* dpt_store_isr (crb, lax_interrupt); */
}


/*
 * LAX_UNIT_INIT - Unit Initialization Routine
 *
 * Functional description:
 *
 *   This routine is called once for each unit by the $LOAD_DRIVER service
 *   after a new unit control block has been created, initialized, and
 *   fully integrated into the I/O database.
 *
 *   This routine is also called for each unit during power fail recovery.
 *
 *   It is the responsibility of this routine to bring unit "on line" and
 *   to make it ready to accept I/O requests.
 *
 * Calling convention:
 *
 *   status = lax_unit_init (idb, ucb)
 *
 * Input parameters:
 *
 *   idb        Pointer to associated interrupt dispatch block.
 *   ucb        Pointer to the unit control block that is to be initialized.
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   status     SS$_NORMAL indicates that the unit was initialized successfully.
 *              SS$_IVADDR indicates that an unexpected ISA I/O address or IRQ
 *                         level was detected.
 *
 * Environment:
 * 
 *   Kernel mode, system context, IPL 31.
 */
 
static int lax_unit_init (IDB *idb, LAX_UCB *ucb)  {

#if defined DEBUG
    /* If a debug version of this driver is being built then invoke the loaded
     * system debugger.  This could either be the High Level Language System
     * Debugger, XDELTA, or nothing.
     */
    {
        extern void ini$brk (void);
        ini$brk ();
    }
#endif

    /* Set device initially offline (for error exits) and initialize other
     * UCB fields.
     */
    ucb->ucb$r_ucb.ucb$v_online = 0;

    /* Clear the stats array and priority mask. */

    memset(&(ucb->ucb$fx_avgs), 0, sizeof(ucb->ucb$fx_avgs));
    ucb->ucb$b_is_stopping = false;
    ucb->ucb$b_is_stopped = false;

    /* This driver can service only a single unit per DDB and IDB.  Thus,
     * make the single unit the permanent owner of the IDB.  This facilitates
     * getting the UCB address in our interrupt service routine.
     * We don't have an ISR, but if we did, that might be useful.
     */
    idb->idb$ps_owner = &(ucb->ucb$r_ucb); 

    /* Start the timer */
    uint64_t tick_time = exe$gq_systime + ucb->ucb$l_tqe.tqe$q_delta;
    exe_std$instimq((int) (tick_time & 0xffffffff),
		    (int) ((tick_time >> 32) & 0xffffffff),
		    (TQE *) &ucb->ucb$l_tqe);

    /* Mark the device as online and ready to accept I/O requests */
    ucb->ucb$r_ucb.ucb$v_online = 1;

    return SS$_NORMAL;
}


/*
 * LAX_READ - FDT Routine for Read Function Codes 
 *
 * Functional description:
 *
 *   Verifies the read arguments, then copies as much data as requested.
 *
 *   Since this is an upper-level FDT routine, this routine always returns
 *   the SS$_FDT_COMPL status.  The $QIO status that is to be returned to
 *   the caller of the $QIO system service is returned indirectly by the
 *   FDT completion routines (e. g. exe_std$abortio, exe_std$qiodrvpkt) via
 *   the FDT context structure.
 *
 * Calling convention:
 *
 *   status = lax_read (irp, pcb, ucb, ccb)
 *
 * Input parameters:
 *
 *   irp        Pointer to I/O request packet
 *   pcb        Pointer process control block
 *   ucb        Pointer to unit control block
 *   ccb        Pointer to channel control block
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   status     SS$_FDT_COMPL
 *
 * Environment:
 * 
 *   Kernel mode, user process context, IPL 2.
 */

static int lax_read (IRP *irp, PCB *pcb, LAX_UCB *ucb, CCB *ccb) {

    /* Get the pointer to the caller's buffer and the size of the caller's
     * buffer from the $QIO P1 and P2 parameters respectively.  The caller's
     * buffer is treated as a 64-bit address although it may be a 32-bit 
     * address.
     */
    CHAR_PQ qio_bufp = (CHAR_PQ)irp->irp$q_qio_p1;

    /* Return an SS$_BADPARAM error if the read size is too small. */
    if (irp->irp$l_qio_p2 < sizeof(uint32_t)) {
	return ( call_abortio (irp, pcb, (UCB *)ucb, SS$_BADPARAM) );
    }

    /* Truncate the read size to 36 bytes if needed. */
    if (irp->irp$l_qio_p2 > sizeof(ucb->ucb$fx_avgs)) {
	irp->irp$l_qio_p2 = sizeof(ucb->ucb$fx_avgs);
    }

    int qio_buflen = irp->irp$l_qio_p2;

    /* Assure that the caller has write access to this buffer to do a read
     * operation.  If not, exe_std$readchk will abort the I/O request and
     * return the SS$_FDT_COMPL warning status.  If this is the case, we must
     * return back to the FDT dispatcher in the $QIO system service.
     */
    if (qio_buflen != 0) {
        int status = exe_std$readchk (irp, pcb, &(ucb->ucb$r_ucb), 
                                      qio_bufp, qio_buflen);
        if ( ! $VMS_STATUS_SUCCESS(status) ) return status;

	memcpy( qio_bufp, &(ucb->ucb$fx_avgs), qio_buflen );
    }

    return ( call_finishio (irp, (UCB *)ucb, SS$_NORMAL, 0) );
}

/*
 * LAX_WRITE - FDT Routine for Write Function Codes 
 *
 * Functional description:
 *
 *   The original LAVDRIVER behavior for writes is to update a mask to
 *   disable specific priorities from being counted. I suspect nobody has
 *   ever used that feature. So this version repurposes writes to set
 *   a single boolean flag to 1 to stop the load average update timer
 *   callback (e.g. for benchmarking), or to 0 to restart the load average
 *   service. Additional bits/bytes are ignored and should be set to zero.
 *
 *   Since this is an upper-level FDT routine, this routine always returns
 *   the SS$_FDT_COMPL status.  The $QIO status that is to be returned to
 *   the caller of the $QIO system service is returned indirectly by the
 *   FDT completion routines (e. g. exe_std$abortio, exe_std$qiodrvpkt) via
 *   the FDT context structure.
 *
 * Calling convention:
 *
 *   status = lax_write (irp, pcb, ucb, ccb)
 *
 * Input parameters:
 *
 *   irp        Pointer to I/O request packet
 *   pcb        Pointer process control block
 *   ucb        Pointer to unit control block
 *   ccb        Pointer to channel control block
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   status     SS$_FDT_COMPL
 *
 * Environment:
 * 
 *   Kernel mode, user process context, IPL 2.
 */

static int lax_write (IRP *irp, PCB *pcb, LAX_UCB *ucb, CCB *ccb) {

    /* Check if the caller has the required CMKRNL privilege. */
    if ((pcb->pcb$q_priv & PRV$M_CMKRNL) == 0)
        return ( call_abortio (irp, pcb, (UCB *)ucb, SS$_NOPRIV) );

    /* Get the pointer to the caller's buffer and the size of the caller's
     * buffer from the $QIO P1 and P2 parameters respectively.  The caller's
     * buffer is treated as a 64-bit address although it may be a 32-bit 
     * address.
     */
    CHAR_PQ qio_bufp = (CHAR_PQ)irp->irp$q_qio_p1;
    int qio_buflen = irp->irp$l_qio_p2;

    /* Return an SS$_BADPARAM error if the write size is too small. */
    if (qio_buflen < sizeof(bool)) {
	return ( call_abortio (irp, pcb, (UCB *)ucb, SS$_BADPARAM) );
    }

    /* Assure that the caller has read access to this buffer to do a write
     * operation.  If not, exe_std$writechk will abort the I/O request and
     * return the SS$_FDT_COMPL warning status.  If this is the case, we must
     * return back to the FDT dispatcher in the $QIO system service.
     */
    if (qio_buflen != 0) {
        int status = exe_std$writechk (irp, pcb, &(ucb->ucb$r_ucb), 
                                   qio_bufp, qio_buflen);
        if ( ! $VMS_STATUS_SUCCESS(status) ) return status;

	/* True = stop updating; false = start updating */
	bool stop_request = (bool)(*qio_bufp);

	if (stop_request && !(ucb->ucb$b_is_stopped)) {
	    /* it's okay if this is already set */
	    ucb->ucb$b_is_stopping = true;
	} else if (!stop_request && ucb->ucb$b_is_stopped) {
	    /* Note: ucb$b_is_stopping should already be false */
	    ucb->ucb$b_is_stopped = false;

	    /* Restart the timer */
	    ucb->ucb$l_tqe.tqe$b_rqtype = TQE$C_SSREPT;
	    uint64_t tick_time = exe$gq_systime + ucb->ucb$l_tqe.tqe$q_delta;
	    exe_std$instimq((int) (tick_time & 0xffffffff),
			    (int) ((tick_time >> 32) & 0xffffffff),
			    (TQE *) &ucb->ucb$l_tqe);
 	}
    }

    return ( call_finishio (irp, (UCB *)ucb, SS$_NORMAL, 0) );
}

/* Define some constants for the load averaging multiplication factors.
 * I used Python to compute the exponential decays for fixed-point math.
 * The multipliers for the new values have an extra 8 bits of fraction.
 * This is subtracted from the 14 bits that the integers are shifted,
 * so that the old and new coefficients will both have 24 fraction bits.
 */

/* old: 1/exp(1s/60s) * (1<<24) */
/* new: (1 - 1/exp(1s/60s)) * (1<<32) */
static const uint32_t old_lav_1min = 16499913;
static const uint32_t new_lav_1min = 70989565;

/* old: 1/exp(1s/300s) * (1<<24) */
/* new: (1 - 1/exp(1s/300s)) * (1<<32) */
static const uint32_t old_lav_5min = 16721385;
static const uint32_t new_lav_5min = 14292723;

/* old: 1/exp(1s/900s) * (1<<24) */
/* new: (1 - 1/exp(1s/900s)) * (1<<32) */
static const uint32_t old_lav_15min = 16758585;
static const uint32_t new_lav_15min = 4769536;

/*
 * LAX_STATS_UPDATE_INT - Periodic update of load averages
 *
 * Functional description:
 *
 *   This routine performs a once-a-second update of the load average data.
 *   The algorithm is the same as the original LAXDRIVER, except that the
 *   system load average is not divided by the number of active CPUs, and
 *   the values are all returned as 32-bit unsigned integers representing
 *   fixed-point values with a binary scaling factor of 14 bits.
 *
 * Calling convention:
 *
 *   lax_stats_update_int (fr3, ucb, tqe)
 *
 * Input parameters:
 *
 *   fr3	Unused
 *   ucb        Pointer to unit control block
 *   tqe        Pointer to TQE used to time request
 *
 * Output parameters:
 *
 *   None.
 *
 * Return value:
 *
 *   None.
 *
 * Environment:
 * 
 *   Kernel mode, system context, at timer IPL.
 */

static void lax_stats_update_int (void *fr3, LAX_UCB *ucb, TQE *tqe) {
    /* acquire the SCHED spinlock, so we can count runnable processes. */
    int orig_ipl;
    sys_lock (SCHED, RAISE_IPL, &orig_ipl);

    uint32_t proc_count = 0;

    /* Traverse COM and COMO queues for each priority that's in use. */

    if (sch$gq_comqs) {
	bool has_realtime = (sch$gq_comqs & RT_PRIO_MASK);
	int startbit = has_realtime ? 0 : FIRST_LIKELY_PRIO;
	uint64_t testmask = (1ULL << startbit);

	for (int idx = startbit; idx < 64; idx++, testmask <<= 1) {
	    if (sch$gq_comqs & testmask) {
		KTB* head = sch$aq_comh[idx << 1];
		KTB* ktb = head;
		while ((ktb = ktb->ktb$l_sqfl) != head) {
		    proc_count++;
		}
	    }
	}
    }

    /* now do the COMO queue */

    if (sch$gq_comoqs) {
	bool has_realtime = (sch$gq_comoqs & RT_PRIO_MASK);
	int startbit = has_realtime ? 0 : FIRST_LIKELY_PRIO;
	uint64_t testmask = (1ULL << startbit);

	for (int idx = startbit; idx < 64; idx++, testmask <<= 1) {
	    if (sch$gq_comoqs & testmask) {
		KTB* head = sch$aq_comoh[idx << 1];
		KTB* ktb = head;
		while ((ktb = ktb->ktb$l_sqfl) != head) {
		    proc_count++;
		}
	    }
	}
    }

    /* add processes from the three page-fault-related wait queues */

    KTB* ktb = sch$gq_colpgwq;
    while ((ktb = ktb->ktb$l_sqfl) != sch$gq_colpgwq) {
	proc_count++;
    }

    ktb = sch$gq_pfwq;
    while ((ktb = ktb->ktb$l_sqfl) != sch$gq_pfwq) {
	proc_count++;
    }

    ktb = sch$gq_fpgwq;
    while ((ktb = ktb->ktb$l_sqfl) != sch$gq_fpgwq) {
	proc_count++;
    }

    /* check active CPUs for running processes and their priorities */
    uint32_t lowest_pri = UINT32_MAX;	/* first active CPU will replace this */

    /* TODO: support >64 CPUs using CBB instead of bitmask */
    const uint64_t cpu_bitmask = (smp$gq_active_set & ~(sch$gq_idle_cpus));
    const int max_cpuid = (smp$gl_max_cpuid > 63 ? 63 : smp$gl_max_cpuid);

    uint64_t testmask = 0x01;
    for (int cpuid = 0; cpuid <= max_cpuid; cpuid++, testmask <<= 1) {
	if (cpu_bitmask & testmask) {
	    /* assume this is non-NULL; otherwise, something's very wrong */
	    const CPU *cpu = smp$gl_cpu_data[cpuid];

	    /* priority will be -1 if we're not running a kernel thread.
	     * Skip this CPU if the idle loop is trying to acquire SCHED.
	     */
	    if (cpu->cpu$l_cur_pri != UINT32_MAX && !(cpu->cpu$v_sched)) {
		proc_count++;

		/* invert internal priority by subtracting from 63 */
		uint32_t cur_pri = (63 - cpu->cpu$l_cur_pri);
		if (cur_pri < lowest_pri) {
		    lowest_pri = cur_pri;
		}
	    }
	}
    }

    /* get the sum of all disk queue lengths */
    uint32_t disk_queue_len = UINT32_MAX;
    UCB* cur_ucb = NULL;
    DDB* cur_ddb = NULL;

    /* skip disk queue length update if we can't lock the IOC database */
    if (!$VMS_STATUS_SUCCESS(sch_std$lockrexec_quad(&ioc$gq_mutex))) {
	goto skip_scan_queue_len;
    }

    disk_queue_len = 0;	    /* distinguish between 0 and failure to lock mutex */

    /* this will return low bit clear when there are no more devices */
    while ($VMS_STATUS_SUCCESS(
	    ioc_std$scan_iodb(cur_ucb, cur_ddb, &cur_ucb, &cur_ddb))) {
	/* is this a mounted disk and not a class driver or shadow set member? */
	if ((cur_ucb->ucb$b_devclass == DC$_DISK) &&
		(cur_ucb->ucb$l_devchar & DEV$M_MNT) &&
		!(cur_ucb->ucb$l_devchar2 & (DEV$M_CDP | DEV$M_SSM)) &&
		(cur_ucb->ucb$l_qlen > 0)) {
	    disk_queue_len += cur_ucb->ucb$l_qlen;
	}
    }

    /* lock and unlock mutex need to acquire SCHED, so do both before releasing it */
    sch_std$unlockexec_quad(&ioc$gq_mutex);

skip_scan_queue_len:

    /* release the SCHED spinlock */
    sys_unlock (SCHED, orig_ipl, SMP_RESTORE);

    /* acquire the UCB device lock, raising IPL, saving previous IPL */
    device_lock (ucb->ucb$r_ucb.ucb$l_dlck, RAISE_IPL, &orig_ipl);

    /* bail out now if the user asked us to stop updating */
    if (ucb->ucb$b_is_stopping) {
	ucb->ucb$b_is_stopping = false;
	ucb->ucb$b_is_stopped = true;

	/* all 0 bits is +0.0 in IEEE-754 */
    	memset(&(ucb->ucb$fx_avgs), 0, sizeof(ucb->ucb$fx_avgs));

	/* cancel the timer */
	tqe->tqe$b_rqtype = 0;

	goto unlock;	/* release device lock and return */
    }

    proc_count <<= FX_LSHIFT;
    ucb->ucb$fx_avgs[0] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[0]) * old_lav_1min) +
				((uint64_t)proc_count * new_lav_1min)) >> FX_RSHIFT);

    ucb->ucb$fx_avgs[1] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[1]) * old_lav_5min) +
				((uint64_t)proc_count * new_lav_5min)) >> FX_RSHIFT);

    ucb->ucb$fx_avgs[2] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[2]) * old_lav_15min) +
				((uint64_t)proc_count * new_lav_15min)) >> FX_RSHIFT);

    if (lowest_pri == UINT32_MAX) {
	lowest_pri = 0;	    /* no CPUs are running processes */
    } else {
	lowest_pri <<= FX_LSHIFT;
    }
    ucb->ucb$fx_avgs[3] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[3]) * old_lav_1min) +
				((uint64_t)lowest_pri * new_lav_1min)) >> FX_RSHIFT);

    ucb->ucb$fx_avgs[4] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[4]) * old_lav_5min) +
				((uint64_t)lowest_pri * new_lav_5min)) >> FX_RSHIFT);

    ucb->ucb$fx_avgs[5] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[5]) * old_lav_15min) +
				((uint64_t)lowest_pri * new_lav_15min)) >> FX_RSHIFT);

    /* skip this section if we failed to lock the IOC database mutex */
    if (disk_queue_len != UINT32_MAX) {
	disk_queue_len <<= FX_LSHIFT;

	ucb->ucb$fx_avgs[6] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[6]) * old_lav_1min) +
				    ((uint64_t)disk_queue_len * new_lav_1min)) >> FX_RSHIFT);

	ucb->ucb$fx_avgs[7] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[7]) * old_lav_5min) +
				    ((uint64_t)disk_queue_len * new_lav_5min)) >> FX_RSHIFT);

	ucb->ucb$fx_avgs[8] = (uint32_t)(((((uint64_t)ucb->ucb$fx_avgs[8]) * old_lav_15min) +
				    ((uint64_t)disk_queue_len * new_lav_15min)) >> FX_RSHIFT);
    }

unlock:

    /* Release the UCB device lock, returning to the previous IPL */
    device_unlock (ucb->ucb$r_ucb.ucb$l_dlck, orig_ipl, SMP_RESTORE);
}
