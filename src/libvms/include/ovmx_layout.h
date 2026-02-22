/*
 * ovmx_layout.h - VMS System Disk Directory Layout
 *
 * Defines the ODS-2 directory structure of the OVMX system disk.
 * This is the SINGLE place where the mapping between VMS directories
 * and their backing-store paths is defined.
 *
 * The mount point (SYSDISK_MOUNT) is the only Unix path constant.
 * Everything below it follows VMS ODS-2 conventions.
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
 */

#ifndef __OVMX_LAYOUT_H
#define __OVMX_LAYOUT_H

/* System disk mount point — the ONE Unix path in the system */
#define SYSDISK_MOUNT    "/vms"

/* ODS-2 directory tree */
#define VMS_SYSROOT      SYSDISK_MOUNT "/SYS0/SYSCOMMON"
#define VMS_SYSEXE       VMS_SYSROOT "/SYSEXE"
#define VMS_SYSLIB       VMS_SYSROOT "/SYSLIB"
#define VMS_SYSMGR       VMS_SYSROOT "/SYSMGR"
#define VMS_SYSHLP       VMS_SYSROOT "/SYSHLP"
#define VMS_USERS        SYSDISK_MOUNT "/USERS"

/* Standard VMS system files at their VMS-canonical locations */
#define VMS_SYSUAF_PATH      VMS_SYSEXE "/SYSUAF.DAT"
#define VMS_LASTLOGIN_DIR    VMS_SYSMGR "/LASTLOGIN"
#define VMS_OPERATOR_LOG     VMS_SYSMGR "/OPERATOR.LOG"
#define VMS_SYLOGIN_PATH     VMS_SYSMGR "/SYLOGIN.COM"
#define VMS_STARTUP_PATH     VMS_SYSMGR "/STARTUP.COM"
#define VMS_HELPLIB_PATH     VMS_SYSHLP "/HELPLIB.HLP"
#define VMS_LNM_CONF_PATH    VMS_SYSMGR "/SYLOGICALS.CONF"
#define VMS_SSH_HOST_KEY     VMS_SYSMGR "/SSH_HOST_RSA_KEY"

/* Executables */
#define VMS_DCL_PATH         VMS_SYSEXE "/DCL.EXE"
#define VMS_LOGINOUT_PATH    VMS_SYSEXE "/LOGINOUT.EXE"
#define VMS_LNMD_PATH        VMS_SYSEXE "/VMSLNMD.EXE"
#define VMS_SSHD_PATH        VMS_SYSEXE "/VMSSSHD.EXE"
#define VMS_AUTHORIZE_PATH   VMS_SYSEXE "/AUTHORIZE.EXE"
#define VMS_INITIALIZE_PATH  VMS_SYSEXE "/INITIALIZE.EXE"

#endif /* __OVMX_LAYOUT_H */
