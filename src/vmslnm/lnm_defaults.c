/*
 * lnm_defaults.c - Default VMS System Logical Names
 *
 * Sets up the standard VMS logical names that every system needs.
 * These correspond to the logicals defined by SYSGEN on a real
 * VMS system and are placed into the SYSTEM table.
 *
 * Equivalence strings use the ODS-2 directory layout under the
 * system disk mount point, matching real VMS conventions.
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
 * @vms_root: Root directory for VMS file tree (mount point)
 *
 * Creates the following logicals in the SYSTEM table:
 *   SYS$SYSDEVICE -> {vms_root}   (system disk mount point)
 *   SYS$SYSTEM    -> {vms_root}/SYS0/SYSCOMMON/SYSEXE
 *   SYS$LIBRARY   -> {vms_root}/SYS0/SYSCOMMON/SYSLIB
 *   SYS$SHARE     -> {vms_root}/SYS0/SYSCOMMON/SYSLIB
 *   SYS$MANAGER   -> {vms_root}/SYS0/SYSCOMMON/SYSMGR
 *   SYS$HELP      -> {vms_root}/SYS0/SYSCOMMON/SYSHLP
 *   SYS$SCRATCH   -> /tmp
 *   SYS$LOGIN     -> user's home directory
 *   SYS$DISK      -> {vms_root}  (process default device = system disk)
 *   SYS$INPUT     -> /dev/stdin
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

    char path[PATH_MAX];

    /*
     * System disk device logical.
     * SYS$SYSDEVICE is the system disk — everything derives from here.
     */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSDEVICE", vms_root,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * System directory logicals — ODS-2 layout.
     * These map VMS standard logical names to their on-disk locations
     * following the [SYS0.SYSCOMMON.*] hierarchy.
     */

    /* SYS$SYSTEM -> [SYS0.SYSCOMMON.SYSEXE] */
    snprintf(path, sizeof(path), "%s/SYS0/SYSCOMMON/SYSEXE", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSTEM", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$LIBRARY -> [SYS0.SYSCOMMON.SYSLIB] */
    snprintf(path, sizeof(path), "%s/SYS0/SYSCOMMON/SYSLIB", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$LIBRARY", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$SHARE -> [SYS0.SYSCOMMON.SYSLIB] (same as SYS$LIBRARY on VMS) */
    snprintf(path, sizeof(path), "%s/SYS0/SYSCOMMON/SYSLIB", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SHARE", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$MANAGER -> [SYS0.SYSCOMMON.SYSMGR] */
    snprintf(path, sizeof(path), "%s/SYS0/SYSCOMMON/SYSMGR", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$MANAGER", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$HELP -> [SYS0.SYSCOMMON.SYSHLP] */
    snprintf(path, sizeof(path), "%s/SYS0/SYSCOMMON/SYSHLP", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$HELP", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$SCRATCH -> /tmp (will move to system disk in future) */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SCRATCH", "/tmp",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * Per-user logicals — defaults in SYSTEM table,
     * overridden per-process by login.
     */

    /* SYS$LOGIN -> user's home directory */
    struct passwd *pw = getpwuid(getuid());
    const char *home = pw ? pw->pw_dir : "/tmp";
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$LOGIN", home,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$DISK -> system disk (process default device) */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$DISK", vms_root,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * I/O channel logicals.
     * On a real VMS system these would be device names (TTA0:, etc.);
     * here they map to Linux device paths.
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
