$! STARTUP.COM - OVMX System Startup Command Procedure
$!
$! Runs after kernel modules and system disk mount. There is no logical
$! name daemon to wait for: on VMS the logical name tables are
$! executive-resident, not a service (vms-a4b) -- see the NOTE ON
$! SERVICES in src/ovmx_init/ovmx_init.c.
$! Edits to this file on the system disk persist across reboots.
$!
$! WHAT RUNS THIS PROCEDURE, AND UNDER WHOSE IDENTITY (vms-9b7).
$!
$! PID 1 (STARTUP.EXE) execs SYS$SYSTEM:PROVISION.EXE, which:
$!   1. reads SYSUAF's SYSTEM record -- through libvms's ONE reader;
$!   2. asks the EXECUTIVE to stamp that identity on itself, and halts the
$!      boot if there is no SYSTEM record or the executive refuses;
$!   3. provisions the accounts' home directories and the system tree's
$!      ownership;
$!   4. execs DCL.EXE on THIS FILE, in the SAME process.
$!
$! So this procedure -- and SYSTARTUP_VMS.COM after it -- runs under SYSTEM's
$! identity as the executive recorded it, which is what OpenVMS does: STARTUP
$! runs under username SYSTEM.
$!
$! PID 1 DOES NOT READ SYSUAF.DAT and must not start. It is statically linked,
$! so when it did, it carried two hand-rolled 512-byte parsers of its own --
$! two of five independent parsers of one format, whose disagreement over the
$! line limit could leave the system with no readable SYSTEM record and no
$! boot. Account provisioning and system identity are system management, and
$! system management belongs on this path.
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
$! SYS$UPDATE -- [SYS0.SYSCOMMON.SYSUPD], the standard VMS home for
$! pre-PCSI layered-product install kits and their SETUP.COM procedures
$! (e.g. SYS$UPDATE:PARTS_SETUP.COM, vms-977/vms-2579). Not one of the
$! logicals lnm_setup_defaults() hardcodes (src/vmslnm/lnm_defaults.c) --
$! defined here, the same way SYS$STARTUP/SYS$LOGIN above are, rather than
$! adding to that baseline table.
$ DEFINE/SYSTEM SYS$UPDATE SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSUPD]
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
