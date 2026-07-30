/*
 * ovmx_layout.h - VMS System Disk Layout (VMS-Native Filespecs)
 *
 * All constants are VMS filespecs — no Linux paths except the single
 * SYSDISK_MOUNT used to bootstrap the device table.
 *
 * Consumers call vmsfs_to_linux_path() at the point of each syscall.
 * The device table (DKA0: → mount point) is populated at boot by
 * STARTUP.EXE and at session start by DCL.
 *
 * VMS Directory Hierarchy:
 *   [000000]                        Master File Directory (MFD)
 *   [SYS0]                          System root
 *   [SYS0.SYSCOMMON]                Common system tree
 *   [SYS0.SYSCOMMON.SYSEXE]        SYS$SYSTEM — executables, SYSUAF
 *   [SYS0.SYSCOMMON.SYSLIB]        SYS$LIBRARY/SYS$SHARE — libraries
 *   [SYS0.SYSCOMMON.SYSMGR]        SYS$MANAGER — admin configs
 *   [SYS0.SYSCOMMON.SYSHLP]        SYS$HELP — help files
 *   [USERS]                         User home directories
 *   [SYSTMP]                        Scratch / temporary files
 */

#ifndef __OVMX_LAYOUT_H
#define __OVMX_LAYOUT_H

/*
 * System disk mount point — the ONE Linux path in the entire system.
 * Used only to bootstrap the device table: DKA0: → SYSDISK_MOUNT.
 */
#define SYSDISK_MOUNT    "/vms"

/*
 * System disk device name.
 * Matches the device table entry that maps to SYSDISK_MOUNT.
 */
#define SYSDISK_DEVICE   "DKA0"

/* ------------------------------------------------------------------ */
/* VMS directory filespecs                                            */
/* ------------------------------------------------------------------ */

#define VMS_MFD          SYSDISK_DEVICE ":[000000]"
#define VMS_SYSROOT      SYSDISK_DEVICE ":[SYS0.SYSCOMMON]"
#define VMS_SYSEXE       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSEXE]"
#define VMS_SYSLIB       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSLIB]"
#define VMS_SYSMGR       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSMGR]"
#define VMS_SYSHLP       SYSDISK_DEVICE ":[SYS0.SYSCOMMON.SYSHLP]"
#define VMS_USERS        SYSDISK_DEVICE ":[USERS]"
#define VMS_SYSTMP       SYSDISK_DEVICE ":[SYSTMP]"

/* ------------------------------------------------------------------ */
/* VMS filespecs for well-known system files                          */
/* ------------------------------------------------------------------ */

#define VMS_SYSUAF_PATH      "SYS$SYSTEM:SYSUAF.DAT"
#define VMS_LASTLOGIN_DIR    "SYS$MANAGER:LASTLOGIN"
#define VMS_OPERATOR_LOG     "SYS$MANAGER:OPERATOR.LOG"
#define VMS_SYLOGIN_PATH     "SYS$MANAGER:SYLOGIN.COM"
#define VMS_STARTUP_PATH     "SYS$MANAGER:STARTUP.COM"
#define VMS_HELPLIB_PATH     "SYS$HELP:HELPLIB.HLP"
#define VMS_LNM_CONF_PATH    "SYS$MANAGER:SYLOGICALS.CONF"
#define VMS_SSH_HOST_KEY     "SYS$MANAGER:SSH_HOST_RSA_KEY"

/* ------------------------------------------------------------------ */
/* VMS filespecs for executables                                      */
/* ------------------------------------------------------------------ */

#define VMS_DCL_PATH         "SYS$SYSTEM:DCL.EXE"
#define VMS_LOGINOUT_PATH    "SYS$SYSTEM:LOGINOUT.EXE"
#define VMS_SSHD_PATH        "SYS$SYSTEM:VMSSSHD.EXE"
#define VMS_AUTHORIZE_PATH   "SYS$SYSTEM:AUTHORIZE.EXE"
#define VMS_INITIALIZE_PATH  "SYS$SYSTEM:INITIALIZE.EXE"

/* ------------------------------------------------------------------ */
/* VMS filespecs for data files                                       */
/* ------------------------------------------------------------------ */

#define VMS_RIGHTSLIST_PATH   "SYS$SYSTEM:RIGHTSLIST.DAT"
#define VMS_QMAN_DB          "SYS$MANAGER:QMAN$MASTER.DAT"

/* ------------------------------------------------------------------ */
/* Linux paths — derived from SYSDISK_MOUNT                           */
/*                                                                     */
/* Use these ONLY in code that runs before the VMS filespec translator */
/* is available (ovmx_init, early boot) or in contexts that need raw   */
/* Linux paths (execv, access, stat).  Prefer VMS filespecs + runtime */
/* translation everywhere else.                                        */
/* ------------------------------------------------------------------ */

#define VMS_SYSTEM_DIR   SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE"
#define VMS_LIBRARY_DIR  SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSLIB"
#define VMS_MANAGER_DIR  SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSMGR"
#define VMS_HELP_DIR     SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSHLP"
#define VMS_USERS_DIR    SYSDISK_MOUNT "/USERS"
#define VMS_TEMP_DIR     SYSDISK_MOUNT "/SYSTMP"

#endif /* __OVMX_LAYOUT_H */
