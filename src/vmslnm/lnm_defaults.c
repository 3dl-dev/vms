/*
 * lnm_defaults.c - Default VMS System Logical Names
 *
 * Sets up the standard VMS logical names that every system needs.
 * These correspond to the logicals defined by SYSGEN on a real
 * VMS system and are placed into the SYSTEM table.
 *
 * All equivalence strings use VMS notation (device:[directory] specs),
 * never Linux paths.  The single mapping from VMS device names to
 * Linux mount points lives in the device table (vmsfs_device_add).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>

#include "vms/logical.h"
#include "ovmx_layout.h"
#include "ssdef.h"

/*
 * lnm_seed_system_locating - Define a SYSTEM logical, with a host-tooling
 * process-scope fallback (vms-48ab, INV-6-preserving).
 *
 * At OVMX runtime, PID 1 pins /dev/vms (Rule 9), so the LNM$SYSTEM create
 * below reaches the executive and that is the whole story: every process on
 * the node sees the SAME system-wide value, exactly as VMS requires, and
 * this function does nothing further.
 *
 * Under host BUILD/TEST tooling there is no executive, so vms-48ab's
 * SS$_NOSUCHDEV comes back honestly. THIS FUNCTION DOES NOT ADD A FALLBACK
 * TO THE REMOVED ONE: lnm_create/lnm_translate/lnm_delete against
 * LNM$SYSTEM itself still have none, so an explicit LNM$SYSTEM lookup
 * (SHOW LOGICAL/SYSTEM, F$TRNLNM(name,"LNM$SYSTEM")) still honestly reports
 * SS$_NOSUCHDEV -- see tests/vmslnm/test_vmslnm.c's
 * test_system_table_no_fallback() and tests/qemu/test_syssvc_lnm_system.c's
 * no-executive branch, neither of which this function touches.
 *
 * What it does instead, only on that SS$_NOSUCHDEV, is define the SAME name
 * with the SAME value in LNM$PROCESS_TABLE -- a real, disclosed, per-process
 * VMS logical, not a disguise of the SYSTEM table (SHOW LOGICAL reports it
 * as "(LNM$PROCESS_TABLE)", never as SYSTEM or shared). This is exactly the
 * thing a sysadmin can always do on real VMS -- a process-level DEFINE
 * shadows a system-level one -- so host tooling (ctest binaries, LOGINOUT.
 * EXE's own authenticate step) can still locate SYS$SYSTEM:SYSUAF.DAT,
 * SYS$SYSTEM:RIGHTSLIST.DAT, etc. through the default LNM$FILE_DEV search
 * order, which checks PROCESS before SYSTEM. It never fires when the
 * executive is present, so real multi-process SYSTEM-table semantics (a
 * sysadmin's system-wide DEFINE/SYSTEM taking effect for every OTHER
 * process) are completely unaffected.
 */
static void lnm_seed_system_locating(lnm_manager_t *mgr, const char *name,
                                     const char *value, uint32_t attr)
{
    uint32_t st = lnm_create(mgr, LNM_SYSTEM_TABLE, name, value,
                             attr, LNM_MODE_EXEC);
    if (st == SS$_NOSUCHDEV)
        lnm_create(mgr, LNM_PROCESS_TABLE, name, value, attr, LNM_MODE_EXEC);
}

/*
 * lnm_setup_defaults - Set up standard VMS system logicals.
 *
 * @mgr:      Manager instance
 * @vms_root: Root directory for VMS file tree (Linux mount point)
 *
 * First registers DKA0: in the device table (the ONE place a Unix
 * path is stored), then creates logicals with VMS equivalences:
 *
 *   SYS$SYSDEVICE -> DKA0:
 *   SYS$SYSTEM    -> SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]
 *   SYS$LIBRARY   -> SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSLIB]
 *   SYS$SHARE     -> SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSLIB]
 *   SYS$MANAGER   -> SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSMGR]
 *   SYS$HELP      -> SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSHLP]
 *   SYS$SCRATCH   -> SYS$SYSDEVICE:[SYSTMP]
 *   SYS$LOGIN     -> SYS$SYSDEVICE:[USERS]  (overridden per-process by login)
 *   SYS$DISK      -> SYS$SYSDEVICE  (process default device)
 *   TT            -> /dev/tty     (the ONE Linux path for terminal I/O)
 *   SYS$INPUT     -> TT:
 *   SYS$OUTPUT    -> TT:
 *   SYS$ERROR     -> TT:
 *   SYS$COMMAND   -> TT:
 */
void lnm_setup_defaults(lnm_manager_t *mgr, const char *vms_root)
{
    if (!mgr)
        return;

    if (!vms_root)
        vms_root = SYSDISK_MOUNT;

    /*
     * System disk device logical.
     * SYS$SYSDEVICE is the system disk — everything derives from here.
     */
    lnm_seed_system_locating(mgr, "SYS$SYSDEVICE", "DKA0:", LNM_ATTR_TERMINAL);

    /*
     * SYS$SYSROOT -> the system root directory [SYS0.] on the system disk.
     * On OpenVMS this is a rooted (concealed) device pointing at the running
     * system root; OVMX's layout puts the running root at SYS$SYSDEVICE:[SYS0.]
     * (VSI OpenVMS System Manager's Manual, system directory structure). The
     * SYS$xxx directory logicals below are the per-directory equivalences under
     * it, spelled out so a single translation resolves each in one step.
     */
    lnm_seed_system_locating(mgr, "SYS$SYSROOT", "SYS$SYSDEVICE:[SYS0.]", 0);

    /*
     * System directory logicals — VMS-native equivalences.
     * These map standard logical names to ODS-2 directory specs
     * on the system device.
     */

    /* SYS$SYSTEM -> [SYS0.SYSCOMMON.SYSEXE] */
    lnm_seed_system_locating(mgr, "SYS$SYSTEM",
                             "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]", 0);

    /* SYS$LIBRARY -> [SYS0.SYSCOMMON.SYSLIB] */
    lnm_seed_system_locating(mgr, "SYS$LIBRARY",
                             "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSLIB]", 0);

    /* SYS$SHARE -> [SYS0.SYSCOMMON.SYSLIB] (same as SYS$LIBRARY on VMS) */
    lnm_seed_system_locating(mgr, "SYS$SHARE",
                             "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSLIB]", 0);

    /* SYS$MANAGER -> [SYS0.SYSCOMMON.SYSMGR] */
    lnm_seed_system_locating(mgr, "SYS$MANAGER",
                             "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSMGR]", 0);

    /*
     * SYS$UPDATE -> [SYS0.SYSCOMMON.SYSUPD], the standard VMS home for pre-PCSI
     * layered-product install kits and their SETUP.COM procedures -- e.g.
     * @SYS$UPDATE:PARTS_SETUP.COM (vms-977/vms-2579, the 0.2 demo). Seeded into
     * the executive-resident SYSTEM table here so a login process sees it even
     * before SYS$MANAGER:STARTUP.COM's DEFINE/SYSTEM runs; STARTUP.COM then
     * (re)defines the same value, an idempotent supersede.
     */
    lnm_seed_system_locating(mgr, "SYS$UPDATE",
                             "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSUPD]", 0);

    /* SYS$HELP -> [SYS0.SYSCOMMON.SYSHLP] */
    lnm_seed_system_locating(mgr, "SYS$HELP",
                             "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSHLP]", 0);

    /* SYS$SCRATCH -> system temp directory */
    lnm_seed_system_locating(mgr, "SYS$SCRATCH", "SYS$SYSDEVICE:[SYSTMP]", 0);

    /*
     * Default queue logicals (vms-f89). On OpenVMS SYS$PRINT and SYS$BATCH are
     * LOGICAL NAMES for the default print/batch queues, not literal queue
     * names baked into PRINT/SUBMIT: a site does DEFINE/SYSTEM SYS$PRINT
     * <queue> to redirect where a bare PRINT (or SUBMIT) sends its job. Seeded
     * to the same-named default queue ensure_queue_init() creates
     * (src/vmsdcl/dcl_cmd_process.c); PRINT/SUBMIT translate the logical at
     * point of use, so a later DEFINE takes effect live.
     *
     * Doc: VSI OpenVMS DCL Dictionary, PRINT (/QUEUE default SYS$PRINT) and
     * SUBMIT (/QUEUE default SYS$BATCH); VSI OpenVMS System Manager's Manual,
     * Vol. 1, "Managing Queues" (SYS$PRINT / SYS$BATCH as the default queue
     * logical names). These are non-terminal so an equivalence that is itself
     * another logical still translates further, matching VMS.
     */
    lnm_seed_system_locating(mgr, "SYS$PRINT", "SYS$PRINT", 0);
    lnm_seed_system_locating(mgr, "SYS$BATCH", "SYS$BATCH", 0);

    /*
     * SYS$TIMEZONE_NAME / SYS$TIMEZONE_RULE / SYS$TIMEZONE_DIFFERENTIAL are
     * ALSO seeded from the live system TZ (vms-f89), but in the DCL session
     * setup (src/vmsdcl/dcl_main.c) rather than here: they need <time.h> (TDF
     * from the current zone), and this file is built by the native-link path
     * (src/vmslink/mk_vmslnm_shr.sh) with a freestanding CFLAGS set that does
     * not guarantee the libc time extensions. lib_datetime.c reads
     * SYS$TIMEZONE_DIFFERENTIAL at point of use.
     */

    /*
     * Per-user logicals — defaults in SYSTEM table,
     * overridden per-process by login.
     */

    /* SYS$LOGIN -> default user area (overridden per-process) */
    lnm_seed_system_locating(mgr, "SYS$LOGIN", "SYS$SYSDEVICE:[USERS]", 0);

    /*
     * SYS$DISK, the terminal and the I/O channel logicals are PER-PROCESS on
     * OpenVMS (LNM$PROCESS_TABLE): SYS$DISK is the process default device, and
     * SYS$INPUT/OUTPUT/ERROR/COMMAND point at THIS process's terminal, which
     * differs between jobs (an interactive session vs. a batch job reading a
     * command file). Seeding them into the process table is both correct and
     * required now that LNM$SYSTEM is executive-resident and node-wide: a
     * terminal logical placed in SYSTEM by one process would wrongly bind every
     * other process on the node to this process's /dev/tty.
     */

    /* SYS$DISK -> system device (process default device) */
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$DISK", "SYS$SYSDEVICE",
               0, LNM_MODE_EXEC);

    /*
     * Terminal device — the ONE Linux path for I/O.
     * On real VMS this would be the physical terminal (TTA0:, VTA123:).
     * All I/O channel logicals point here via "TT:".
     */
    const char *tty = ttyname(STDIN_FILENO);
    if (!tty)
        tty = "/dev/tty";
    lnm_create(mgr, LNM_PROCESS_TABLE, "TT", tty,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * I/O channel logicals — point at TT:, not Linux /dev/ paths.
     * On real VMS these are per-process and may differ (e.g. batch
     * jobs get SYS$INPUT pointing at a command file).  For now
     * they all resolve through TT: → /dev/tty.
     */
    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$INPUT", "TT:",
               0, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$OUTPUT", "TT:",
               0, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$ERROR", "TT:",
               0, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$COMMAND", "TT:",
               0, LNM_MODE_EXEC);
}

/*
 * lnm_define_login_logicals - per-user identity logicals from SYSUAF (vms-e48).
 *
 * WHAT THIS REPLACES: SYS$LOGIN/SYS$SCRATCH used to be Linux environment
 * variables -- vms_login setenv'd them and DCL getenv'd them -- so
 * F$TRNLNM("SYS$LOGIN") never saw the user's real home and returned the
 * generic SYS$SYSDEVICE:[USERS] default lnm_setup_defaults() seeds. These are
 * now REAL LNM$PROCESS logicals created through the same lnm_create() path
 * DEFINE uses, so translation is truthful.
 *
 * Contract and doc citations are on the declaration in vms/logical.h.
 */
/* lnm_create() reports a replaced name with SS$_SUPERSEDE (844), which is an
 * EVEN success code -- so plain (status & 1) would read a supersede as a
 * failure. SYS$LOGIN et al. deliberately supersede the generic defaults
 * lnm_setup_defaults() seeded, so both codes are success here (the same pair
 * dcl_translate_logical() accepts). */
#define LNM_LOGIN_OK(st)  ((st) == SS$_NORMAL || (st) == SS$_SUPERSEDE)

uint32_t lnm_define_login_logicals(lnm_manager_t *mgr, const char *table_name,
                                   const char *default_dir)
{
    if (!mgr || !table_name || !table_name[0] || !default_dir || !default_dir[0])
        return SS$_BADPARAM;

    /* SYS$LOGIN: the account's full default device + directory. Supersedes the
     * generic [USERS] default seeded elsewhere; the LNM$FILE_DEV search order
     * (PROCESS -> JOB -> GROUP -> SYSTEM) reaches this table before the SYSTEM
     * default. */
    uint32_t st = lnm_create(mgr, table_name, "SYS$LOGIN",
                             default_dir, 0, LNM_MODE_SUPER);
    if (!LNM_LOGIN_OK(st))
        return st;

    /* SYS$LOGIN_DEVICE: the device field of default_dir -- everything up to
     * and including the first ':'. Only defined when default_dir names a
     * device (it always does when it comes from a SYSUAF record). */
    const char *colon = strchr(default_dir, ':');
    if (colon) {
        size_t devlen = (size_t)(colon - default_dir) + 1;   /* keep the ':' */
        char device[LNM_MAX_VALUE + 1];
        if (devlen > LNM_MAX_VALUE)
            devlen = LNM_MAX_VALUE;
        memcpy(device, default_dir, devlen);
        device[devlen] = '\0';
        st = lnm_create(mgr, table_name, "SYS$LOGIN_DEVICE",
                        device, 0, LNM_MODE_SUPER);
        if (!LNM_LOGIN_OK(st))
            return st;
    }

    /*
     * SYS$SCRATCH IS DELIBERATELY NOT REDEFINED HERE (vms-e48 round 3).
     *
     * On OpenVMS SYS$SCRATCH defaults to the login directory (= SYS$LOGIN),
     * but a site may DEFINE it to a dedicated scratch area, and OVMX ships
     * exactly that: lnm_setup_defaults() defines SYS$SCRATCH system-wide to
     * SYS$SYSDEVICE:[SYSTMP] -- a directory the boot image masters
     * SYSTEM-writable (distro/Dockerfile.bootable; the vms-e5c/vms-221
     * executive fix that made SYS$SCRATCH: genuinely sys$create-able). Every
     * process on the node -- an interactive DCL and the images it activates
     * alike -- resolves that one system-wide value, so scratch files land in
     * one writable, consistent place.
     *
     * Overriding it per-login to = SYS$LOGIN would point it at each account's
     * home, which is NOT guaranteed to be an RMS-writable directory in the
     * booted image: measured against the PARTS 0.2 demo, a SYSTEM session's
     * sys$create in its own [SYSMGR] home returns RMS/EACCES (only [SYSTMP]/
     * [USERS] are mastered writable), so PARTS fell through to a fallback
     * candidate and DIRECTORY SYS$SCRATCH:PARTS.DAT then looked elsewhere.
     * Making per-account login directories RMS-writable is a vmsfs/provision
     * concern outside this login-logicals item (tracked separately). SYS$LOGIN
     * and SYS$LOGIN_DEVICE -- the identity this item is about -- are sourced
     * from SYSUAF above regardless; SYS$SCRATCH stays the system scratch.
     */
    (void)0;
    return st;
}
