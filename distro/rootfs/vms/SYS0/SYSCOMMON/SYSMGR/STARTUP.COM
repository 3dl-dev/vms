$! STARTUP.COM - OVMX System Startup Command Procedure
$!
$! Executed by STARTUP.EXE during boot from SYS$MANAGER:STARTUP.COM
$! Runs after kernel modules, system disk mount, and LNM daemon start.
$! Edits to this file on the system disk persist across reboots.
$!
$ SET NOON
$!
$! Display system time
$ SHOW TIME
$!
$! Define system-wide logical names
$ DEFINE/SYSTEM SYS$STARTUP SYS$MANAGER
$ DEFINE/SYSTEM SYS$LOGIN SYS$MANAGER
$!
$! Login banners are NOT set here -- LOGINOUT reads SYS$ANNOUNCE and
$! SYS$WELCOME, which a manager defines in SYS$MANAGER:SYLOGICALS.CONF.
$! The startup banner itself is displayed by STARTUP.EXE from the system
$! identity module, so there is exactly one place that knows the version.
$!
$! Site-specific startup. Services (SSH and anything a manager adds) are
$! started from SYSTARTUP_VMS.COM as detached processes -- never from
$! STARTUP.EXE, which starts no services at all.
$ IF F$SEARCH("SYS$MANAGER:SYSTARTUP_VMS.COM") .NES. "" THEN @SYS$MANAGER:SYSTARTUP_VMS.COM
$!
$ EXIT
