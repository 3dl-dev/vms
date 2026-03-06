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
$! Welcome banner
$ WRITE SYS$OUTPUT ""
$ WRITE SYS$OUTPUT "    OVMX V1.0 -- OpenVMS-Compatible Environment for Linux"
$ WRITE SYS$OUTPUT ""
$!
$ EXIT
