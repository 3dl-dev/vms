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
 *   SYS$INPUT     -> /dev/stdin   (I/O device — no VMS equivalent yet)
 *   SYS$OUTPUT    -> /dev/stdout
 *   SYS$ERROR     -> /dev/stderr
 *   SYS$COMMAND   -> /dev/stdin
 *   TT            -> /dev/tty
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
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSDEVICE", "DKA0:",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * System directory logicals — VMS-native equivalences.
     * These map standard logical names to ODS-2 directory specs
     * on the system device.
     */

    /* SYS$SYSTEM -> [SYS0.SYSCOMMON.SYSEXE] */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSTEM",
               "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSEXE]",
               0, LNM_MODE_EXEC);

    /* SYS$LIBRARY -> [SYS0.SYSCOMMON.SYSLIB] */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$LIBRARY",
               "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSLIB]",
               0, LNM_MODE_EXEC);

    /* SYS$SHARE -> [SYS0.SYSCOMMON.SYSLIB] (same as SYS$LIBRARY on VMS) */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SHARE",
               "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSLIB]",
               0, LNM_MODE_EXEC);

    /* SYS$MANAGER -> [SYS0.SYSCOMMON.SYSMGR] */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$MANAGER",
               "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSMGR]",
               0, LNM_MODE_EXEC);

    /* SYS$HELP -> [SYS0.SYSCOMMON.SYSHLP] */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$HELP",
               "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSHLP]",
               0, LNM_MODE_EXEC);

    /* SYS$SCRATCH -> system temp directory */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SCRATCH",
               "SYS$SYSDEVICE:[SYSTMP]",
               0, LNM_MODE_EXEC);

    /*
     * Per-user logicals — defaults in SYSTEM table,
     * overridden per-process by login.
     */

    /* SYS$LOGIN -> default user area (overridden per-process) */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$LOGIN",
               "SYS$SYSDEVICE:[USERS]",
               0, LNM_MODE_EXEC);

    /* SYS$DISK -> system device (process default device) */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$DISK", "SYS$SYSDEVICE",
               0, LNM_MODE_EXEC);

    /*
     * I/O channel logicals.
     * On a real VMS system these would be device names (TTA0:, etc.).
     * These remain as Linux device paths until we have a terminal
     * device driver layer.
     */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$INPUT", "/dev/stdin",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$OUTPUT", "/dev/stdout",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$ERROR", "/dev/stderr",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$COMMAND", "/dev/stdin",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* TT -> terminal device */
    const char *tty = ttyname(STDIN_FILENO);
    if (!tty)
        tty = "/dev/tty";
    lnm_create(mgr, LNM_SYSTEM_TABLE, "TT", tty,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
}
