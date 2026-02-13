/*
 * lnm_defaults.c - Default VMS System Logical Names
 *
 * Sets up the standard VMS logical names that every system needs.
 * These correspond to the logicals defined by SYSGEN on a real
 * VMS system and are placed into the SYSTEM table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>

#include "vms/logical.h"
#include "ssdef.h"

/*
 * lnm_setup_defaults - Set up standard VMS system logicals.
 *
 * @mgr:      Manager instance
 * @vms_root: Root directory for VMS file tree (e.g., "/vms")
 *
 * Creates the following logicals in the SYSTEM table:
 *   SYS$SYSTEM   -> {vms_root}/sys$system
 *   SYS$LIBRARY  -> {vms_root}/sys$library
 *   SYS$MANAGER  -> {vms_root}/sys$manager
 *   SYS$HELP     -> {vms_root}/sys$help
 *   SYS$SCRATCH  -> /tmp
 *   SYS$LOGIN    -> user's home directory
 *   SYS$DISK     -> current working directory's device
 *   SYS$INPUT    -> /dev/stdin   (conceptual)
 *   SYS$OUTPUT   -> /dev/stdout
 *   SYS$ERROR    -> /dev/stderr
 *   SYS$COMMAND  -> /dev/stdin
 *   TT           -> /dev/tty
 */
void lnm_setup_defaults(lnm_manager_t *mgr, const char *vms_root)
{
    if (!mgr)
        return;

    if (!vms_root)
        vms_root = "/vms";

    char path[PATH_MAX];

    /*
     * System directory logicals -- these go into LNM$SYSTEM.
     * They are marked TERMINAL so iterative translation stops here.
     */

    /* SYS$SYSTEM -> {vms_root}/sys$system */
    snprintf(path, sizeof(path), "%s/sys$system", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SYSTEM", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$LIBRARY -> {vms_root}/sys$library */
    snprintf(path, sizeof(path), "%s/sys$library", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$LIBRARY", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$MANAGER -> {vms_root}/sys$manager */
    snprintf(path, sizeof(path), "%s/sys$manager", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$MANAGER", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$HELP -> {vms_root}/sys$help */
    snprintf(path, sizeof(path), "%s/sys$help", vms_root);
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$HELP", path,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$SCRATCH -> /tmp */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$SCRATCH", "/tmp",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /*
     * Per-user logicals -- still in the SYSTEM table as defaults,
     * but individual processes can override them in the PROCESS table.
     */

    /* SYS$LOGIN -> user's home directory */
    struct passwd *pw = getpwuid(getuid());
    const char *home = pw ? pw->pw_dir : "/tmp";
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$LOGIN", home,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$DISK -> current working directory */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$DISK", cwd,
                   LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    } else {
        lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$DISK", "/",
                   LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
    }

    /*
     * I/O channel logicals.
     * On a real VMS system these would be device names; here we
     * map them to Linux device paths.
     */

    /* SYS$INPUT -> /dev/stdin (conceptual) */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$INPUT", "/dev/stdin",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$OUTPUT -> /dev/stdout */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$OUTPUT", "/dev/stdout",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$ERROR -> /dev/stderr */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$ERROR", "/dev/stderr",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* SYS$COMMAND -> /dev/stdin */
    lnm_create(mgr, LNM_SYSTEM_TABLE, "SYS$COMMAND", "/dev/stdin",
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);

    /* TT -> /dev/tty (terminal device) */
    const char *tty = ttyname(STDIN_FILENO);
    if (!tty)
        tty = "/dev/tty";
    lnm_create(mgr, LNM_SYSTEM_TABLE, "TT", tty,
               LNM_ATTR_TERMINAL, LNM_MODE_EXEC);
}
