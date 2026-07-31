$! STARTUP.COM - OVMX System Startup Command Procedure
$!
$! Executed by STARTUP.EXE during boot from SYS$MANAGER:STARTUP.COM
$! Runs after kernel modules and system disk mount. There is no logical
$! name daemon to wait for: on VMS the logical name tables are
$! executive-resident, not a service (vms-a4b) -- see the NOTE ON
$! SERVICES in src/ovmx_init/ovmx_init.c.
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
$! Site-specific startup. This is where services are started, and the
$! only place: STARTUP.EXE (PID 1) starts none of its own. Invoked
$! unconditionally and last, after the system logical names exist -- if
$! the procedure is missing, DCL says so rather than skipping it
$! silently.
$ @SYS$MANAGER:SYSTARTUP_VMS.COM
$!
$ EXIT
